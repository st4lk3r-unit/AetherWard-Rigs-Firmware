#pragma once

/* Brain — PRISM rig (ESP32 DevKit, Arduino framework)
 *
 * All pin macros are guarded with #ifndef so flash.py can inject any value
 * via -D flags without editing this file.  The defaults below reflect the
 * known-good PRISM wiring; edit prism.json (pins.brain) to change them.
 *
 * Strapping-pin avoidance:
 *   GPIO12  — flash voltage strapping (HIGH at boot = 1.8 V → avoid)
 *   GPIO2   — bootloader entry strapping (must be LOW during upload → avoid)
 *   GPIO5   — SDIO strapping, driven at boot → avoid for active signals
 *   GPIO34–39 — input-only, no internal pull-up/down → avoid for READY lines
 *
 * READY lines need INPUT_PULLDOWN so a disconnected companion reads LOW
 * (not ready) rather than floating HIGH and triggering phantom polls.
 * GPIO32/33/26 all support internal pull-down.
 *
 * Wiring table (3-companion PRISM rig):
 *   Signal              Brain GPIO   Companion (XIAO ESP32C6)         Dir
 *   ──────────────────  ──────────   ─────────────────────────────── ──────────
 *   SPI SCK             14           D8  / GPIO19  (SPI_CLK)          brain→comp
 *   SPI MOSI            13           D10 / GPIO18  (SPI_MOSI)         brain→comp
 *   SPI MISO            27           D9  / GPIO20  (SPI_MISO)         comp→brain
 *   CS  companion-1     15           D3  / GPIO21                     brain→comp
 *   CS  companion-2     25           D3  / GPIO21                     brain→comp
 *   CS  companion-3      4           D3  / GPIO21                     brain→comp
 *   READY companion-1   32 (in)      D4  / GPIO22                     comp→brain
 *   READY companion-2   33 (in)      D4  / GPIO22                     comp→brain
 *   READY companion-3   26 (in)      D4  / GPIO22                     comp→brain
 *   RESET (shared)      21           D2  / GPIO2                      brain→comp
 *   GND                 GND          GND                              common
 */

#ifndef NEU_BOARD_NAME
#define NEU_BOARD_NAME         "brain-prism-esp32"
#endif
#ifndef NEU_UART_CONSOLE_IDX
#define NEU_UART_CONSOLE_IDX   0
#endif
#ifndef NEU_UART_CONSOLE_BAUD
#define NEU_UART_CONSOLE_BAUD  115200
#endif

/* SPI2 (HSPI) — CLK/MOSI stay on standard HSPI pins; MISO moved off GPIO12 */
#ifndef AWBUS_SPI_HOST
#define AWBUS_SPI_HOST    HSPI_HOST
#endif
#ifndef AWBUS_SPI_SCK_PIN
#define AWBUS_SPI_SCK_PIN  14
#endif
#ifndef AWBUS_SPI_MOSI_PIN
#define AWBUS_SPI_MOSI_PIN 13
#endif
#ifndef AWBUS_SPI_MISO_PIN
#define AWBUS_SPI_MISO_PIN 27
#endif

/* Default SPI clock — override via -DAWBUS_SPI_HZ=N at build time or flash.py.
 * 4 MHz works with all SPI slaves and any wiring length; go higher via flash.py
 * once signal quality is confirmed (40 MHz requires short, low-capacitance connections). */
#ifndef AWBUS_SPI_HZ
#define AWBUS_SPI_HZ  4000000UL
#endif

/* Companion chip-select pins (active LOW, one per companion)
 * Positions 3–7 have no PRISM default — assign them in prism.json if extending. */
#ifndef AWBUS_CS_PIN_0
#define AWBUS_CS_PIN_0   15
#endif
#ifndef AWBUS_CS_PIN_1
#define AWBUS_CS_PIN_1   25
#endif
#ifndef AWBUS_CS_PIN_2
#define AWBUS_CS_PIN_2    4
#endif
#ifndef AWBUS_CS_PIN_3
#define AWBUS_CS_PIN_3   -1
#endif
#ifndef AWBUS_CS_PIN_4
#define AWBUS_CS_PIN_4   -1
#endif
#ifndef AWBUS_CS_PIN_5
#define AWBUS_CS_PIN_5   -1
#endif
#ifndef AWBUS_CS_PIN_6
#define AWBUS_CS_PIN_6   -1
#endif
#ifndef AWBUS_CS_PIN_7
#define AWBUS_CS_PIN_7   -1
#endif

/* Companion READY input pins (INPUT_PULLDOWN, one per companion)
 * Positions 3–7 have no PRISM default — assign in prism.json if extending. */
#ifndef AWBUS_READY_PIN_0
#define AWBUS_READY_PIN_0  32
#endif
#ifndef AWBUS_READY_PIN_1
#define AWBUS_READY_PIN_1  33
#endif
#ifndef AWBUS_READY_PIN_2
#define AWBUS_READY_PIN_2  26
#endif
#ifndef AWBUS_READY_PIN_3
#define AWBUS_READY_PIN_3  -1
#endif
#ifndef AWBUS_READY_PIN_4
#define AWBUS_READY_PIN_4  -1
#endif
#ifndef AWBUS_READY_PIN_5
#define AWBUS_READY_PIN_5  -1
#endif
#ifndef AWBUS_READY_PIN_6
#define AWBUS_READY_PIN_6  -1
#endif
#ifndef AWBUS_READY_PIN_7
#define AWBUS_READY_PIN_7  -1
#endif

/* Shared active-low RESET output */
#ifndef AWBUS_RESET_PIN
#define AWBUS_RESET_PIN       21
#endif
#ifndef AWBUS_RESET_PULSE_MS
#define AWBUS_RESET_PULSE_MS  10
#endif

/* Number of companions — injected by flash.py as -DAWBUS_COMPANION_COUNT=N.
 * The default here covers manual builds without flash.py. */
#ifndef AWBUS_COMPANION_COUNT
#define AWBUS_COMPANION_COUNT  3
#endif

/* Onboard LED — GPIO2 (blue LED on most ESP32 DevKit boards) */
#ifndef BRAIN_LED_GPIO
#define BRAIN_LED_GPIO  2
#endif

/* Rig identity — override via build flags or flash.py */
#ifndef AWBUS_RIG_NAME
#define AWBUS_RIG_NAME     "UNKNOWN"
#endif
#ifndef AWBUS_RIG_REVISION
#define AWBUS_RIG_REVISION "0.0"
#endif
