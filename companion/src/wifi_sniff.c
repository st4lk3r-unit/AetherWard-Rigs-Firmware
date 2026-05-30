#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "aw_bus/aw_bus.h"
#include "aw_bus/aw_bus_types.h"
#include "aw_wifi/wifi_proto.h"
#include "board.h"
#include "wifi_sniff.h"

#define TAG "wifi-sniff"

/* ---- Double-buffer blob assembly ---- */
#define BLOB_BUF_SIZE        AWBUS_MAX_PAYLOAD
#define BLOB_FLUSH_THRESHOLD ((BLOB_BUF_SIZE * 3u) / 4u)

static uint8_t  s_blob[2][BLOB_BUF_SIZE];
static uint16_t s_blob_used[2];       /* bytes used for frame records after blob header */
static uint8_t  s_blob_nframes[2];
static uint32_t s_blob_first_seq[2];
static volatile int s_fill = 0;
static SemaphoreHandle_t s_mutex;

/* ---- Capture state ---- */
static volatile int s_running     = 0;
static volatile int s_flush_due   = 0;
static int64_t      s_ts_offset   = 0;   /* brain_us - local_us at last timesync */
static uint8_t      s_filter_mask = 0;
static volatile uint8_t s_cur_channel = 1;
static volatile int s_last_err = 0;

/* ---- Channel hopper ---- */
static uint8_t            s_channels[13];
static uint8_t            s_n_channels = 0;
static volatile uint8_t   s_ch_idx     = 0;
static esp_timer_handle_t s_hop_timer  = NULL;
static esp_timer_handle_t s_flush_timer = NULL;

/* ---- Core counters ---- */
static volatile uint32_t s_captured  = 0;  /* actually queued into blob buffer */
static volatile uint32_t s_dropped   = 0;
static volatile uint32_t s_sent      = 0;  /* handed to AWBUS push */
static volatile uint32_t s_hop_count = 0;
static volatile uint32_t s_frame_seq = 0;  /* increments for every accepted frame */
static volatile uint32_t s_blob_seq  = 0;

/* ---- Capture/filter counters ---- */
static volatile uint32_t s_seen_mgmt = 0;
static volatile uint32_t s_seen_data = 0;
static volatile uint32_t s_seen_ctrl = 0;
static volatile uint32_t s_seen_misc = 0;
static volatile uint32_t s_seen_short = 0;
static volatile uint32_t s_filtered = 0;
static volatile uint32_t s_kept_beacon = 0;
static volatile uint32_t s_kept_probe_req = 0;
static volatile uint32_t s_kept_probe_rsp = 0;
static volatile uint32_t s_kept_eapol = 0;

/* ---- Drop/pathology counters ---- */
static volatile uint32_t s_drop_mutex_busy = 0;
static volatile uint32_t s_drop_blob_full = 0;
static volatile uint32_t s_drop_record_limit = 0;
static volatile uint32_t s_drop_pending_busy = 0;
static volatile uint32_t s_drop_push_fail = 0;

/* ---- Blob/drain counters ---- */
static volatile uint32_t s_blob_flushes = 0;
static volatile uint32_t s_blob_popped = 0;
static volatile uint32_t s_blob_pending_seen = 0;
static volatile uint32_t s_channel_set_ok = 0;
static volatile uint32_t s_channel_set_err = 0;
static volatile uint32_t s_max_blob_bytes = 0;
static volatile uint32_t s_max_blob_frames = 0;
static volatile uint32_t s_max_fill_bytes = 0;
static volatile uint32_t s_max_fill_frames = 0;
static volatile uint32_t s_flush_timer_fires = 0;
static volatile uint32_t s_flush_full_fires = 0;
static volatile uint32_t s_flush_empty = 0;

/* ---- Pending blob staged for the next NULL poll ---- */
static volatile int s_blob_pending = 0;
/* A blob can be staged by the Wi-Fi RX callback, where we do not have the
 * awbus_t needed to raise READY.  The companion main loop sees this flag and
 * asserts READY on the next pass. */
static volatile int s_pending_needs_ready = 0;
static int          s_pending_idx = 0;
static uint16_t     s_pending_used = 0;
static uint8_t      s_pending_nframes = 0;

static inline void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8u);
}

static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static inline void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static inline void bump_drop(volatile uint32_t *why) {
    (*why)++;
    s_dropped++;
}

/* Seal the current fill blob and swap to the other buffer.
 * Caller must hold s_mutex and must ensure s_blob_pending == 0 and nf > 0.
 * Returns 1 when a pending blob was staged. */
static int seal_fill_locked(void) {
    int fi = s_fill;
    uint8_t nf = s_blob_nframes[fi];
    uint16_t used = s_blob_used[fi];
    if (nf == 0u || s_blob_pending) return 0;

    uint32_t bseq = ++s_blob_seq;
    uint32_t first_seq = s_blob_first_seq[fi];

    s_blob[fi][0] = WIFI_DATA_SNIFF_BLOB;
    s_blob[fi][1] = WIFI_PROTO_VERSION;
    s_blob[fi][2] = (uint8_t)AWBUS_NODE_ID;
    s_blob[fi][3] = nf;
    s_blob[fi][4] = s_cur_channel;
    s_blob[fi][5] = 0;
    s_blob[fi][6] = WIFI_BLOB_HDR_SIZE;
    s_blob[fi][7] = 0;
    wr32(s_blob[fi] + 8, bseq);
    wr32(s_blob[fi] + 12, first_seq);
    wr16(s_blob[fi] + 16, used);
    wr16(s_blob[fi] + 18, s_blob_used[fi ^ 1]);
    wr32(s_blob[fi] + 20, s_dropped);

    int next_idx = fi ^ 1;
    s_blob_used[next_idx] = 0;
    s_blob_nframes[next_idx] = 0;
    s_blob_first_seq[next_idx] = 0;
    s_fill = next_idx;

    s_pending_idx = fi;
    s_pending_used = used;
    s_pending_nframes = nf;
    s_blob_pending = 1;
    s_pending_needs_ready = 1;

    s_blob_flushes++;
    if ((uint32_t)(WIFI_BLOB_HDR_SIZE + used) > s_max_blob_bytes)
        s_max_blob_bytes = (uint32_t)(WIFI_BLOB_HDR_SIZE + used);
    if ((uint32_t)nf > s_max_blob_frames)
        s_max_blob_frames = nf;

    return 1;
}

static void reset_stats(void) {
    s_captured = s_dropped = s_sent = s_hop_count = 0;
    s_frame_seq = s_blob_seq = 0;
    s_seen_mgmt = s_seen_data = s_seen_ctrl = s_seen_misc = s_seen_short = 0;
    s_filtered = 0;
    s_kept_beacon = s_kept_probe_req = s_kept_probe_rsp = s_kept_eapol = 0;
    s_drop_mutex_busy = s_drop_blob_full = s_drop_record_limit = 0;
    s_drop_pending_busy = s_drop_push_fail = 0;
    s_blob_flushes = s_blob_popped = s_blob_pending_seen = 0;
    s_channel_set_ok = s_channel_set_err = 0;
    s_max_blob_bytes = s_max_blob_frames = s_max_fill_bytes = s_max_fill_frames = 0;
    s_flush_timer_fires = s_flush_full_fires = s_flush_empty = 0;
    s_last_err = 0;
}

static void flush_timer_cb(void *arg) {
    (void)arg;
    s_flush_due = 1;
    s_flush_timer_fires++;
}

static int set_channel_counted(uint8_t ch) {
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        s_cur_channel = ch;
        s_channel_set_ok++;
        s_last_err = 0;
        return 1;
    }
    s_channel_set_err++;
    s_last_err = (int)err;
    return 0;
}

static void hop_timer_cb(void *arg) {
    (void)arg;
    if (!s_running || s_n_channels == 0) return;
    s_ch_idx = (uint8_t)((s_ch_idx + 1u) % s_n_channels);
    set_channel_counted(s_channels[s_ch_idx]);
    s_hop_count++;
}

static int is_eapol_data(const uint8_t *frame, uint16_t frame_len, uint8_t fsubtype) {
    if (frame_len < 32u) return 0;
    uint8_t  fc1      = frame[1];
    uint16_t body_off = (fsubtype & 0x08u) ? 26u : 24u;
    if ((fc1 & 0x03u) == 0x03u) body_off += 6u;
    if (frame_len < (uint16_t)(body_off + 8u)) return 0;
    const uint8_t *llc = frame + body_off;
    return (llc[0] == 0xAAu && llc[1] == 0xAAu && llc[2] == 0x03u &&
            llc[3] == 0x00u && llc[4] == 0x00u && llc[5] == 0x00u &&
            llc[6] == 0x88u && llc[7] == 0x8Eu);
}

static void wifi_rx_cb(void *buf, wifi_promiscuous_pkt_type_t pkt_type) {
    if (!s_running) return;

    if (pkt_type == WIFI_PKT_MISC) {
        s_seen_misc++;
        return;
    }

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    uint16_t orig_len = (uint16_t)pkt->rx_ctrl.sig_len;

    /* ESP-IDF commonly includes FCS in sig_len; strip it when present. */
    uint16_t frame_len = orig_len;
    if (frame_len > 4u) frame_len -= 4u;
    if (frame_len < 4u) {
        s_seen_short++;
        return;
    }

    uint8_t fc0      = frame[0];
    uint8_t ftype    = (fc0 >> 2u) & 0x03u;
    uint8_t fsubtype = (fc0 >> 4u) & 0x0Fu;

    if (pkt_type == WIFI_PKT_MGMT) s_seen_mgmt++;
    else if (pkt_type == WIFI_PKT_DATA) s_seen_data++;
    else if (pkt_type == WIFI_PKT_CTRL) s_seen_ctrl++;
    else s_seen_misc++;

    int accept = 0;
    uint8_t rec_flags = 0;
    if (pkt_type == WIFI_PKT_MGMT && ftype == 0) {
        if ((s_filter_mask & WIFI_FILTER_BEACON) && fsubtype == 8) {
            accept = 1; s_kept_beacon++;
        } else if ((s_filter_mask & WIFI_FILTER_PROBE_REQ) && fsubtype == 4) {
            accept = 1; s_kept_probe_req++;
        } else if ((s_filter_mask & WIFI_FILTER_PROBE_RSP) && fsubtype == 5) {
            accept = 1; s_kept_probe_rsp++;
        }
    } else if (pkt_type == WIFI_PKT_DATA && (s_filter_mask & WIFI_FILTER_EAPOL)) {
        if (is_eapol_data(frame, frame_len, fsubtype)) {
            accept = 1;
            rec_flags |= WIFI_REC_F_EAPOL;
            s_kept_eapol++;
        }
    }

    if (!accept) {
        s_filtered++;
        return;
    }

    uint32_t frame_seq = ++s_frame_seq;
    uint16_t captured_len = frame_len;
    if (captured_len > (uint16_t)WIFI_SNIFF_SNAPLEN) {
        captured_len = (uint16_t)WIFI_SNIFF_SNAPLEN;
        rec_flags |= WIFI_REC_F_TRUNCATED;
    }

    uint64_t ts_us = (uint64_t)((int64_t)esp_timer_get_time() + s_ts_offset);
    int8_t   rssi  = (int8_t)pkt->rx_ctrl.rssi;
    uint8_t  ch    = (uint8_t)pkt->rx_ctrl.channel;
    uint16_t needed = (uint16_t)(WIFI_RECORD_HDR_SIZE + captured_len);

    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        bump_drop(&s_drop_mutex_busy);
        return;
    }

    if (WIFI_BLOB_HDR_SIZE + needed > BLOB_BUF_SIZE) {
        xSemaphoreGive(s_mutex);
        bump_drop(&s_drop_blob_full);
        return;
    }

    /* Try at most twice: if the active blob is full, seal/swap it and retry
     * this same frame in the fresh fill buffer.  Before this patch, the first
     * overflow simply dropped the frame, producing blobF == drop even when the
     * bus was otherwise healthy. */
    for (int attempt = 0; attempt < 2; attempt++) {
        int fi = s_fill;
        int rec_limit = (s_blob_nframes[fi] >= 255u);
        int no_room = (WIFI_BLOB_HDR_SIZE + s_blob_used[fi] + needed > BLOB_BUF_SIZE);

        if (rec_limit || no_room) {
            if (s_blob_nframes[fi] == 0u) {
                xSemaphoreGive(s_mutex);
                if (rec_limit) bump_drop(&s_drop_record_limit);
                else bump_drop(&s_drop_blob_full);
                return;
            }

            if (s_blob_pending) {
                xSemaphoreGive(s_mutex);
                bump_drop(&s_drop_pending_busy);
                return;
            }

            if (!seal_fill_locked()) {
                xSemaphoreGive(s_mutex);
                bump_drop(rec_limit ? &s_drop_record_limit : &s_drop_blob_full);
                return;
            }
            s_flush_full_fires++;
            continue;
        }

        if (s_blob_nframes[fi] == 0u)
            s_blob_first_seq[fi] = frame_seq;

        uint8_t *p = s_blob[fi] + WIFI_BLOB_HDR_SIZE + s_blob_used[fi];
        wr64(p + 0, ts_us);
        wr32(p + 8, frame_seq);
        wr16(p + 12, captured_len);
        wr16(p + 14, frame_len);
        p[16] = ch;
        p[17] = (uint8_t)rssi;
        p[18] = fc0;
        p[19] = rec_flags;
        memcpy(p + WIFI_RECORD_HDR_SIZE, frame, captured_len);

        s_blob_used[fi] = (uint16_t)(s_blob_used[fi] + needed);
        s_blob_nframes[fi]++;
        s_captured++;

        if ((uint32_t)s_blob_used[fi] > s_max_fill_bytes) s_max_fill_bytes = s_blob_used[fi];
        if ((uint32_t)s_blob_nframes[fi] > s_max_fill_frames) s_max_fill_frames = s_blob_nframes[fi];

        xSemaphoreGive(s_mutex);
        return;
    }

    xSemaphoreGive(s_mutex);
    bump_drop(&s_drop_blob_full);
}

void wifi_sniff_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t el = esp_event_loop_create_default();
    if (el != ESP_OK && el != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(el);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    const esp_timer_create_args_t flush_args = { .callback = flush_timer_cb, .name = "sniff-flush" };
    ESP_ERROR_CHECK(esp_timer_create(&flush_args, &s_flush_timer));

    const esp_timer_create_args_t hop_args = { .callback = hop_timer_cb, .name = "sniff-hop" };
    ESP_ERROR_CHECK(esp_timer_create(&hop_args, &s_hop_timer));

    ESP_LOGI(TAG, "WiFi sniff ready: awbus_payload=%u snaplen=%u",
             (unsigned)AWBUS_MAX_PAYLOAD, (unsigned)WIFI_SNIFF_SNAPLEN);
}

void wifi_sniff_timesync(uint64_t brain_us) {
    s_ts_offset = (int64_t)brain_us - (int64_t)esp_timer_get_time();
    ESP_LOGI(TAG, "timesync offset=%" PRId64 " us", (int64_t)s_ts_offset);
}

void wifi_sniff_start(const uint8_t *channels, uint8_t n_ch,
                      uint8_t filter_mask, uint16_t hop_period_ms) {
    if (s_running) wifi_sniff_stop();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_fill = 0;
    memset(s_blob_used, 0, sizeof s_blob_used);
    memset(s_blob_nframes, 0, sizeof s_blob_nframes);
    memset(s_blob_first_seq, 0, sizeof s_blob_first_seq);
    xSemaphoreGive(s_mutex);

    s_blob_pending = 0;
    s_pending_needs_ready = 0;
    s_pending_idx = 0;
    s_pending_used = 0;
    s_pending_nframes = 0;
    s_flush_due = 0;
    reset_stats();
    s_filter_mask = filter_mask;

    if (n_ch > 13u) n_ch = 13u;
    memcpy(s_channels, channels, n_ch);
    s_n_channels  = n_ch;
    s_ch_idx      = 0;
    s_cur_channel = (n_ch > 0u) ? channels[0] : 1u;

    wifi_promiscuous_filter_t flt = {0};
    if (filter_mask & (WIFI_FILTER_BEACON | WIFI_FILTER_PROBE_REQ | WIFI_FILTER_PROBE_RSP))
        flt.filter_mask |= WIFI_PROMIS_FILTER_MASK_MGMT;
    if (filter_mask & WIFI_FILTER_EAPOL)
        flt.filter_mask |= WIFI_PROMIS_FILTER_MASK_DATA;

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(wifi_rx_cb);
    set_channel_counted(s_cur_channel);
    esp_wifi_set_promiscuous(true);

    s_running = 1;
    esp_timer_start_periodic(s_flush_timer, 50000u);   /* 50 ms */
    if (n_ch > 1u && hop_period_ms > 0u)
        esp_timer_start_periodic(s_hop_timer, (uint64_t)hop_period_ms * 1000u);

    ESP_LOGI(TAG, "sniff start: filter=0x%02x ch_count=%u hop=%ums",
             filter_mask, (unsigned)n_ch, (unsigned)hop_period_ms);
}

void wifi_sniff_stop(void) {
    if (!s_running) return;
    s_running = 0;

    esp_timer_stop(s_flush_timer);
    esp_timer_stop(s_hop_timer);
    esp_wifi_set_promiscuous(false);
    s_flush_due = 1;   /* ask loop to seal remaining buffered frames */

    ESP_LOGI(TAG, "sniff stop: captured=%u dropped=%u sent=%u blob_seq=%u frame_seq=%u",
             (unsigned)s_captured, (unsigned)s_dropped, (unsigned)s_sent,
             (unsigned)s_blob_seq, (unsigned)s_frame_seq);
}

int wifi_sniff_flush(awbus_t *bus) {
    if (s_blob_pending) {
        s_blob_pending_seen++;
        if (s_pending_needs_ready) {
            s_pending_needs_ready = 0;
            awbus_companion_set_ready(bus, 1);
            return 1;
        }
        if (s_flush_due) s_drop_pending_busy++;
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int      fi       = s_fill;
    uint8_t  nf       = s_blob_nframes[fi];
    uint16_t used     = s_blob_used[fi];
    int      buf_full = (WIFI_BLOB_HDR_SIZE + used >= BLOB_FLUSH_THRESHOLD);

    if (buf_full) s_flush_full_fires++;
    if (nf == 0u) {
        if (s_flush_due) s_flush_empty++;
        s_flush_due = 0;
        xSemaphoreGive(s_mutex);
        return 0;
    }
    if (!s_flush_due && !buf_full) {
        xSemaphoreGive(s_mutex);
        return 0;
    }
    s_flush_due = 0;

    if (!seal_fill_locked()) {
        s_flush_empty++;
        xSemaphoreGive(s_mutex);
        return 0;
    }
    s_pending_needs_ready = 0;
    xSemaphoreGive(s_mutex);

    awbus_companion_set_ready(bus, 1);
    return 1;
}

int wifi_sniff_pop_pending(awbus_t *bus) {
    if (!s_blob_pending) return 0;
    s_blob_pending = 0;
    s_pending_needs_ready = 0;

    uint16_t total = (uint16_t)(WIFI_BLOB_HDR_SIZE + s_pending_used);
    int rc = awbus_companion_push(bus, AWBUS_CMD_DATA_PUSH,
                                  s_blob[s_pending_idx], total, NULL);
    if (rc != AWBUS_OK) {
        s_last_err = rc;
        s_drop_push_fail++;
        s_dropped += s_pending_nframes;
        return 0;
    }
    awbus_companion_set_ready(bus, 1);
    s_sent += s_pending_nframes;
    s_blob_popped++;
    return 1;
}

static void put_u32_counter(uint8_t *buf, unsigned idx, uint32_t val) {
    uint8_t *p = buf + WIFI_TELEM_HDR_SIZE + (idx * 4u);
    wr32(p, val);
}

uint16_t wifi_sniff_telemetry(uint8_t *buf) {
    memset(buf, 0, WIFI_TELEMETRY_SIZE);
    buf[0] = WIFI_DATA_TELEMETRY;
    buf[1] = WIFI_PROTO_VERSION;
    buf[2] = (uint8_t)AWBUS_NODE_ID;
    buf[3] = s_cur_channel;
    buf[4] = (uint8_t)(s_running ? 1u : 0u);
    buf[5] = (uint8_t)(s_blob_pending ? 1u : 0u);
    buf[6] = (uint8_t)s_fill;
    buf[7] = (uint8_t)(s_last_err & 0xFF);

    put_u32_counter(buf, WIFI_TELEM_U32_LOCAL_MS, (uint32_t)((uint64_t)esp_timer_get_time() / 1000u));
    put_u32_counter(buf, WIFI_TELEM_U32_CAPTURED, s_captured);
    put_u32_counter(buf, WIFI_TELEM_U32_SENT, s_sent);
    put_u32_counter(buf, WIFI_TELEM_U32_DROP_TOTAL, s_dropped);
    put_u32_counter(buf, WIFI_TELEM_U32_SEEN_MGMT, s_seen_mgmt);
    put_u32_counter(buf, WIFI_TELEM_U32_SEEN_DATA, s_seen_data);
    put_u32_counter(buf, WIFI_TELEM_U32_SEEN_CTRL, s_seen_ctrl);
    put_u32_counter(buf, WIFI_TELEM_U32_SEEN_MISC, s_seen_misc);
    put_u32_counter(buf, WIFI_TELEM_U32_SEEN_SHORT, s_seen_short);
    put_u32_counter(buf, WIFI_TELEM_U32_FILTERED, s_filtered);
    put_u32_counter(buf, WIFI_TELEM_U32_KEPT_BEACON, s_kept_beacon);
    put_u32_counter(buf, WIFI_TELEM_U32_KEPT_PROBE_REQ, s_kept_probe_req);
    put_u32_counter(buf, WIFI_TELEM_U32_KEPT_PROBE_RSP, s_kept_probe_rsp);
    put_u32_counter(buf, WIFI_TELEM_U32_KEPT_EAPOL, s_kept_eapol);
    put_u32_counter(buf, WIFI_TELEM_U32_DROP_MUTEX_BUSY, s_drop_mutex_busy);
    put_u32_counter(buf, WIFI_TELEM_U32_DROP_BLOB_FULL, s_drop_blob_full);
    put_u32_counter(buf, WIFI_TELEM_U32_DROP_RECORD_LIMIT, s_drop_record_limit);
    put_u32_counter(buf, WIFI_TELEM_U32_DROP_PENDING_BUSY, s_drop_pending_busy);
    put_u32_counter(buf, WIFI_TELEM_U32_DROP_PUSH_FAIL, s_drop_push_fail);
    put_u32_counter(buf, WIFI_TELEM_U32_BLOB_FLUSHES, s_blob_flushes);
    put_u32_counter(buf, WIFI_TELEM_U32_BLOB_POPPED, s_blob_popped);
    put_u32_counter(buf, WIFI_TELEM_U32_BLOB_PENDING_SEEN, s_blob_pending_seen);
    put_u32_counter(buf, WIFI_TELEM_U32_HOP_COUNT, s_hop_count);
    put_u32_counter(buf, WIFI_TELEM_U32_CHANNEL_SET_OK, s_channel_set_ok);
    put_u32_counter(buf, WIFI_TELEM_U32_CHANNEL_SET_ERR, s_channel_set_err);
    put_u32_counter(buf, WIFI_TELEM_U32_MAX_BLOB_BYTES, s_max_blob_bytes);
    put_u32_counter(buf, WIFI_TELEM_U32_MAX_BLOB_FRAMES, s_max_blob_frames);
    put_u32_counter(buf, WIFI_TELEM_U32_MAX_FILL_BYTES, s_max_fill_bytes);
    put_u32_counter(buf, WIFI_TELEM_U32_MAX_FILL_FRAMES, s_max_fill_frames);
    put_u32_counter(buf, WIFI_TELEM_U32_FLUSH_TIMER_FIRES, s_flush_timer_fires);
    put_u32_counter(buf, WIFI_TELEM_U32_FLUSH_FULL_FIRES, s_flush_full_fires);
    put_u32_counter(buf, WIFI_TELEM_U32_FLUSH_EMPTY, s_flush_empty);

    return WIFI_TELEMETRY_SIZE;
}
