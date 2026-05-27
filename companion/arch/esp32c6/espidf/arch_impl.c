#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "neutrino/arch_api.h"
#include "board.h"

static uint32_t s_start_ms = 0;

static int espidf_init(void) {
    s_start_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

    uart_config_t cfg = {
        .baud_rate           = COMPANION_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    uart_driver_install(COMPANION_UART_PORT, 1024, 0, 0, NULL, 0);
    uart_param_config(COMPANION_UART_PORT, &cfg);
    uart_set_pin(COMPANION_UART_PORT,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return 0;
}

static void espidf_delay_ms(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

static uint32_t espidf_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000LL) - s_start_ms;
}

static int espidf_uart_init(int idx, uint32_t baud) {
    (void)idx; (void)baud;
    return 0;  /* done in espidf_init */
}

static int espidf_uart_write(int idx, const void *buf, size_t len) {
    int n = uart_write_bytes((uart_port_t)idx, (const char *)buf, len);
    return (n >= 0) ? n : 0;
}

static int espidf_uart_read(int idx, void *buf, size_t len) {
    int n = uart_read_bytes((uart_port_t)idx, (uint8_t *)buf, (uint32_t)len, 0);
    return (n >= 0) ? n : 0;
}

static const arch_api_t API = {
    .init       = espidf_init,
    .delay_ms   = espidf_delay_ms,
    .millis     = espidf_millis,
    .uart_init  = espidf_uart_init,
    .uart_write = espidf_uart_write,
    .uart_read  = espidf_uart_read,
};

const arch_api_t *arch_api(void) { return &API; }
