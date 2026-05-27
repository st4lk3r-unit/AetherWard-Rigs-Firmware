#pragma once
#include <stdint.h>

/* ---- Action sub-types (payload[0] of AWBUS_CMD_ACTION) ---- */
typedef enum {
    WIFI_ACTION_TIMESYNC    = 0x01,   /* brain → companion: sync clock            */
    WIFI_ACTION_SNIFF_START = 0x02,   /* brain → companion: begin capture         */
    WIFI_ACTION_SNIFF_STOP  = 0x03,   /* brain → companion: stop, flush remaining */
} wifi_action_t;

/* ---- Data sub-types (payload[0] of AWBUS_CMD_DATA_PUSH) ---- */
typedef enum {
    WIFI_DATA_SNIFF_BLOB = 0x01,   /* companion → brain: captured-frame batch */
    WIFI_DATA_TELEMETRY  = 0x02,   /* companion → brain: counters / stats     */
} wifi_data_t;

/* ---- Frame-type filter bitmask (WIFI_ACTION_SNIFF_START filter_mask) ---- */
#define WIFI_FILTER_BEACON    (1u << 0)
#define WIFI_FILTER_PROBE_REQ (1u << 1)
#define WIFI_FILTER_PROBE_RSP (1u << 2)
#define WIFI_FILTER_EAPOL     (1u << 3)

/*
 * WIFI_ACTION_TIMESYNC payload (9 bytes):
 *   [0]    WIFI_ACTION_TIMESYNC
 *   [1-8]  brain_us (uint64_t LE) — brain microsecond clock at send time
 *
 * WIFI_ACTION_SNIFF_START payload (5 + n_ch bytes, max 18):
 *   [0]           WIFI_ACTION_SNIFF_START
 *   [1]           filter_mask
 *   [2]           n_channels
 *   [3..2+n]      channels[n]  (1-based 2.4 GHz channel numbers, 1..13)
 *   [3+n..4+n]    hop_period_ms (uint16_t LE)
 *
 * WIFI_ACTION_SNIFF_STOP payload (1 byte):
 *   [0]    WIFI_ACTION_SNIFF_STOP
 *
 * WIFI_DATA_SNIFF_BLOB payload:
 *   [0]    WIFI_DATA_SNIFF_BLOB
 *   [1]    node_id
 *   [2]    n_frames
 *   [3]    cur_channel  (channel companion was on when flush was triggered)
 *   [4+]   frame records (packed, variable-length per record):
 *            uint64_t ts_us     (8B) — brain-synced microseconds
 *            uint16_t frame_len (2B) — bytes of 802.11 frame data following
 *            uint8_t  channel   (1B)
 *            int8_t   rssi      (1B)
 *            uint8_t  data[frame_len]
 *
 * WIFI_DATA_TELEMETRY payload (24 bytes):
 *   [0]     WIFI_DATA_TELEMETRY
 *   [1]     node_id
 *   [2]     current_channel
 *   [3]     pad
 *   [4-7]   frames_captured (uint32_t LE)
 *   [8-11]  frames_dropped  (uint32_t LE)
 *   [12-15] frames_sent     (uint32_t LE)
 *   [16-19] hop_count       (uint32_t LE)
 *   [20-23] uptime_ms       (uint32_t LE)
 */
#define WIFI_BLOB_HDR_SIZE    4u    /* subtype + node_id + n_frames + pad */
#define WIFI_RECORD_HDR_SIZE  12u   /* ts_us(8) + frame_len(2) + channel(1) + rssi(1) */
#define WIFI_TELEMETRY_SIZE   24u
