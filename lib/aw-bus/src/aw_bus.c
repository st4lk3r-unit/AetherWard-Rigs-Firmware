#include "aw_bus/aw_bus.h"
#include <string.h>

/* ---- CRC16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no bit-reflect ---- */
uint16_t awbus_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u)
                ? (uint16_t)((crc << 1u) ^ 0x1021u)
                : (uint16_t)(crc << 1u);
    }
    return crc;
}

/* ---- Internal: extend a running CRC over a second buffer ---- */
static uint16_t crc16_continue(uint16_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u)
                ? (uint16_t)((crc << 1u) ^ 0x1021u)
                : (uint16_t)(crc << 1u);
    }
    return crc;
}

/* ---- Serialize awbus_frame_t → flat wire buffer ---- */
void awbus_serialize(const awbus_frame_t *f, uint8_t *out) {
    uint16_t plen = (f->payload_len <= AWBUS_MAX_PAYLOAD) ? f->payload_len : 0u;

    memset(out, 0, AWBUS_FRAME_SIZE);

    out[0] = AWBUS_MAGIC_0;
    out[1] = AWBUS_MAGIC_1;
    out[2] = f->node_id;
    out[3] = f->cmd;
    out[4] = (uint8_t)(plen & 0xFFu);
    out[5] = (uint8_t)(plen >> 8u);
    /* bytes [6..7] reserved for CRC — filled below */

    uint16_t crc = awbus_crc16(out, 6);

    if (plen > 0u) {
        memcpy(out + AWBUS_HEADER_SIZE, f->payload, plen);
        crc = crc16_continue(crc, out + AWBUS_HEADER_SIZE, plen);
    }

    out[6] = (uint8_t)(crc & 0xFFu);
    out[7] = (uint8_t)(crc >> 8u);
}

/* ---- Deserialize flat wire buffer → awbus_frame_t ---- */
int awbus_deserialize(const uint8_t *in, awbus_frame_t *f) {
    if (in[0] != AWBUS_MAGIC_0 || in[1] != AWBUS_MAGIC_1)
        return AWBUS_ERR_MAGIC;

    uint16_t plen = (uint16_t)(in[4] | ((uint16_t)in[5] << 8u));
    uint16_t rcrc = (uint16_t)(in[6] | ((uint16_t)in[7] << 8u));

    if (plen > AWBUS_MAX_PAYLOAD)
        return AWBUS_ERR_OVERFLOW;

    /* Recompute CRC over header bytes [0..5] + payload */
    uint16_t crc = awbus_crc16(in, 6);
    if (plen > 0u)
        crc = crc16_continue(crc, in + AWBUS_HEADER_SIZE, plen);

    if (crc != rcrc)
        return AWBUS_ERR_CRC;

    f->node_id     = in[2];
    f->cmd         = in[3];
    f->payload_len = plen;
    if (plen > 0u)
        memcpy(f->payload, in + AWBUS_HEADER_SIZE, plen);

    return AWBUS_OK;
}

/* ---- Bus context ---- */
void awbus_init(awbus_t *bus, awbus_port_t *port, uint8_t node_id) {
    bus->port    = port;
    bus->node_id = node_id;
}

/* ---- Brain: send a command, optionally collect companion's queued TX ---- */
int awbus_send(awbus_t *bus, uint8_t target, uint8_t cmd,
               const uint8_t *payload, uint16_t len,
               awbus_frame_t *rx_out) {
    static uint8_t tx_wire[AWBUS_FRAME_SIZE];
    static uint8_t rx_wire[AWBUS_FRAME_SIZE];

    awbus_frame_t tx = {0};
    tx.node_id     = target;
    tx.cmd         = cmd;
    tx.payload_len = (len <= AWBUS_MAX_PAYLOAD) ? len : AWBUS_MAX_PAYLOAD;
    if (tx.payload_len > 0u && payload)
        memcpy(tx.payload, payload, tx.payload_len);

    awbus_serialize(&tx, tx_wire);

    int rc = bus->port->transfer(bus->port->ctx, target,
                                 tx_wire, rx_out ? rx_wire : NULL,
                                 AWBUS_FRAME_SIZE);
    if (rc != AWBUS_OK)
        return rc;

    if (rx_out)
        return awbus_deserialize(rx_wire, rx_out);

    return AWBUS_OK;
}

/* ---- Brain: poll a companion for pending data (READY-gated) ---- */
int awbus_poll(awbus_t *bus, uint8_t node_id, awbus_frame_t *rx_out) {
    int rdy = bus->port->ready(bus->port->ctx, node_id);
    if (rdy <= 0)
        return AWBUS_ERR_NODATA;

    /* Send a NULL filler frame — the companion sends its queued data back */
    return awbus_send(bus, node_id, AWBUS_CMD_NULL, NULL, 0u, rx_out);
}

/* ---- Brain: reset all companions ---- */
void awbus_reset_all(awbus_t *bus) {
    bus->port->reset_all(bus->port->ctx);
}

/* ---- Companion: queue a TX frame for the next brain poll ---- */
int awbus_companion_push(awbus_t *bus, uint8_t cmd,
                         const uint8_t *payload, uint16_t len,
                         awbus_frame_t *rx_out) {
    static uint8_t tx_wire[AWBUS_FRAME_SIZE];

    awbus_frame_t tx = {0};
    tx.node_id     = bus->node_id;
    tx.cmd         = cmd;
    tx.payload_len = (len <= AWBUS_MAX_PAYLOAD) ? len : AWBUS_MAX_PAYLOAD;
    if (tx.payload_len > 0u && payload)
        memcpy(tx.payload, payload, tx.payload_len);

    awbus_serialize(&tx, tx_wire);

    int rc = bus->port->transfer(bus->port->ctx, 0u,
                                 tx_wire, NULL, AWBUS_FRAME_SIZE);
    if (rc != AWBUS_OK)
        return rc;

    if (rx_out)
        memset(rx_out, 0, sizeof *rx_out);

    return AWBUS_OK;
}

/* ---- Companion: poll for RX without touching TX buffer ---- */
int awbus_companion_recv(awbus_t *bus, awbus_frame_t *rx_out) {
    static uint8_t rx_wire[AWBUS_FRAME_SIZE];
    /* tx_buf=NULL, rx_buf!=NULL asks the port to reap one completed RX.
     * Always provide rx_wire so recv(NULL) still consumes/discards a frame;
     * tx_buf=NULL, rx_buf=NULL is reserved for awbus_companion_arm_rx(). */
    int rc = bus->port->transfer(bus->port->ctx, 0u,
                                 NULL, rx_wire, AWBUS_FRAME_SIZE);
    if (rc != AWBUS_OK)
        return rc;
    if (rx_out)
        return awbus_deserialize(rx_wire, rx_out);
    return AWBUS_OK;
}

/* ---- Companion: arm an idle RX transaction ---- */
int awbus_companion_arm_rx(awbus_t *bus) {
    return bus->port->transfer(bus->port->ctx, 0u, NULL, NULL, AWBUS_FRAME_SIZE);
}

/* ---- Companion: drive READY output pin ---- */
void awbus_companion_set_ready(awbus_t *bus, int on) {
    bus->port->ready(bus->port->ctx, (uint8_t)(on ? 1u : 0u));
}

/* ---- Companion: yield until next frame arrives or timeout ---- */
void awbus_companion_wait_frame(awbus_t *bus, uint32_t timeout_ms) {
    if (bus->port->wait_frame)
        bus->port->wait_frame(bus->port->ctx, timeout_ms);
}
