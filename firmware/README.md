# Firmware

固件使用原生 ESP-IDF，不使用 PlatformIO。当前锁定并验证的组合是：

- ESP-IDF v6.1（由 EIM 管理）；
- esp-desktop-buddy 固定到`b6bac05db208717676e70180e5269d79f32b2d68`。


Git 来源组件的锁文件哈希会受 checkout 行尾配置影响。本项目的 LF 基准、证据等级、排查与更新流程见 [Firmware 依赖锁文件与 Git 行尾](../docs/firmware-dependency-locks.md)。`core.autocrlf` 引起的文件字节差异不应被误判为操作系统限制。

## 共享组件与设备代码

`components/` 中的代码可由不同设备目标复用：

- `usage_core`：用量数据、最后有效值、连接和数据可用状态和倒计时格式；
- `usage_protocol`：`usage.v1` 命令解析与严格字段校验；
- `usage_json`：原始 JSON、UTF-8、重复键及名称/版本边界校验；
- `usage_ota`：物理确认、ESP-IDF OTA 写入/校验、rollback 状态和 Folder Push OTA sink；
- `usage_ble`：事件队列、BLE Secure Connections、Bonding、协议处理和状态响应；
- `usage_rtc`：PCF85063A 本地日历读写与 BCD 编解码，挂在目标自有的 I2C 总线上；
- `usage_panel`：面板视图模型、页面环与导航、配额 pace 与告警、一位图绘制表面、点阵字体和屏幕通用片段；
- `usage_panel_app`：所有面板共用的应用：BLE 链路、OTA 会话、事件循环、渲染节奏，以及目标实现的 `PanelTarget` 接口；
- `esp_desktop_buddy_folder_push`：固定到上游 `b6bac05d` 的 Folder Push 源码快照；

Folder Push 本地快照原因见 [UPSTREAM.md](components/esp_desktop_buddy_folder_push/UPSTREAM.md)；核心 Buddy 和 BLE transport 由 Component Manager 锁定获取。


| 目标设备 | 状态 | UI / 输入栈 |
| --- | --- | --- |
| ESP32-S3-RLCD-4.2 | 已维护 | 自绘帧缓冲、ST7305 驱动、KEY / BOOT 按键 |
| ZLTech 1.54 TFT | hardware bring-up | 自绘帧缓冲、ST7789 驱动、KEY = VOL+ / BOOT 按键、背光 PWM |

各目标的实机验收范围见 [verification.md](../docs/verification.md)。

维护中的硬件目标统一登记在 `bridge/src/quotaframe_bridge/targets.py`，用于 CI、Release、镜像结构和分区容量校验。Bridge 基础发现使用公共 QF 名称空间和 NUS，运行时名称来自 status，常驻会话来自 `devices.json`；第三方设备基础接入无需加入 registry。`scripts/target_registry.py` 输出官方构建与发布 matrix。

新增或移植硬件目标时，先阅读 [Hardware Target Porting Guide](../docs/PORTING.md)。其中定义了 target 目录与 Registry 契约、共享/板级边界、分区与 OTA 要求、target test app 接入方式，以及 build-only 与 hardware-verified 的证据边界；可复制的最小工程骨架位于 `docs/porting/minimal-target/`。

## 从源码构建

首次安装无需 ESP-IDF，可使用项目首页的 Web 烧录器，或按下文「烧录和串口日志」写入 Release 完整镜像。

开发者从仓库根目录执行 `eim --version` 和 `eim list`，确认已安装目标注册表指定的 SDK。当前维护目标使用 v6.1：

```powershell
eim run "idf.py -C firmware/targets/<target> build" v6.1
eim run "idf.py -C firmware/targets/<target> merge-bin -o <full-image>" v6.1
```

也可在 EIM 创建的 v6.1 PowerShell 中直接运行相同的 `idf.py` 命令。无需手工拼接 PATH。

| `<target>` | OTA app 镜像 | `<full-image>` |
| --- | --- | --- |
| `waveshare_rlcd_42` | `ws_rlcd_42.bin` | `ws_rlcd_42_full.bin` |
| `zltech_154tft` | `zltech_154tft.bin` | `zltech_154tft_full.bin` |




## 分区表与 OTA 准备

所有生产目标都使用双 OTA 槽分区表（`ota_0` / `ota_1` / `otadata`），没有 `factory` 分区。首次烧录时 `otadata` 为空，bootloader 按 ESP-IDF 标准行为默认从 `ota_0` 启动。各目标的分区布局见 `targets/<target>/partitions.csv`，OTA 槽容量同时登记在 Target Registry 的 `ota_partition_bytes`，发布装配器会核对两者。

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 已开启。`usage_ota` 使用`esp_ota_begin` / `esp_ota_write` / `esp_ota_end` 写入非当前分区，核对镜像和 SHA-256 后才切换启动分区；新镜像完成显示和 BLE 初始化后才标记有效。

## Folder Push OTA

固件通过现有 Buddy/NUS 通道接收 `ota.folder.v1`，不注册独立 OTA GATT Service。应用 sink 只接受 transfer `firmware`，其中必须先发送 `manifest.json`，物理确认后再发送 `firmware.bin`。详细 wire contract 见 [协议文档](../protocol/README.md)。

固件使用 2,880 B 解码块，完整 JSON 帧上限为 4,096 B。应用容量按目标真实 OTA 槽校验，传输总量另计最多 512 B manifest。设备核对项目、target、镜像结构与摘要，并逐次要求物理确认。

## 烧录和串口日志

首次安装或串口恢复无需 ESP-IDF。直接写入 Release 中与硬件型号匹配的 `-full-` 镜像，从 `0x0` 开始：

```powershell
esptool --chip esp32s3 -p <PORT> write-flash 0x0 waveshare-esp32-s3-touch-amoled-216-full-v<version>.bin
esptool --chip esp32s31 -p <PORT> write-flash 0x0 espressif-esp-mosaico-full-v<version>.bin
```


必须选择正确的设备型号和串口。`-full-` 文件不能用于 BLE OTA；日常升级使用 Release 中对应的 `-ota-` 文件。下面的 `idf.py flash` 命令仅适用于已经从源码构建的开发环境。

先在设备管理器确认串口，使用实际端口 `<PORT>`：

```powershell
```

退出 monitor 使用 `Ctrl+]`。只烧录不监视：

```powershell
```

固件启动后会以 `QF-M5-XXXX` 广播，其中 `XXXX` 来自 BLE MAC 末两字节。

Waveshare 的烧录命令相同，只需替换工程路径和实际端口：

```powershell
```


## 页面和状态

RLCD 4.2 的按键与页面环：

```text
Home -> Deepseek -> Minimax -> Music -> Clock -> Home
```

横屏与竖屏的页面环一致：每个页面都有竖屏版式，按键在两种方向下都走完整的五页。
Settings 由长按 KEY 进入，不在环上。

状态优先级：

1. 配对码浮层；
2. `NO DATA`；
3. `OFFLINE`；
4. `PARTIAL`；
5. `ONLINE`。

蓝牙连接正常时持续显示最后一次有效用量，刷新失败和样本年龄不改变显示。只有蓝牙断开或从未取得有效数据时显示灰态。设备重启后用量从空状态开始，收到有效数据后恢复正常显示。



若全屏画布分配失败，串口会记录 `full-screen canvas allocation failed`，随后降级为直接绘制。降级不会影响页面功能，但合法更新时可能再次看见短暂的整页重绘。


实机验收按 [RLCD 4.2 说明](targets/waveshare_rlcd_42/README.md#hardware-verification-checklist) 的清单执行：面板方向与像素序、按键消抖与长按、SHTC3 读数、电量、提示音、配对重连和 OTA 物理确认。

固件构建成功只能确认画布和刷新调度代码可编译，不能替代上述实机观察。

## BLE 配对

添加设备、输入配对码及主机绑定修复见 [Bridge 配对说明](../bridge/README.md#托盘配对和重连)。加密后配对浮层消失，绑定分别保存在设备 NVS 与主机。

若设备旧绑定仍阻止配对，最后手段是擦除后重新烧录。先确认设备、实际串口和目标工程：

```powershell
eim run "idf.py -C firmware/targets/<target> -p <PORT> erase-flash" v6.1
eim run "idf.py -C firmware/targets/<target> -p <PORT> flash" v6.1
```

`erase-flash` 清除固件及设备全部配置和绑定，仅在确认目标、端口及配置可清除后使用。

## 固件测试工程

CI 按改动路径选择构建：目标目录中的代码或配置变更构建该目标及其板级测试工程；仅修改测试工程时只构建该测试工程。共享固件组件、协议输入、目标注册表或工作流变更会触发全量固件构建。普通 Markdown 文档不触发固件构建。

未受影响的固件项目保留同名轻量检查，以满足分支保护要求，但不会启动 ESP-IDF 容器。Bridge 测试仍会验证固件目录和测试源码注册；Web 检查仅在网站或相关构建输入变化时运行。CI 只编译固件测试镜像，不执行芯片上的 Unity 用例。

共享组件的测试工程在 `test_apps/` 下，每个目标自己的板级逻辑测试在`targets/<target>/test_apps/logic`。本节命令与上面的构建命令一样，在 EIM v6.1 PowerShell 中执行；若 EIM CLI 已在 PATH 中，也可写成 `eim run "<命令>" v6.1`。构建某个目标的板级 Unity 测试镜像：

```powershell
```

连接该目标后可烧录并运行：

```powershell
```

测试覆盖页面循环、倒计时、电量视图、供电判定与按键路由。没有设备时只能验证测试镜像成功编译，不能声明 Unity 用例已在目标芯片上运行。

构建 Buddy JSON 路由、usage.v1 ACK 和 Folder Push OTA sink 契约测试镜像：

```powershell
idf.py -C firmware/test_apps/protocol build
```

连接设备后的运行方式与逻辑测试相同，只需把工程路径替换为`firmware/test_apps/protocol`。该工程还固定检查 M5 的 BLE 广播名和 status JSON、Waveshare 的独立身份、manifest/firmware 顺序、物理确认门控、镜像校验与 abort 清理，并覆盖配置字符串输出缓冲区不足的情况。

Waveshare 页面状态和刷新逻辑测试镜像：

```powershell
```

无 Waveshare 实机时，该命令只能证明测试代码和目标逻辑完成编译、链接，不能声明 Unity 测试已经在芯片上运行。
