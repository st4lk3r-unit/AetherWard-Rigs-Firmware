#pragma once
#include <stdint.h>

/* ---- Action sub-types (payload[0] of AWBUS_CMD_ACTION) ---- */
typedef enum {
    WIFI_ACTION_TIMESYNC    = 0x01,   /* brain -> companion: sync clock            */
    WIFI_ACTION_SNIFF_START = 0x02,   /* brain -> companion: begin capture         */
    WIFI_ACTION_SNIFF_STOP  = 0x03,   /* brain -> companion: stop, flush remaining */
    WIFI_ACTION_TELEMETRY_REQ = 0x04, /* brain -> companion: request telemetry */
} wifi_action_t;

/* ---- Data sub-types (payload[0] of AWBUS_CMD_DATA_PUSH) ---- */
typedef enum {
    WIFI_DATA_SNIFF_BLOB = 0x01,   /* companion -> brain: captured-frame batch */
    WIFI_DATA_TELEMETRY  = 0x02,   /* companion -> brain: counters / stats     */
} wifi_data_t;

#define WIFI_PROTO_VERSION  2u

/* ---- Frame-type filter bitmask (WIFI_ACTION_SNIFF_START filter_mask) ---- */
#define WIFI_FILTER_BEACON    (1u << 0)
#define WIFI_FILTER_PROBE_REQ (1u << 1)
#define WIFI_FILTER_PROBE_RSP (1u << 2)
#define WIFI_FILTER_EAPOL     (1u << 3)

/* ---- Sniff blob format v2 -------------------------------------------------
 * WIFI_DATA_SNIFF_BLOB payload:
 *   [0]      subtype = WIFI_DATA_SNIFF_BLOB
 *   [1]      version = WIFI_PROTO_VERSION
 *   [2]      node_id
 *   [3]      n_frames
 *   [4]      cur_channel when blob was sealed
 *   [5]      blob_flags
 *   [6]      blob_hdr_len = WIFI_BLOB_HDR_SIZE
 *   [7]      reserved
 *   [8-11]   blob_seq (uint32 LE)
 *   [12-15]  first_frame_seq in this blob (uint32 LE; 0 if empty)
 *   [16-17]  records_len bytes after the blob header (uint16 LE)
 *   [18-19]  fill_bytes_snapshot at seal time (uint16 LE)
 *   [20-23]  dropped_total_snapshot (uint32 LE)
 *   [24+]    frame records
 *
 * Frame record v2:
 *   [0-7]    brain-synced timestamp us (uint64 LE)
 *   [8-11]   frame_seq (uint32 LE; increments for every accepted frame,
 *            including accepted frames later dropped before queueing)
 *   [12-13]  captured_len bytes following this header (uint16 LE)
 *   [14-15]  original_len before snap/truncate (uint16 LE)
 *   [16]     channel
 *   [17]     RSSI (int8)
 *   [18]     802.11 FC byte 0 (type/subtype)
 *   [19]     record_flags
 *   [20+]    raw 802.11 frame bytes
 */
#define WIFI_BLOB_HDR_SIZE    24u
#define WIFI_RECORD_HDR_SIZE  20u

#define WIFI_REC_F_EAPOL      (1u << 0)
#define WIFI_REC_F_TRUNCATED  (1u << 1)

/* ---- Telemetry format v2 --------------------------------------------------
 * WIFI_DATA_TELEMETRY payload:
 *   [0]      subtype = WIFI_DATA_TELEMETRY
 *   [1]      version = WIFI_PROTO_VERSION
 *   [2]      node_id
 *   [3]      current_channel
 *   [4]      running (0/1)
 *   [5]      blob_pending (0/1)
 *   [6]      active fill-buffer index
 *   [7]      last_err / last ESP-IDF error bucket
 *   [8+]     uint32 LE counters indexed by WIFI_TELEM_U32_*
 */
#define WIFI_TELEM_HDR_SIZE   8u

#define WIFI_TELEM_U32_LOCAL_MS             0u
#define WIFI_TELEM_U32_CAPTURED             1u  /* frames successfully queued into blob buffer */
#define WIFI_TELEM_U32_SENT                 2u  /* frames successfully handed to AWBUS push */
#define WIFI_TELEM_U32_DROP_TOTAL           3u
#define WIFI_TELEM_U32_SEEN_MGMT            4u
#define WIFI_TELEM_U32_SEEN_DATA            5u
#define WIFI_TELEM_U32_SEEN_CTRL            6u
#define WIFI_TELEM_U32_SEEN_MISC            7u
#define WIFI_TELEM_U32_SEEN_SHORT           8u
#define WIFI_TELEM_U32_FILTERED             9u
#define WIFI_TELEM_U32_KEPT_BEACON         10u
#define WIFI_TELEM_U32_KEPT_PROBE_REQ      11u
#define WIFI_TELEM_U32_KEPT_PROBE_RSP      12u
#define WIFI_TELEM_U32_KEPT_EAPOL          13u
#define WIFI_TELEM_U32_DROP_MUTEX_BUSY     14u
#define WIFI_TELEM_U32_DROP_BLOB_FULL      15u
#define WIFI_TELEM_U32_DROP_RECORD_LIMIT   16u
#define WIFI_TELEM_U32_DROP_PENDING_BUSY   17u
#define WIFI_TELEM_U32_DROP_PUSH_FAIL      18u
#define WIFI_TELEM_U32_BLOB_FLUSHES        19u
#define WIFI_TELEM_U32_BLOB_POPPED         20u
#define WIFI_TELEM_U32_BLOB_PENDING_SEEN   21u
#define WIFI_TELEM_U32_HOP_COUNT           22u
#define WIFI_TELEM_U32_CHANNEL_SET_OK      23u
#define WIFI_TELEM_U32_CHANNEL_SET_ERR     24u
#define WIFI_TELEM_U32_MAX_BLOB_BYTES      25u
#define WIFI_TELEM_U32_MAX_BLOB_FRAMES     26u
#define WIFI_TELEM_U32_MAX_FILL_BYTES      27u
#define WIFI_TELEM_U32_MAX_FILL_FRAMES     28u
#define WIFI_TELEM_U32_FLUSH_TIMER_FIRES   29u
#define WIFI_TELEM_U32_FLUSH_FULL_FIRES    30u
#define WIFI_TELEM_U32_FLUSH_EMPTY         31u
#define WIFI_TELEM_U32_COUNT               32u

#define WIFI_TELEMETRY_SIZE (WIFI_TELEM_HDR_SIZE + (WIFI_TELEM_U32_COUNT * 4u))

/* Keep the companion MVP conservative until AWBUS frame size / serial bridge
 * are intentionally tuned. Full raw mode can raise this later. */
#ifndef WIFI_SNIFF_SNAPLEN
#define WIFI_SNIFF_SNAPLEN 400u
#endif
