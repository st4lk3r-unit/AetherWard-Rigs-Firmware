### PROTO-PRISM-001

**Brain :** ESP32

**Companion :** XIAO ESP32C6 

#### SPI lanes

1 SPI lane for 3 companions.

N SPI lane per brain.

#### Hardware

##### Blades

Blades are board which contain 3 companion and an optimized wiring to allow reaching higher freq with more stability.

*blade pinount* :
|Signal|Blade GPIO|Companion|Dir|
|-----------|------|------------|--------|
|SPI SCK|0|D8/GPIO19(SPI_CLK)|brain→comp|
|SPI MOSI|1|D10/GPIO18(SPI_MOSI)|brain→comp|
|SPI MISO|2|D9/GPIO20(SPI_MISO)|comp→brain|
|CS companion-1|5|D3/GPIO21|brain→comp|
|CS companion-2|7|D3/GPIO21|brain→comp|
|CS companion-3|9|D3/GPIO21|brain→comp|
|READY companion-1|6|D4/GPIO22|comp→brain|
|READY companion-2|8|D4/GPIO22|comp→brain|
|READY companion-3|10|D4/GPIO22|comp→brain|
|RESET (shared)|3|D2/GPIO2|brain→comp|
|GND|4|GND|common|


*blade to brain* :
|Signal|Blade GPIO|Board GPIO|Dir|
|-----------|------|------------|--------|
|SPI SCK|0|14|brain→comp|
|SPI MOSI|1|13|brain→comp|
|SPI MISO|2|27|comp→brain|
|CS companion-1|5|15|brain→comp|
|CS companion-2|7|25|brain→comp|
|CS companion-3|9|4|brain→comp|
|READY companion-1|32|6|comp→brain|
|READY companion-2|33|8|comp→brain|
|READY companion-3|26|10|comp→brain|
|RESET (shared)|3|21|brain→comp|
|GND|4|GND|common|
