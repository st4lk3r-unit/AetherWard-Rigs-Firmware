#pragma once
#include "aw_bus_types.h"
#include "aw_bus_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    awbus_port_t *port;
    uint8_t       node_id;   /* 0x00 = brain; 1..N = companion */
} awbus_t;

/* ---- Init ---- */
void awbus_init(awbus_t *bus, awbus_port_t *port, uint8_t node_id);

/* ---- Shared frame utilities ---- */

/* Serialize awbus_frame_t into a flat AWBUS_FRAME_SIZE wire buffer.
   out must point to at least AWBUS_FRAME_SIZE bytes. */
void awbus_serialize(const awbus_frame_t *f, uint8_t *out);

/* Deserialize AWBUS_FRAME_SIZE wire bytes into awbus_frame_t.
   Returns AWBUS_OK, AWBUS_ERR_MAGIC, AWBUS_ERR_CRC, or AWBUS_ERR_OVERFLOW. */
int  awbus_deserialize(const uint8_t *in, awbus_frame_t *f);

/* CRC16/CCITT-FALSE (poly=0x1021, init=0xFFFF, no reflect) */
uint16_t awbus_crc16(const uint8_t *data, size_t len);

/* ---- Brain-side API ---- */

/* Send a command to `target` companion. Performs one full SPI transaction.
   rx_out receives whatever the companion had queued; pass NULL to discard.
   Returns AWBUS_OK or negative awbus_err_t. */
int awbus_send(awbus_t *bus, uint8_t target, uint8_t cmd,
               const uint8_t *payload, uint16_t len,
               awbus_frame_t *rx_out);

/* Poll companion `node_id` for pending data (checks READY pin first).
   Sends a NULL frame as filler TX. Fills rx_out on success.
   Returns AWBUS_ERR_NODATA if READY is low, AWBUS_OK if data received. */
int awbus_poll(awbus_t *bus, uint8_t node_id, awbus_frame_t *rx_out);

/* Pulse shared RESET line — resets all companions on the bus. */
void awbus_reset_all(awbus_t *bus);

/* ---- Companion-side API ---- */

/* Poll for a brain frame without touching the companion's TX buffer.
   Non-blocking — returns AWBUS_ERR_NODATA if no transaction has completed
   since the last call. Use this in the companion main loop. */
int awbus_companion_recv(awbus_t *bus, awbus_frame_t *rx_out);

/* Queue a reply/push frame for the brain. Loads the TX buffer so the brain
   receives it on the next SPI transaction it initiates.
   rx_out is optional and is zeroed when supplied; replies are queued rather
   than exchanged synchronously on the companion side. */
int awbus_companion_push(awbus_t *bus, uint8_t cmd,
                         const uint8_t *payload, uint16_t len,
                         awbus_frame_t *rx_out);

/* Arm an idle RX transaction after a received frame did not generate a reply. */
int awbus_companion_arm_rx(awbus_t *bus);

/* Drive the companion's READY output pin: 1 = data pending, 0 = idle. */
void awbus_companion_set_ready(awbus_t *bus, int on);

/* Block the companion task until the next SPI frame arrives or timeout_ms
 * elapses.  No-op if the port does not implement wait_frame. */
void awbus_companion_wait_frame(awbus_t *bus, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
