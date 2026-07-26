### ESP32-S3 ↔ Si4463 硬件连接

| ESP32-S3 GPIO | 连接方向 | Si4463 引脚 | 功能说明          |
|---------------|--------------|---------------|-------------------|
| GPIO16        | →       | GPIO0       | TX 数据线         |
| GPIO11        | →       | SDI (MOSI)  | SPI MOSI          |
| GPIO12        | →       | SCLK        | SPI 时钟          |
| GPIO10        | →       | nSEL        | SPI 片选 (CS)     |
| GPIO13        | ←       | SDO (MISO)  | SPI MISO          |
| GPIO5         | →       | SDN         | 关断控制 (Shutdown) |
| GPIO4         | ←       | IRQ         | 中断输出           |

### SI4463

| 左侧引脚   | 右侧引脚 |
|------------|----------|
| VDD        | GND      |
| GPIO0      | SDN      |
| GPIO1      | GPIO3    |
| IRQ        | GPIO2    |
| SCK        | NSEL     |
| MISO       | MOSI     |


### ESP32-S3 CORE

| 左侧引脚   | 右侧引脚  |
|------------|-----------|
| GND        | GND       |
| GPIO1      | 3.3V      |
| GPIO2      | GPIO18    |
| GPIO3      | GPIO17    |
| GPIO4      | GPIO16    |
| GPIO5      | GPIO15    |
| GND        | GPIO14    |
| GPIO6      | GPIO13    |
| GPIO7      | GND       |
| GPIO8      | 3.3V      |
| GPIO9      | GPIO12    |
| REST       | GPIO11    |
| 3.3V       | GPIO10    |
| GND        | GPIO0     |
| PWB        | 5V        |
| 5V         | GND       |