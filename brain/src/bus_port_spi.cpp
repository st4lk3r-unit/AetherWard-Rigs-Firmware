#include <Arduino.h>
#include <SPI.h>
#include "aw_bus/aw_bus_port.h"
#include "aw_bus/aw_bus_types.h"
#include "board.h"

/* Pin tables — indexed by companion node_id (1-based, index 0 unused).
 * Entries are conditionally included up to AWBUS_COMPANION_COUNT so the
 * arrays resize automatically when flash.py injects a different count.
 * To extend beyond 8 companions, add more AWBUS_CS_PIN_N / AWBUS_READY_PIN_N
 * defaults in board.h and mirror them here. */
static const int kCSPins[AWBUS_COMPANION_COUNT + 1] = {
    -1,             /* [0] unused — node IDs are 1-based */
#if AWBUS_COMPANION_COUNT >= 1
    AWBUS_CS_PIN_0,
#endif
#if AWBUS_COMPANION_COUNT >= 2
    AWBUS_CS_PIN_1,
#endif
#if AWBUS_COMPANION_COUNT >= 3
    AWBUS_CS_PIN_2,
#endif
#if AWBUS_COMPANION_COUNT >= 4
    AWBUS_CS_PIN_3,
#endif
#if AWBUS_COMPANION_COUNT >= 5
    AWBUS_CS_PIN_4,
#endif
#if AWBUS_COMPANION_COUNT >= 6
    AWBUS_CS_PIN_5,
#endif
#if AWBUS_COMPANION_COUNT >= 7
    AWBUS_CS_PIN_6,
#endif
#if AWBUS_COMPANION_COUNT >= 8
    AWBUS_CS_PIN_7,
#endif
};
static const int kReadyPins[AWBUS_COMPANION_COUNT + 1] = {
    -1,
#if AWBUS_COMPANION_COUNT >= 1
    AWBUS_READY_PIN_0,
#endif
#if AWBUS_COMPANION_COUNT >= 2
    AWBUS_READY_PIN_1,
#endif
#if AWBUS_COMPANION_COUNT >= 3
    AWBUS_READY_PIN_2,
#endif
#if AWBUS_COMPANION_COUNT >= 4
    AWBUS_READY_PIN_3,
#endif
#if AWBUS_COMPANION_COUNT >= 5
    AWBUS_READY_PIN_4,
#endif
#if AWBUS_COMPANION_COUNT >= 6
    AWBUS_READY_PIN_5,
#endif
#if AWBUS_COMPANION_COUNT >= 7
    AWBUS_READY_PIN_6,
#endif
#if AWBUS_COMPANION_COUNT >= 8
    AWBUS_READY_PIN_7,
#endif
};

static SPISettings s_spi_settings(AWBUS_SPI_HZ, MSBFIRST, SPI_MODE0);

/* ---- Port callbacks ---- */

static int spi_transfer(void *ctx, uint8_t node_id,
                        const uint8_t *tx_buf, uint8_t *rx_buf, size_t len) {
    (void)ctx;
    if (node_id < 1 || node_id > AWBUS_COMPANION_COUNT) return AWBUS_ERR_IO;

    int cs = kCSPins[node_id];
    SPI.beginTransaction(s_spi_settings);
    digitalWrite(cs, LOW);

    if (rx_buf) {
        for (size_t i = 0; i < len; i++)
            rx_buf[i] = SPI.transfer(tx_buf ? tx_buf[i] : 0x00u);
    } else {
        for (size_t i = 0; i < len; i++)
            SPI.transfer(tx_buf ? tx_buf[i] : 0x00u);
    }

    digitalWrite(cs, HIGH);
    SPI.endTransaction();
    return AWBUS_OK;
}

/* Brain: read READY input for the given companion */
static int spi_ready(void *ctx, uint8_t node_id) {
    (void)ctx;
    if (node_id < 1 || node_id > AWBUS_COMPANION_COUNT) return AWBUS_ERR_IO;
    return digitalRead(kReadyPins[node_id]);
}

static void spi_reset_all(void *ctx) {
    (void)ctx;
    digitalWrite(AWBUS_RESET_PIN, LOW);
    delay(AWBUS_RESET_PULSE_MS);
    digitalWrite(AWBUS_RESET_PIN, HIGH);
}

/* ---- Public init ---- */

static awbus_port_t s_port = {
    .transfer   = spi_transfer,
    .ready      = spi_ready,
    .reset_all  = spi_reset_all,
    .wait_frame = NULL,  /* brain never blocks waiting for frames */
    .ctx        = NULL,
};

extern "C" awbus_port_t *brain_bus_port_init(void) {
    /* CS pins — default HIGH (deselected) */
    for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++) {
        pinMode(kCSPins[i], OUTPUT);
        digitalWrite(kCSPins[i], HIGH);
    }
    /* READY pins — INPUT_PULLDOWN: missing companion reads LOW (not ready)
       instead of floating HIGH and causing phantom polls */
    for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++)
        pinMode(kReadyPins[i], INPUT_PULLDOWN);

    /* Shared RESET — default HIGH (not reset) */
    pinMode(AWBUS_RESET_PIN, OUTPUT);
    digitalWrite(AWBUS_RESET_PIN, HIGH);

    SPI.begin(AWBUS_SPI_SCK_PIN, AWBUS_SPI_MISO_PIN, AWBUS_SPI_MOSI_PIN, -1);
    return &s_port;
}

/* ---- Brain heartbeat — 2 fast blinks per second ---- */
/* ON 80ms, OFF 80ms, ON 80ms, OFF 760ms = 1 s cycle, 2 blinks */

#if defined(BRAIN_HEARTBEAT) && BRAIN_HEARTBEAT

static uint32_t s_hb_last_ms = 0;
static int      s_hb_phase   = 3;   /* start in last OFF phase */
static const uint32_t s_hb_delays[4] = {80u, 80u, 80u, 760u};

extern "C" void brain_heartbeat_init(void) {
    pinMode(BRAIN_LED_GPIO, OUTPUT);
    digitalWrite(BRAIN_LED_GPIO, LOW);
    s_hb_last_ms = millis();
}

extern "C" void brain_heartbeat_tick(void) {
    uint32_t now = millis();
    if (now - s_hb_last_ms >= s_hb_delays[s_hb_phase]) {
        s_hb_phase   = (s_hb_phase + 1) & 3;
        s_hb_last_ms = now;
        /* phases 0 and 2 are ON, phases 1 and 3 are OFF */
        digitalWrite(BRAIN_LED_GPIO, (s_hb_phase == 0 || s_hb_phase == 2) ? HIGH : LOW);
    }
}

#endif /* BRAIN_HEARTBEAT */
