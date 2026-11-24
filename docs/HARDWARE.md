# 默认硬件与接线

ESP32-S3 N16R8，16 MiB Flash、8 MiB octal PSRAM，240 MHz。ESP-IDF v5.5.6：<https://github.com/espressif/esp-idf/tree/v5.5.6>。使用供电稳定的数据 USB 线。

| 设备 | 信号 | GPIO |
| --- | --- | --- |
| INMP441 | SCK / WS / SD | 5 / 4 / 6 |
| INMP441 | L/R | GND（左声道） |
| MAX98357A | BCLK / LRC / DIN | 15 / 16 / 7 |
| ST7789 240×240 | SCL / SDA / RES | 21 / 47 / 45 |
| ST7789 240×240 | DC / CS / BLK | 40 / 41 / 42 |
| WS2812 RGB | DATA | 48（配置页可改为 38） |
| BOOT 按键 | 低电平有效 | 0 |

ST7789 以 SPI 连接，SCL/SDA 在这里指 SPI CLK/MOSI。各模块共地，电源电压按模块规格连接。扬声器功放的数字引脚在非播报期间保持静音，默认音量 50%。

BOOT 短按暂停/继续；长按两秒进入热点配置。RST 仅重启，保留 NVS 参数。不要在重启时一直按住 BOOT，以免进入 ROM 下载模式。

示例使用单应用分区，不提供 OTA。烧录脚本只写 manifest 声明的镜像，不默认擦除 NVS。首次跨项目使用若 NVS 不兼容，可显式选择清空整片 Flash；这会删除已有设备配置。不要复制别人设备的 NVS、Wi-Fi 密码或 API Key。
