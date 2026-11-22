# Waveshare ESP32-S3-Touch-AMOLED-1.8（V1：SH8601 + FT3168）

Target: `waveshare_amoled_18`；应用镜像：`ws_amoled_18.bin`。
ESP-IDF: v6.1。Flash: 16 MB；PSRAM: 8 MB（octal）。屏幕：368 × 448 SH8601（QSPI AMOLED）。

```text
eim run "idf.py -C firmware/targets/waveshare_amoled_18 build" v6.1
eim run "idf.py -C firmware/targets/waveshare_amoled_18 -p <PORT> flash" v6.1
eim run "idf.py -C firmware/targets/waveshare_amoled_18 merge-bin -o ws_amoled_18_full.bin" v6.1
```

证据等级：**hardware bring-up**。固件在 ESP-IDF v6.1 下编译通过，`ws_amoled_18.bin` 为
0xc9020 字节（7 MB 应用分区剩余 89%）。已在一台 V1 板上整片擦除并烧录：v6.1 引导、
新分区表、8 MB PSRAM、PCF85063A RTC、AXP2101 PMIC（IC 类型校验通过）、FT3168 触摸
（`touch=1`）与 SH8601 面板初始化均从串口日志确认，BLE 以 `QF-WS-AMO18-` 前缀广播。
触摸滑动翻页、Bridge 配对后的 `usage.v1` 渲染与 OTA 仍待实机确认，之后才可称为
hardware-verified。

本 target 只适配 **V1** 硬件（SH8601 + FT3168）。V2 版（CO5300 + CST820）是另一块屏，
引脚协议不同，不在本 target 覆盖范围内。

## 输入：触摸左右滑动切页

页面导航来自触摸面板（FT3168，I2C 0x38，FT5x06 寄存器协议）：

| 手势 | 动作 |
| --- | --- |
| 向左滑动 ≥ 60 px（横竖行程 ≥ 2:1，≤ 1.2 s） | 下一页（Forward） |
| 向右滑动 | 上一页（Back） |
| 轻点（≤ 0.5 s，位移 < 24 px） | 回主页（Home） |
| 原地按住 ≥ 800 ms | 进设置页（Hold，与按键长按一致） |

第二根手指按下即取消本次手势；长按已上报后松手不再补发。滑动方向按 Settings 里的
ROTATION 设置映射（0°/90°/180°），保证「向左滑 = 下一页」在任何方向下都符合玻璃上的
直觉。BOOT 键（GPIO0）保留：短按 = 回主页，长按 800 ms = 进设置。

## 板卡与引脚

引脚映射来自厂商第一方示例（`13_display_colorbar`）与 BSP 组件
`waveshare/esp32_s3_touch_amoled_1_8`：

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| LCD SCLK | 11 | QSPI |
| LCD CS | 12 | QSPI |
| LCD D0–D3 | 4 / 5 / 6 / 7 | QSPI 数据线 |
| I2C SDA / SCL | 15 / 14 | 触摸、PMIC、RTC、电源扩展器共用 |
| 触摸 INT | 21 | FT3168（本 target 只轮询，不挂中断） |
| BOOT 键 | 0 | 与 strapping 复用 |
| ES8311 | MCLK 16 / BCLK 9 / WS 45 / DO 8 / DI 10 / PA 46 | 本 target 未启用 |
| microSD | CMD 1 / CLK 2 / D0 3 | 本 target 未启用 |

屏幕电源不在 ESP32 直控引脚上，而是挂在 I2C 扩展器 XCA9554/TCA9554（0x20）的 0/1/2 三根
线上：板级初始化先拉低 20 ms 再拉高（与厂商示例一致），之后 SH8601 才做软件复位。

AXP2101（0x34）带电量计：百分比直接读 0xA4 寄存器，是电量计读数而不是电压代理；
VBUS 在位状态可读，但与现有共享运行时的 OTA 供电门（`OtaSession::set_power_source`）
之间还没有接线，与其他维护目标保持一致，OTA 供电门是后续工作。

## 显示

SH8601 通过 `espressif/esp_lcd_sh8601` 组件驱动（QSPI 40 MHz，RGB565）。这个目标用彩色画布：
368 × 448 的 RGB565 帧缓冲直接占 PSRAM（约 322 KB），面板原样发送，不再有逐帧的 1 → 16 位展开；
最近一次提交的帧另存一份（同样在 PSRAM），与画布逐字节相同就不重发。亮度走面板自身的 CABC
命令（0x51），无背光 PWM；默认 80%。

画布把四种墨色映射成玻璃上的颜色，调色板在 `main/canvas.cpp`：纸白底、近黑正文、琥珀色表示
"正在处理"、青绿表示"在运行且空闲"、冷灰用于计数与细线。其余三个目标仍是 1 位画布，除白色以外
的墨色在它们上面一律画成黑色，所以同一份版面代码在反射屏和 TFT 上仍然成立。

页面环与其他目标一致：Home → Deepseek → Minimax → Sessions → Music → Clock，Settings 在
环外。会话页只在收到第一条会话数据后进入页面环，没有会话数据时环上仍是五页。上块固定顶部、
下块（页脚、页码点、主页 MiniMax 块）锚定玻璃底边，三种 ROTATION 设置都能完整落在
368 × 448 的玻璃内。

## 分区布局

16 MB 全部重新规划，双 OTA 各 7 MB（与 `zltech_154tft` 相同）：

| 分区 | 偏移 | 大小 |
| --- | --- | --- |
| nvs | 0x9000 | 24 KB |
| otadata | 0xf000 | 8 KB |
| phy_init | 0x11000 | 4 KB |
| ota_0 | 0x20000 | 7 MB |
| ota_1 | 0x720000 | 7 MB |
| storage | 0xe20000 | 1920 KB |

`CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_MAX_TRANSFER_BYTES` 等于 7 MB + 512 B 的 manifest，
与 Registry 的 `ota_partition_bytes` 对应。烧录 `ws_amoled_18_full.bin` 会覆盖整片
Flash，出厂固件不可恢复，除非事先备份。

## 与其他目标的差异

| 项目 | 本目标 | Waveshare RLCD 4.2 | ZLTech 1.54 TFT |
| --- | --- | --- | --- |
| 屏幕 | 368 × 448 SH8601 QSPI AMOLED | 400 × 300 ST7305 反射 LCD | 240 × 240 ST7789 |
| 输入 | 触摸滑动/轻点/长按 + BOOT 键 | KEY / BOOT 双键 | 顶部三键 |
| RTC | PCF85063A | PCF85063A | 无 |
| 电量 | AXP2101 电量计 | ADC 分压 | ADC 分压 |
| 提示音 | 静音（ES8311 未接线） | ES8311 | 静音 |
| Claude Code 会话页 | ✅ `sessions.v1` | ✅ `sessions.v1` | 不声明 |

## 硬件验收清单

按顺序确认，逐项记入 [verification.md](../../../docs/verification.md)：

1. 上电后画面点亮、方向正确、亮度合适（必要时调 `sh8601.cpp` 的默认亮度）；
2. 触摸：向左滑下一页、向右滑上一页、轻点回主页、长按进设置；翻转 ROTATION 后方向随动；
3. BOOT 键：短按回主页、长按 800 ms 进设置；
4. Bridge 配对：屏幕显示 6 位配对码，输入后进入 LINKED；
5. 真实 `usage.v1` 数据下的主页、DeepSeek 页、MiniMax 页；
6. 会话页：Bridge 下发 `sessions.v1` 后页面进入环，顶部计数、状态色条、会话名与忙 / 闲芯片互不重叠；
7. 电量读数与 AXP2101 电量计读数对照（`battery_percent`）；
8. OTA：`ota.folder.v1` 全流程 + 物理确认 + 回滚确认。
