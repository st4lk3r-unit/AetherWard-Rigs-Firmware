#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
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

/* Two buffers: s_fill is written by the WiFi callback; the other is
 * sent to the brain by wifi_sniff_flush once the buffers are swapped. */
static uint8_t  s_blob[2][BLOB_BUF_SIZE];
static uint16_t s_blob_used[2];     /* bytes used for frame records (after blob header) */
static uint8_t  s_blob_nframes[2];
static volatile int s_fill = 0;
static SemaphoreHandle_t s_mutex;

/* ---- Capture state ---- */
static volatile int s_running    = 0;
static volatile int s_flush_due  = 0;
static uint64_t     s_ts_offset  = 0;   /* brain_us − local_us at timesync */
static uint8_t      s_filter_mask = 0;
static volatile uint8_t s_cur_channel = 1;

/* ---- Channel hopper ---- */
static uint8_t           s_channels[13];
static uint8_t           s_n_channels  = 0;
static volatile uint8_t  s_ch_idx      = 0;
static esp_timer_handle_t s_hop_timer  = NULL;

/* ---- Statistics ---- */
static volatile uint32_t s_captured  = 0;
static volatile uint32_t s_dropped   = 0;
static volatile uint32_t s_sent      = 0;
static volatile uint32_t s_hop_count = 0;

/* ---- Pending blob (set by wifi_sniff_flush, cleared by wifi_sniff_pop_pending) ----
 * wifi_sniff_flush signals the brain by raising READY but does NOT push the blob
 * into the SPI queue yet — the IDLE slot already sitting at the front of the queue
 * would be consumed first, sending a NULL frame to the brain and leaving the REPLY
 * stuck.  Instead, the flush raises READY and parks the swapped buffer here.
 * When the brain sends its NULL poll, companion_run -> handle_brain_frame(NULL) ->
 * wifi_sniff_pop_pending queues the REPLY as the response to that NULL, which is
 * the correct position in the queue. */
static volatile int s_blob_pending      = 0;
static int          s_pending_idx       = 0;
static uint16_t     s_pending_used      = 0;
static uint8_t      s_pending_nframes   = 0;

/* ---- Timer callbacks ---- */

static void flush_timer_cb(void *arg) {
    (void)arg;
    s_flush_due = 1;
}

static void hop_timer_cb(void *arg) {
    (void)arg;
    if (!s_running || s_n_channels == 0) return;
    s_ch_idx = (uint8_t)((s_ch_idx + 1u) % s_n_channels);
    s_cur_channel = s_channels[s_ch_idx];
    esp_wifi_set_channel(s_cur_channel, WIFI_SECOND_CHAN_NONE);
    s_hop_count++;
}

/* ---- Flush timer (created once, stopped/started per sniff session) ---- */
static esp_timer_handle_t s_flush_timer = NULL;

/* ---- WiFi promiscuous callback ---- */

static void wifi_rx_cb(void *buf, wifi_promiscuous_pkt_type_t pkt_type) {
    if (!s_running || pkt_type == WIFI_PKT_MISC) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    uint16_t frame_len   = (uint16_t)pkt->rx_ctrl.sig_len;

    /* Strip 4-byte FCS appended by ESP-IDF */
    if (frame_len > 4u) frame_len -= 4u;
    if (frame_len < 4u) return;

    uint8_t fc0      = frame[0];
    uint8_t ftype    = (fc0 >> 2u) & 0x03u;
    uint8_t fsubtype = (fc0 >> 4u) & 0x0Fu;

    int accept = 0;
    if (pkt_type == WIFI_PKT_MGMT) {
        if ((s_filter_mask & WIFI_FILTER_BEACON)    && ftype == 0 && fsubtype == 8)  accept = 1;
        if ((s_filter_mask & WIFI_FILTER_PROBE_REQ) && ftype == 0 && fsubtype == 4)  accept = 1;
        if ((s_filter_mask & WIFI_FILTER_PROBE_RSP) && ftype == 0 && fsubtype == 5)  accept = 1;
    } else if (pkt_type == WIFI_PKT_DATA && (s_filter_mask & WIFI_FILTER_EAPOL)) {
        /* Body offset: 24 for non-QoS, 26 for QoS (subtype bit 3 set).
         * Add 6 for 4-address frames (ToDS=1 && FromDS=1). */
        uint8_t  fc1      = frame[1];
        uint16_t body_off = (fsubtype & 0x08u) ? 26u : 24u;
        if ((fc1 & 0x03u) == 0x03u) body_off += 6u;
        /* LLC+SNAP EAPOL magic: AA AA 03 00 00 00 88 8E */
        if (frame_len >= body_off + 8u) {
            const uint8_t *llc = frame + body_off;
            if (llc[0] == 0xAAu && llc[1] == 0xAAu && llc[2] == 0x03u &&
                llc[3] == 0x00u && llc[4] == 0x00u && llc[5] == 0x00u &&
                llc[6] == 0x88u && llc[7] == 0x8Eu)
                accept = 1;
        }
    }

    if (!accept) return;

    /* Cap frame size — prevents oversized records from wasting blob space */
    if (frame_len > 400u) frame_len = 400u;

    uint64_t ts_us  = (uint64_t)esp_timer_get_time() + s_ts_offset;
    int8_t   rssi   = (int8_t)pkt->rx_ctrl.rssi;
    uint8_t  ch     = (uint8_t)pkt->rx_ctrl.channel;
    uint16_t needed = WIFI_RECORD_HDR_SIZE + frame_len;

    /* Non-blocking mutex: if flush is in progress, drop the frame */
    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        int fi = s_fill;
        if (s_blob_nframes[fi] < 255u &&
            WIFI_BLOB_HDR_SIZE + s_blob_used[fi] + needed <= BLOB_BUF_SIZE) {

            uint8_t *p = s_blob[fi] + WIFI_BLOB_HDR_SIZE + s_blob_used[fi];

            /* ts_us — 8 bytes LE */
            for (int i = 0; i < 8; i++) p[i] = (uint8_t)(ts_us >> (i * 8));
            /* frame_len — 2 bytes LE */
            p[8]  = (uint8_t)(frame_len & 0xFFu);
            p[9]  = (uint8_t)(frame_len >> 8u);
            /* channel, rssi */
            p[10] = ch;
            p[11] = (uint8_t)rssi;
            /* 802.11 frame bytes */
            memcpy(p + WIFI_RECORD_HDR_SIZE, frame, frame_len);

            s_blob_used[fi]   += needed;
            s_blob_nframes[fi]++;
            s_captured++;
        } else {
            s_dropped++;
        }
        xSemaphoreGive(s_mutex);
    } else {
        s_dropped++;
    }
}

/* ---- Public API ---- */

void wifi_sniff_init(void) {
    /* NVS is required by esp_wifi_init */
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
    /* STA mode (without connecting) is required for esp_wifi_set_channel to
     * actually tune the radio.  WIFI_MODE_NULL leaves the receiver inactive. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    /* Create timers once; they are started/stopped per session */
    const esp_timer_create_args_t flush_args = { .callback = flush_timer_cb, .name = "sniff-flush" };
    ESP_ERROR_CHECK(esp_timer_create(&flush_args, &s_flush_timer));

    const esp_timer_create_args_t hop_args = { .callback = hop_timer_cb, .name = "sniff-hop" };
    ESP_ERROR_CHECK(esp_timer_create(&hop_args, &s_hop_timer));

    ESP_LOGI(TAG, "WiFi sniff ready");
}

void wifi_sniff_timesync(uint64_t brain_us) {
    s_ts_offset = brain_us - (uint64_t)esp_timer_get_time();
    ESP_LOGI(TAG, "timesync offset=%" PRId64 " us", (int64_t)s_ts_offset);
}

void wifi_sniff_start(const uint8_t *channels, uint8_t n_ch,
                      uint8_t filter_mask, uint16_t hop_period_ms) {
    /* Reset double-buffer */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_fill = 0;
    memset(s_blob_used,    0, sizeof s_blob_used);
    memset(s_blob_nframes, 0, sizeof s_blob_nframes);
    xSemaphoreGive(s_mutex);

    s_captured    = 0;
    s_dropped     = 0;
    s_sent        = 0;
    s_hop_count   = 0;
    s_flush_due   = 0;
    s_filter_mask = filter_mask;

    if (n_ch > 13u) n_ch = 13u;
    memcpy(s_channels, channels, n_ch);
    s_n_channels  = n_ch;
    s_ch_idx      = 0;
    s_cur_channel = (n_ch > 0u) ? channels[0] : 1u;

    /* Configure ESP-IDF promiscuous filter */
    wifi_promiscuous_filter_t flt = {0};
    if (filter_mask & (WIFI_FILTER_BEACON | WIFI_FILTER_PROBE_REQ | WIFI_FILTER_PROBE_RSP))
        flt.filter_mask |= WIFI_PROMIS_FILTER_MASK_MGMT;
    if (filter_mask & WIFI_FILTER_EAPOL)
        flt.filter_mask |= WIFI_PROMIS_FILTER_MASK_DATA;

    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(wifi_rx_cb);
    esp_wifi_set_channel(s_cur_channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);

    s_running = 1;

    esp_timer_start_periodic(s_flush_timer, 50000u);   /* 50 ms */

    if (n_ch > 1u && hop_period_ms > 0u)
        esp_timer_start_periodic(s_hop_timer, (uint64_t)hop_period_ms * 1000u);

    ESP_LOGI(TAG, "sniff start: filter=0x%02x  ch=%d  hop=%dms",
             filter_mask, n_ch, hop_period_ms);
}

void wifi_sniff_stop(void) {
    if (!s_running) return;
    s_running = 0;

    esp_timer_stop(s_flush_timer);
    esp_timer_stop(s_hop_timer);

    esp_wifi_set_promiscuous(false);
    s_flush_due = 1;   /* flush whatever is in the fill buffer */

    ESP_LOGI(TAG, "sniff stop: captured=%u dropped=%u sent=%u",
             (unsigned)s_captured, (unsigned)s_dropped, (unsigned)s_sent);
}

int wifi_sniff_flush(awbus_t *bus) {
    /* Don't overwrite a blob that hasn't been sent yet */
    if (s_blob_pending) return 0;

    /* Gate: only flush when the timer fired or the buffer is nearly full */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int      fi       = s_fill;
    uint8_t  nf       = s_blob_nframes[fi];
    uint16_t used     = s_blob_used[fi];
    int      buf_full = (WIFI_BLOB_HDR_SIZE + used >= BLOB_FLUSH_THRESHOLD);
    xSemaphoreGive(s_mutex);

    if (nf == 0u) { s_flush_due = 0; return 0; }
    if (!s_flush_due && !buf_full) return 0;
    s_flush_due = 0;

    /* Swap buffers under lock — WiFi cb switches to writing the other buffer */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    fi   = s_fill;
    nf   = s_blob_nframes[fi];
    used = s_blob_used[fi];

    if (nf == 0u) { xSemaphoreGive(s_mutex); return 0; }

    /* Write blob header into the flush buffer before swapping */
    s_blob[fi][0] = WIFI_DATA_SNIFF_BLOB;
    s_blob[fi][1] = (uint8_t)AWBUS_NODE_ID;
    s_blob[fi][2] = nf;
    s_blob[fi][3] = s_cur_channel;

    int flush_idx = fi;
    int next_idx  = fi ^ 1;
    s_blob_used[next_idx]    = 0;
    s_blob_nframes[next_idx] = 0;
    s_fill = next_idx;
    xSemaphoreGive(s_mutex);

    /* Park the ready blob and raise READY.  The actual awbus_companion_push
     * happens in wifi_sniff_pop_pending when the brain's next NULL poll arrives,
     * so the REPLY slot is at the correct position in the SPI queue. */
    s_pending_idx     = flush_idx;
    s_pending_used    = used;
    s_pending_nframes = nf;
    s_blob_pending    = 1;
    awbus_companion_set_ready(bus, 1);
    return 1;
}

/* Called from handle_brain_frame(AWBUS_CMD_NULL) — sends the pending blob as the
 * reply to the brain's NULL poll.  Returns 1 if a reply was queued, 0 otherwise. */
int wifi_sniff_pop_pending(awbus_t *bus) {
    if (!s_blob_pending) return 0;
    s_blob_pending = 0;

    uint16_t total = WIFI_BLOB_HDR_SIZE + s_pending_used;
    int rc = awbus_companion_push(bus, AWBUS_CMD_DATA_PUSH,
                                  s_blob[s_pending_idx], total, NULL);
    if (rc != AWBUS_OK) {
        s_dropped += s_pending_nframes;
        return 0;
    }
    awbus_companion_set_ready(bus, 1);
    s_sent += s_pending_nframes;
    return 1;
}

uint16_t wifi_sniff_telemetry(uint8_t *buf) {
    buf[0] = WIFI_DATA_TELEMETRY;
    buf[1] = (uint8_t)AWBUS_NODE_ID;
    buf[2] = s_cur_channel;
    buf[3] = 0;

    int off = 4;
    uint32_t vals[5] = {
        s_captured,
        s_dropped,
        s_sent,
        s_hop_count,
        (uint32_t)((uint64_t)esp_timer_get_time() / 1000u),
    };
    for (int i = 0; i < 5; i++) {
        buf[off++] = (uint8_t)( vals[i]        & 0xFFu);
        buf[off++] = (uint8_t)((vals[i] >>  8) & 0xFFu);
        buf[off++] = (uint8_t)((vals[i] >> 16) & 0xFFu);
        buf[off++] = (uint8_t)((vals[i] >> 24) & 0xFFu);
    }
    return (uint16_t)off;   /* == WIFI_TELEMETRY_SIZE == 24 */
}
