# ZLTech 1.54 TFT（小智 1.54 寸屏板）

Target: `zltech_154tft`；应用镜像：`zltech_154tft.bin`。
ESP-IDF: v6.1。Flash: 16 MB；PSRAM: 8 MB（octal）。屏幕：240 × 240 ST7789（SPI）。

```text
eim run "idf.py -C firmware/targets/zltech_154tft build" v6.1
eim run "idf.py -C firmware/targets/zltech_154tft -p <PORT> flash" v6.1
eim run "idf.py -C firmware/targets/zltech_154tft merge-bin -o zltech_154tft_full.bin" v6.1
```

证据等级：**hardware bring-up**。固件在 ESP-IDF v6.1 下编译通过，`zltech_154tft.bin` 为
0xc60c0 字节（7 MB 应用分区剩余 89%）。设备已整片烧录并启动：新分区表生效、8 MB PSRAM 识别正常、
ADC 校准成功、ST7789 面板初始化完成并点亮 70% 背光、BLE 以 `QF-ZLT-154-FE16` 广播。
Bridge 已建立 Secure Connections 连接并推送 `usage.v1`，面板渲染出余额与配额；
顶部三键的按键事件从串口日志确认。OTA 流程、电量标定与音频仍待实机确认，之后才可称为 hardware-verified。
下面的引脚表来自运行态取证，不是抄厂商文档。

设备以 `QF-ZLT-154-` 加蓝牙 MAC 末两字节广播，鉴权后的 status 报告 `zltech_154tft`。
用量发布、时间同步、翻页命令与 OTA 复用共享的 BLE 组件，协议与其他目标一致。

## 板卡来源

这块板出厂预装第三方「小智」固件（`xiaozhi_TY_1.54_194.1.1S`，project name 自报
`zltech-1.54tft-wifi`）。其硬件与开源的星智 cube 1.54 TFT Wi-Fi 参考设计同源，引脚一致。

本 target **整片重写**：分区表按本项目自己的双 OTA 布局设计，不保留出厂镜像的分区偏移，
也不保留其 `model`（语音资源）分区。烧录 `zltech_154tft_full.bin` 会覆盖整片 Flash，
出厂固件不可恢复，除非事先备份。

## 引脚

引脚不是从厂商文档抄的，而是从板载运行中的固件里读出来的：用 OpenOCD 在芯片运行时
halt，读 GPIO 矩阵的 `GPIO_FUNCn_OUT_SEL_CFG` 与 `IO_MUX_GPIO_PINn_REG`，再与公开参考设计
比对。实测得到的映射：

| 信号 | GPIO | 取证方式 |
| --- | --- | --- |
| LCD SCLK | 9 | 矩阵 `OUT_SEL = 66`（SPI3_CLK） |
| LCD MOSI | 10 | 矩阵 `OUT_SEL = 68`（SPI3_D） |
| LCD CS | 14 | 矩阵 `OUT_SEL = 71`（SPI3_CS0） |
| LCD 背光 | 13 | 矩阵 `OUT_SEL = 73`（LEDC 通道 0，PWM 调光） |
| LCD DC | 8 | 参考设计同款 |
| LCD RST | 18 | 参考设计同款 |
| KEY 上一页 | 39 | 参考设计同款（VOL-） |
| KEY 回主页 | 0 | 参考设计同款（BOOT，与 strapping 复用） |
| KEY 下一页 | 40 | 参考设计同款（VOL+） |
| 电池采样 | 17 | ADC2 通道 6 |

全部集中在 [`main/board_config.hpp`](main/board_config.hpp)，换板只改这一个文件。
电池电压经 1:2 分压，按 3.3–4.15 V 线性映射为百分比——这是电压代理，不是电量计。

出厂固件把 I2S 扬声器（BCLK 15 / WS 16 / DOUT 7）与麦克风（WS 4 / SCK 5 / DIN 6）挂在
ES8311 上；本 target 不用音频通路，这些脚空闲。

## 显示

240 × 240 正方形屏，所以没有横竖屏两套布局：整页按 220 px 单栏排版，Settings 里的
ROTATION 只把成品画面在玻璃上转 0°/90°/180°，逻辑坐标不变。页面环在三种方向下一致
（Home → Deepseek → Minimax → Music → Clock），按键在任何方向都能翻到全部页面。

屏幕是彩色 ST7789，但这一版所有界面都是一位图，因此每帧从画布展开成黑白 RGB565：
`1` → 黑，`0` → 白。传输按 20 行一带走内部 DMA 缓冲，一帧 12 带、共 115 KB，
40 MHz 下约 30 ms；帧间隔 250 ms，内容不变则整帧不下发。

与 4.2 寸反射屏一致，界面刻意保留分段液晶的观感：

- 数字（用量百分比、时钟、配对码、OTA 进度）用七段数码管绘制，每个数字固定格宽；
- 百分比使用「百位半格 + 两个右对齐格 + 百分号」的固定字段，避免数值变化时抖动；
- 中文（歌曲名、歌手、歌词）用 12 × 12 点阵在 2 倍缩放下渲染，与内置 ASCII 字体同基线。

进度条是一段实心填充加一条 pace 刻度（白色缝隙 + 竖线），刻度收在条内，不吃边框。

排版可以在主机上预览，不需要设备：

```text
g++ -std=c++17 -O2 -Ifirmware/components/usage_core/include \
    -Ifirmware/components/usage_panel/include \
    -Ifirmware/targets/zltech_154tft/main tools/zl154/render_preview.cpp \
    firmware/targets/zltech_154tft/main/screens.cpp \
    firmware/components/usage_panel/src/{canvas,cjk_font,cjk_font_data,panel_logic,screen_parts}.cpp \
    firmware/components/usage_core/src/usage_state.cpp -o zl154_preview
./zl154_preview <输出目录>
python3 tools/rlcd42/pbm_to_png.py <输出目录> --scale 2
```

## 分区布局

16 MB 全部重新规划，双 OTA 各 7 MB：

| 分区 | 偏移 | 大小 |
| --- | --- | --- |
| nvs | 0x9000 | 24 KB |
| otadata | 0xf000 | 8 KB |
| phy_init | 0x11000 | 4 KB |
| ota_0 | 0x20000 | 7 MB |
| ota_1 | 0x720000 | 7 MB |
| storage | 0xe20000 | 1920 KB |

`CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_MAX_TRANSFER_BYTES` 等于 7 MB + 512 B 的 manifest，
与 Registry 的 `ota_partition_bytes` 对应。

## 与其他目标的差异

| 项目 | 本目标 | Waveshare RLCD 4.2 |
| --- | --- | --- |
| 屏幕 | 240 × 240 ST7789，彩色，背光可调 | 400 × 300 ST7305，黑白反射 |
| 布局 | 单栏 220 px | 横屏双栏 / 竖屏两套 |
| RTC | 无，时间来自 `usage.v1` 同步，掉电丢失 | PCF85063A |
| 温湿度 | 无，显示占位符 | SHTC3 |
| 提示音 | **静音**：不初始化 ES8311，设置项保留但无声 | ES8311 |
| 按键 | 三键：左 = 上一页（GPIO39），中 = 回主页（GPIO0），右 = 下一页（GPIO40）；任一键长按 800 ms 进设置 | KEY = GPIO18，BOOT = GPIO0 |

## 硬件验收清单

按顺序确认，逐项记入 [verification.md](../../../docs/verification.md)：

1. 上电后背光点亮、画面方向正确（必要时只改 `screens.cpp` 的 ROTATION 或 `board_config.hpp`）；
2. 顶部三键：左键上一页、中键回主页、右键下一页；任一键长按 800 ms 进入设置；
3. Bridge 配对：屏幕显示 6 位配对码，输入后进入 LINKED；
4. 真实 `usage.v1` 数据下的主页、DeepSeek 页、MiniMax 页；
5. 音乐页歌词滚动与中文点阵；
6. 电量读数与真实电池电压对照（校 `board_config.hpp` 的分压与空满点）；
7. OTA：`ota.folder.v1` 全流程 + 物理确认 + 回滚确认。
