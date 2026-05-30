#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "companion.h"
#include "board.h"
#include "aw_wifi/wifi_proto.h"
#include "wifi_sniff.h"

#define TAG "companion"

#ifndef AWBUS_RIG_REVISION
#define AWBUS_RIG_REVISION "0.0"
#endif

/* AWBUS_RESET_PIN is configured as INPUT_PULLUP in bus_port_spi.c.
 * The brain pulls it LOW to reboot all companions simultaneously. */
static inline void check_hw_reset(void) {
    if (!gpio_get_level(AWBUS_RESET_PIN)) {
        ESP_LOGW(TAG, "HW RESET — rebooting");
        esp_restart();
    }
}

extern awbus_port_t *companion_bus_port_init(void);

static const arch_api_t *A;
static awbus_t g_bus;

/* ---- RF antenna selection --------------------------------------------------
 * XIAO ESP32C6 RF switch (internal PCB GPIOs, not on pin headers):
 *   GPIO3  (WIFI_ENABLE) — drive LOW  to power the RF switch IC
 *   GPIO14 (WIFI_ANT)    — LOW  = onboard PCB ceramic antenna (default)
 *                          HIGH = external U.FL connector
 *
 * Build flag: -DCOMPANION_EXT_ANTENNA=1  → selects external U.FL at boot
 */
static void antenna_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << COMPANION_WIFI_ENABLE_GPIO) |
                        (1ULL << COMPANION_ANT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    gpio_set_level(COMPANION_WIFI_ENABLE_GPIO, 0);   /* enable RF switch IC */

#if defined(COMPANION_EXT_ANTENNA) && COMPANION_EXT_ANTENNA
    gpio_set_level(COMPANION_ANT_GPIO, 1);
    ESP_LOGI(TAG, "antenna: external U.FL");
#else
    gpio_set_level(COMPANION_ANT_GPIO, 0);
    ESP_LOGI(TAG, "antenna: onboard PCB ceramic");
#endif
}

/* ---- LED heartbeat ---------------------------------------------------------
 * GPIO15 = onboard user LED (not on pin headers).
 * Build flag: -DCOMPANION_HEARTBEAT=1  → blinks the LED at ~1 Hz when healthy.
 */
#if defined(COMPANION_HEARTBEAT) && COMPANION_HEARTBEAT
static int s_led_state = 0;

static void heartbeat_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << COMPANION_LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(COMPANION_LED_GPIO, 0);
    ESP_LOGI(TAG, "heartbeat: enabled on GPIO%d", COMPANION_LED_GPIO);
}

/* 2 fast blinks per second: ON 80ms, OFF 80ms, ON 80ms, OFF 760ms */
static void heartbeat_tick(void) {
    static uint32_t last_ms = 0;
    static int      phase   = 3;   /* start in last OFF phase so first tick turns ON */
    static const uint32_t delays[4] = {80u, 80u, 80u, 760u};

    uint32_t now = A->millis();
    if (now - last_ms >= delays[phase]) {
        phase   = (phase + 1) & 3;
        last_ms = now;
        /* phases 0 and 2 are ON, phases 1 and 3 are OFF */
        s_led_state = (phase == 0 || phase == 2) ? 1 : 0;
        gpio_set_level(COMPANION_LED_GPIO, s_led_state);
    }
}
#endif /* COMPANION_HEARTBEAT */

/* ---- Status payload: [node_id, uptime (4 bytes LE)] ---- */
static void fill_status(uint8_t *buf, uint16_t *len) {
    uint32_t up = A->millis();
    buf[0] = AWBUS_NODE_ID;
    buf[1] = (uint8_t)( up        & 0xFFu);
    buf[2] = (uint8_t)((up >>  8) & 0xFFu);
    buf[3] = (uint8_t)((up >> 16) & 0xFFu);
    buf[4] = (uint8_t)((up >> 24) & 0xFFu);
    *len   = 5u;
}

/* ---- Info payload: [node_id(1), uptime(4 LE), board_name(16), fw_rev(8)] ---- */
static void fill_info(uint8_t *buf, uint16_t *len) {
    uint32_t    up  = A->millis();
    int         i   = 0;
    const char *bn  = NEU_BOARD_NAME;
    const char *rv  = AWBUS_RIG_REVISION;
    size_t      bl, rl;

    buf[i++] = (uint8_t)AWBUS_NODE_ID;
    buf[i++] = (uint8_t)( up        & 0xFFu);
    buf[i++] = (uint8_t)((up >>  8) & 0xFFu);
    buf[i++] = (uint8_t)((up >> 16) & 0xFFu);
    buf[i++] = (uint8_t)((up >> 24) & 0xFFu);

    bl = strlen(bn); if (bl > 15) bl = 15;
    memset(buf + i, 0, 16); memcpy(buf + i, bn, bl); i += 16;

    rl = strlen(rv); if (rl > 7) rl = 7;
    memset(buf + i, 0, 8);  memcpy(buf + i, rv, rl); i += 8;

    *len = (uint16_t)i;
}

/* ---- Handle a frame received from the brain ----
 * Returns 1 when a reply transaction was queued, 0 when the bus must be
 * re-armed for idle RX. */
static int handle_brain_frame(const awbus_frame_t *rx) {
    uint8_t  payload[32];
    uint16_t plen      = 0;
    uint8_t  reply_cmd = AWBUS_CMD_NULL;

    switch ((awbus_cmd_t)rx->cmd) {
    case AWBUS_CMD_NULL:
        return wifi_sniff_pop_pending(&g_bus);

    case AWBUS_CMD_PING:
        reply_cmd = AWBUS_CMD_PONG;
        ESP_LOGI(TAG, "PING");
        break;

    case AWBUS_CMD_STATUS_REQ:
        reply_cmd = AWBUS_CMD_STATUS_RSP;
        fill_status(payload, &plen);
        ESP_LOGI(TAG, "STATUS_REQ");
        break;

    case AWBUS_CMD_INFO_REQ:
        reply_cmd = AWBUS_CMD_INFO_RSP;
        fill_info(payload, &plen);
        ESP_LOGI(TAG, "INFO_REQ");
        break;

    case AWBUS_CMD_RESET:
        ESP_LOGW(TAG, "RESET from brain — rebooting");
        esp_restart();
        return 0;

    case AWBUS_CMD_ACTION:
        if (rx->payload_len < 1) return 0;
        switch ((wifi_action_t)rx->payload[0]) {

        case WIFI_ACTION_TIMESYNC:
            if (rx->payload_len >= 9u) {
                uint64_t brain_us = 0;
                for (int i = 0; i < 8; i++)
                    brain_us |= (uint64_t)rx->payload[1 + i] << (i * 8);
                wifi_sniff_timesync(brain_us);
            }
            return 0;

        case WIFI_ACTION_SNIFF_START: {
            uint8_t n_ch = (rx->payload_len >= 3u) ? rx->payload[2] : 0u;
            if (rx->payload_len < (uint16_t)(3u + n_ch + 2u)) return 0;
            uint8_t  filter  = rx->payload[1];
            const uint8_t *channels = &rx->payload[3];
            uint16_t hop_ms  = (uint16_t)rx->payload[3 + n_ch]
                             | ((uint16_t)rx->payload[4 + n_ch] << 8u);
            wifi_sniff_start(channels, n_ch, filter, hop_ms);
            return 0;
        }

        case WIFI_ACTION_SNIFF_STOP: {
            wifi_sniff_stop();
            /* Push telemetry immediately as the STOP reply */
            uint8_t tbuf[WIFI_TELEMETRY_SIZE];
            uint16_t tlen = wifi_sniff_telemetry(tbuf);
            if (awbus_companion_push(&g_bus, AWBUS_CMD_DATA_PUSH, tbuf, tlen, NULL) == AWBUS_OK) {
                awbus_companion_set_ready(&g_bus, 1);
                return 1;
            }
            return 0;
        }

        case WIFI_ACTION_TELEMETRY_REQ: {
            uint8_t tbuf[WIFI_TELEMETRY_SIZE];
            uint16_t tlen = wifi_sniff_telemetry(tbuf);
            if (awbus_companion_push(&g_bus, AWBUS_CMD_DATA_PUSH, tbuf, tlen, NULL) == AWBUS_OK) {
                awbus_companion_set_ready(&g_bus, 1);
                return 1;
            }
            return 0;
        }

        default:
            ESP_LOGW(TAG, "unknown ACTION subtype 0x%02x", rx->payload[0]);
            return 0;
        }
        return 0;   /* all inner cases return; this silences fallthrough warnings */

    default:
        ESP_LOGW(TAG, "unknown cmd 0x%02x", rx->cmd);
        return 0;
    }

    if (awbus_companion_push(&g_bus, reply_cmd, payload, plen, NULL) == AWBUS_OK) {
        awbus_companion_set_ready(&g_bus, 1);
        return 1;
    }

    ESP_LOGW(TAG, "failed to queue reply cmd=0x%02x", reply_cmd);
    return 0;
}

/* ---- Public API ---- */

int companion_init(const arch_api_t *arch) {
    A = arch;

    antenna_init();

#if defined(COMPANION_HEARTBEAT) && COMPANION_HEARTBEAT
    heartbeat_init();
#endif

    awbus_port_t *port = companion_bus_port_init();
    if (!port) {
        ESP_LOGE(TAG, "bus port init failed");
        return -1;
    }
    awbus_init(&g_bus, port, (uint8_t)AWBUS_NODE_ID);

    wifi_sniff_init();

    ESP_LOGI(TAG, "aetherward-rig companion  node=%d  board=%s",
             AWBUS_NODE_ID, NEU_BOARD_NAME);
    return 0;
}

void companion_run(void) {
    check_hw_reset();

    awbus_frame_t rx = {0};
    int rc = awbus_companion_recv(&g_bus, &rx);
    if (rc == AWBUS_OK) {
        awbus_companion_set_ready(&g_bus, 0);
        if (!handle_brain_frame(&rx)) {
            int ar = awbus_companion_arm_rx(&g_bus);
            if (ar != AWBUS_OK)
                ESP_LOGW(TAG, "idle arm failed: %d", ar);
        }
    } else if (rc == AWBUS_ERR_NODATA) {
        /* Try to flush a pending WiFi blob.  If one was queued (READY raised),
         * skip the wait so the brain can poll immediately.  Otherwise yield
         * until the next SPI transaction or the 10 ms heartbeat timeout. */
        if (!wifi_sniff_flush(&g_bus))
            awbus_companion_wait_frame(&g_bus, 10u);
    } else {
        /* A bad/malformed frame was already reaped.  Re-arm idle RX so one
         * corrupted frame does not wedge the whole companion offline. */
        awbus_companion_set_ready(&g_bus, 0);
        ESP_LOGW(TAG, "bad brain frame: %d", rc);
        int ar = awbus_companion_arm_rx(&g_bus);
        if (ar != AWBUS_OK)
            ESP_LOGW(TAG, "idle re-arm after bad frame failed: %d", ar);
    }
#if defined(COMPANION_HEARTBEAT) && COMPANION_HEARTBEAT
    heartbeat_tick();
#endif
}
