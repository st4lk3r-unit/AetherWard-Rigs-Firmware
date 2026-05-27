#pragma once

/* Companion — PRISM rig (Seeed XIAO ESP32C6, ESP-IDF framework)
 *
 * All pin macros are guarded with #ifndef so flash.py can inject any value
 * via -D flags without editing this file.  The defaults below reflect the
 * known-good PRISM wiring; edit prism.json (pins.companion) to change them.
 *
 * XIAO ESP32C6 GPIO mapping (from Seeed schematic):
 *
 *   Header  Function    GPIO   Note
 *   ──────  ──────────  ─────  ─────────────────────────────
 *   D0      Analog      GPIO0
 *   D1      Analog      GPIO1
 *   D2      Analog      GPIO2
 *   D3      Digital     GPIO21  SDIO_DATA1
 *   D4      SDA         GPIO22  SDIO_DATA2 / I2C
 *   D5      SCL         GPIO23  SDIO_DATA3 / I2C
 *   D6      TX          GPIO16  UART0 TX  ─┐ kept free
 *   D7      RX          GPIO17  UART0 RX  ─┘ for debug console
 *   D8      SCK         GPIO19  SPI_CLK
 *   D9      MISO        GPIO20  SPI_MISO
 *   D10     MOSI        GPIO18  SPI_MOSI
 *
 *   Internal (not on headers):
 *   GPIO3   WIFI_ENABLE — drive LOW to enable the RF switch IC
 *   GPIO14  WIFI_ANT    — LOW=onboard PCB antenna, HIGH=external U.FL
 *   GPIO15  User LED    — onboard LED (heartbeat)
 *
 * AW-Bus wiring for PRISM
 * ─────────────────────────────────────────────────────────────────────────────
 *  Brain GPIO         Companion GPIO   Direction   Signal
 *  ─────────────────  ───────────────  ──────────  ────────────────────────────
 *  GPIO14 (SCK)   ──► D8  GPIO19       brain→comp  shared SPI clock
 *  GPIO13 (MOSI)  ──► D10 GPIO18       brain→comp  shared MOSI
 *  GPIO27 (MISO)  ◄── D9  GPIO20       comp→brain  shared MISO
 *  GPIO15/25/4 CS ──► D3  GPIO21       brain→comp  individual chip-select
 *  GPIO32/33/26   ◄── D4  GPIO22       comp→brain  HIGH = data pending
 *  GPIO21 (RESET) ──► D2  GPIO2        brain→comp  LOW  = reboot companion
 */

#ifndef NEU_BOARD_NAME
#define NEU_BOARD_NAME         "companion-prism-xiao-c6"
#endif
#ifndef COMPANION_UART_PORT
#define COMPANION_UART_PORT    UART_NUM_0
#endif
#ifndef COMPANION_UART_BAUD
#define COMPANION_UART_BAUD    115200
#endif

/* SPI2 (FSPI) slave */
#ifndef AWBUS_SPI_SCK_PIN
#define AWBUS_SPI_SCK_PIN   19   /* D8  */
#endif
#ifndef AWBUS_SPI_MOSI_PIN
#define AWBUS_SPI_MOSI_PIN  18   /* D10  brain→companion */
#endif
#ifndef AWBUS_SPI_MISO_PIN
#define AWBUS_SPI_MISO_PIN  20   /* D9   companion→brain */
#endif
#ifndef AWBUS_SPI_CS_PIN
#define AWBUS_SPI_CS_PIN    21   /* D3   hardware CS input from brain */
#endif

/* Control signals */
#ifndef AWBUS_READY_PIN
#define AWBUS_READY_PIN     22   /* D4   output: HIGH = data pending */
#endif
#ifndef AWBUS_RESET_PIN
#define AWBUS_RESET_PIN      2   /* D2   input:  LOW  = reboot       */
#endif

/* RF antenna switch — internal PCB GPIOs (not on pin headers) */
#ifndef COMPANION_WIFI_ENABLE_GPIO
#define COMPANION_WIFI_ENABLE_GPIO   3   /* drive LOW to enable RF switch IC */
#endif
#ifndef COMPANION_ANT_GPIO
#define COMPANION_ANT_GPIO          14   /* LOW=onboard PCB ant, HIGH=ext U.FL */
#endif

/* User LED — onboard, not on pin headers.
 * Enable heartbeat blink with -DCOMPANION_HEARTBEAT=1 */
#ifndef COMPANION_LED_GPIO
#define COMPANION_LED_GPIO  15
#endif

/* Node ID — always injected by flash.py as -DAWBUS_NODE_ID=N.
 * The fallback here is a safety net for manual builds only. */
#ifndef AWBUS_NODE_ID
#define AWBUS_NODE_ID  1
#endif
