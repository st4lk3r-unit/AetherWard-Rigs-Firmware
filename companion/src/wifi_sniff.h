#pragma once
#include <stdint.h>
#include "aw_bus/aw_bus.h"

/* One-time WiFi stack init — call from companion_init. */
void wifi_sniff_init(void);

/* Store the brain's microsecond clock for timestamp sync.
 * Call before wifi_sniff_start when WIFI_ACTION_TIMESYNC is received. */
void wifi_sniff_timesync(uint64_t brain_us);

/* Begin promiscuous capture.
 * channels[n_ch]: 1-based 2.4 GHz channel numbers.
 * filter_mask: WIFI_FILTER_* bitmask.
 * hop_period_ms: dwell per channel in milliseconds. */
void wifi_sniff_start(const uint8_t *channels, uint8_t n_ch,
                      uint8_t filter_mask, uint16_t hop_period_ms);

/* Stop capture; marks remaining buffered frames for immediate flush. */
void wifi_sniff_stop(void);

/* Call from the companion main loop.
 * Swaps the fill buffer and raises READY when the flush timer fires or the
 * buffer is ≥75% full.  The actual SPI push is deferred to
 * wifi_sniff_pop_pending.  Returns 1 if READY was raised, 0 otherwise. */
int wifi_sniff_flush(awbus_t *bus);

/* Call from handle_brain_frame(AWBUS_CMD_NULL).
 * Sends the blob that wifi_sniff_flush staged as the reply to the brain's
 * NULL poll — this puts the REPLY slot at the correct position in the SPI
 * queue (after the IDLE slot that was already at the front is consumed).
 * Returns 1 if a reply was queued, 0 if nothing was pending. */
int wifi_sniff_pop_pending(awbus_t *bus);

/* Write a WIFI_DATA_TELEMETRY payload into buf[WIFI_TELEMETRY_SIZE].
 * Returns WIFI_TELEMETRY_SIZE. */
uint16_t wifi_sniff_telemetry(uint8_t *buf);
