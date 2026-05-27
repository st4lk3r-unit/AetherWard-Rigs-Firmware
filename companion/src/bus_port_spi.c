#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "aw_bus/aw_bus.h"
#include "aw_bus/aw_bus_port.h"
#include "aw_bus/aw_bus_types.h"
#include "board.h"

#define TAG "awbus-spi"

/*
 * SPI slave scheduling model
 * --------------------------
 * The ESP-IDF slave driver only receives a master transaction when a slave
 * transaction has already been queued.  The previous patch fixed DMA ownership,
 * but it still left a small gap after a reply was clocked out: the companion
 * task had to wake, reap the brain's NULL poll frame, and queue the next idle RX
 * before the brain started the next command.  In tight benchmarks the brain can
 * start immediately, so every second command is sent while no slave transaction
 * is armed -> deterministic 50/50 timeout.
 *
 * This version keeps the slave pipelined with two descriptors:
 *
 *   idle RX queued
 *     brain command completes -> task reaps command
 *     task queues reply TX first, then an idle RX tail, then raises READY
 *     brain polls reply       -> reply descriptor completes, READY drops
 *     next brain command      -> already-cued idle tail receives it, even if
 *                                the companion task has not reaped the poll yet
 *
 * queue_size=2 is therefore intentional.  Each descriptor owns its own TX/RX
 * buffers, and buffers are only read/reused after spi_slave_get_trans_result().
 */

typedef enum {
    SLOT_FREE  = 0,
    SLOT_IDLE  = 1,
    SLOT_REPLY = 2,
} slot_kind_t;

typedef struct {
    spi_slave_transaction_t trans;
    uint8_t tx[AWBUS_FRAME_SIZE] __attribute__((aligned(4)));
    uint8_t rx[AWBUS_FRAME_SIZE] __attribute__((aligned(4)));
    volatile uint8_t queued;
    volatile uint8_t kind;
} spi_slot_t;

static spi_slot_t  s_slot[2];
static TaskHandle_t s_task = NULL;
static volatile int s_done_count = 0;

static void make_null_frame(uint8_t *out) {
    awbus_frame_t f = {0};
    f.node_id = (uint8_t)AWBUS_NODE_ID;
    f.cmd     = AWBUS_CMD_NULL;
    awbus_serialize(&f, out);
}

static int slot_index_from_trans(const spi_slave_transaction_t *t) {
    for (int i = 0; i < 2; i++) {
        if (t == &s_slot[i].trans)
            return i;
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < 2; i++) {
        if (!s_slot[i].queued)
            return i;
    }
    return -1;
}

static int has_queued_idle(void) {
    for (int i = 0; i < 2; i++) {
        if (s_slot[i].queued && s_slot[i].kind == SLOT_IDLE)
            return 1;
    }
    return 0;
}

static int queue_slot(slot_kind_t kind, const uint8_t *tx_wire) {
    int idx = find_free_slot();
    if (idx < 0)
        return AWBUS_ERR_IO;

    spi_slot_t *s = &s_slot[idx];

    if (kind == SLOT_REPLY && tx_wire) {
        memcpy(s->tx, tx_wire, AWBUS_FRAME_SIZE);
    } else {
        make_null_frame(s->tx);
        kind = SLOT_IDLE;
    }

    memset(s->rx, 0, sizeof s->rx);
    memset(&s->trans, 0, sizeof s->trans);
    s->trans.length    = AWBUS_FRAME_SIZE * 8u;
    s->trans.tx_buffer = s->tx;
    s->trans.rx_buffer = s->rx;

    /* Mark before queueing; the callback can run as soon as the master clocks. */
    s->kind   = (uint8_t)kind;
    s->queued = 1;

    esp_err_t err = spi_slave_queue_trans(SPI2_HOST, &s->trans, portMAX_DELAY);
    if (err != ESP_OK) {
        s->queued = 0;
        s->kind   = SLOT_FREE;
        ESP_LOGE(TAG, "spi_slave_queue_trans failed: 0x%x", err);
        return AWBUS_ERR_IO;
    }

    return AWBUS_OK;
}

static int queue_idle_if_needed(void) {
    if (has_queued_idle())
        return AWBUS_OK;
    return queue_slot(SLOT_IDLE, NULL);
}

static void IRAM_ATTR post_trans_cb(spi_slave_transaction_t *t) {
    int idx = slot_index_from_trans(t);

    /* READY means "a reply descriptor is waiting for the brain to clock it".
     * Drop it as soon as that reply transaction finishes.  For IDLE transactions
     * this is harmless because READY should already be low. */
    if (idx < 0 || s_slot[idx].kind == SLOT_REPLY)
        gpio_set_level(AWBUS_READY_PIN, 0);

    s_done_count++;

    BaseType_t woken = pdFALSE;
    if (s_task)
        vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static int reap_completed(uint8_t *rx_buf, size_t len) {
    if (s_done_count <= 0)
        return AWBUS_ERR_NODATA;

    spi_slave_transaction_t *done = NULL;
    esp_err_t err = spi_slave_get_trans_result(SPI2_HOST, &done, 0);
    if (err != ESP_OK)
        return AWBUS_ERR_NODATA;

    int idx = slot_index_from_trans(done);
    if (idx < 0) {
        ESP_LOGE(TAG, "completed unknown SPI transaction");
        return AWBUS_ERR_IO;
    }

    spi_slot_t *s = &s_slot[idx];

    if (rx_buf) {
        size_t n = (len < AWBUS_FRAME_SIZE) ? len : AWBUS_FRAME_SIZE;
        memcpy(rx_buf, s->rx, n);
    }

    s->queued = 0;
    s->kind   = SLOT_FREE;
    if (s_done_count > 0)
        s_done_count--;

    return AWBUS_OK;
}

/* ---- Port callbacks ---- */

static int spi_transfer(void *ctx, uint8_t node_id,
                        const uint8_t *tx_buf, uint8_t *rx_buf, size_t len) {
    (void)ctx;
    (void)node_id;

    if (len != AWBUS_FRAME_SIZE)
        return AWBUS_ERR_IO;

    if (tx_buf) {
        /* Queue the reply first, then an idle RX tail.  The tail removes the
         * post-reply gap that caused deterministic 50/50 ping-pong failures. */
        int rc = queue_slot(SLOT_REPLY, tx_buf);
        if (rc != AWBUS_OK)
            return rc;

        rc = queue_idle_if_needed();
        if (rc != AWBUS_OK)
            ESP_LOGW(TAG, "reply queued but idle tail queue failed: %d", rc);

        return AWBUS_OK;
    }

    if (rx_buf) {
        /* Consume exactly one completed master transaction. */
        return reap_completed(rx_buf, len);
    }

    /* tx_buf == NULL && rx_buf == NULL: explicit idle-arm request. */
    return queue_idle_if_needed();
}

static int spi_ready(void *ctx, uint8_t level) {
    (void)ctx;
    gpio_set_level(AWBUS_READY_PIN, level ? 1 : 0);
    return AWBUS_OK;
}

static void spi_reset_all(void *ctx) {
    (void)ctx;  /* companion is the peripheral — nothing to reset */
}

static void spi_wait_frame(void *ctx, uint32_t timeout_ms) {
    (void)ctx;
    if (s_done_count > 0)
        return;

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1u);
    if (ticks == 0)
        ticks = 1;
    ulTaskNotifyTake(pdTRUE, ticks);
}

static awbus_port_t s_port = {
    .transfer   = spi_transfer,
    .ready      = spi_ready,
    .reset_all  = spi_reset_all,
    .wait_frame = spi_wait_frame,
    .ctx        = NULL,
};

/* ---- Public init ---- */

awbus_port_t *companion_bus_port_init(void) {
    /* Record the owner task before the first transaction can complete. */
    s_task = xTaskGetCurrentTaskHandle();

    gpio_config_t ready_cfg = {
        .pin_bit_mask = (1ULL << AWBUS_READY_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&ready_cfg);
    gpio_set_level(AWBUS_READY_PIN, 0);

    gpio_config_t reset_cfg = {
        .pin_bit_mask = (1ULL << AWBUS_RESET_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&reset_cfg);

    spi_bus_config_t bus = {
        .mosi_io_num   = AWBUS_SPI_MOSI_PIN,
        .miso_io_num   = AWBUS_SPI_MISO_PIN,
        .sclk_io_num   = AWBUS_SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_slave_interface_config_t slv = {
        .mode          = 0,
        .spics_io_num  = AWBUS_SPI_CS_PIN,
        .queue_size    = 2,
        .flags         = 0,
        .post_trans_cb = post_trans_cb,
    };

    esp_err_t err = spi_slave_initialize(SPI2_HOST, &bus, &slv, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize failed: 0x%x", err);
        return NULL;
    }

    memset(s_slot, 0, sizeof s_slot);
    s_done_count = 0;

    if (queue_idle_if_needed() != AWBUS_OK) {
        ESP_LOGE(TAG, "initial idle RX arm failed");
        return NULL;
    }

    ESP_LOGI(TAG, "SPI2 slave ready — node_id=%d  cs=%d  ready=%d  reset=%d",
             AWBUS_NODE_ID, AWBUS_SPI_CS_PIN, AWBUS_READY_PIN, AWBUS_RESET_PIN);
    return &s_port;
}
