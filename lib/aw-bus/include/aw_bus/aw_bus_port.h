#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport vtable — one instance per bus/platform combination.
 *
 * Brain (SPI master) fills it like this:
 *   transfer()   — assert CS[node_id], full-duplex SPI, deassert CS
 *   ready()      — read READY input GPIO for node_id  (1=data pending, 0=idle)
 *   reset_all()  — pulse shared RESET output GPIO low→high
 *
 * Companion (SPI slave) fills it like this:
 *   transfer()   — load tx_buf into slave DMA TX, retrieve last completed
 *                  RX into rx_buf; node_id is ignored, pass 0
 *   ready()      — drive own READY output GPIO: 1=assert, 0=deassert
 *   reset_all()  — no-op (companion IS the peripheral)
 *
 * Both tx_buf and rx_buf point to AWBUS_FRAME_SIZE bytes of storage.
 * Passing NULL for rx_buf discards the received data.
 */
typedef struct {
    int  (*transfer)(void *ctx, uint8_t node_id,
                     const uint8_t *tx_buf, uint8_t *rx_buf, size_t len);
    int  (*ready)(void *ctx, uint8_t node_id);
    void (*reset_all)(void *ctx);
    /* Companion only: block until the next SPI transaction completes or
     * timeout_ms elapses.  Allows the companion task to sleep instead of
     * busy-polling.  NULL if not supported (port falls back to polling). */
    void (*wait_frame)(void *ctx, uint32_t timeout_ms);
    void *ctx;
} awbus_port_t;

#ifdef __cplusplus
}
#endif
