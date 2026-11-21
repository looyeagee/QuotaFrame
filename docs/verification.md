# 验证范围

本页描述已有硬件验证覆盖，不能替代每个发布候选的验收。构建、软件模拟、串口执行和物理观察分别记录；发布版本应以对应 Release 的测试说明为准。

## 设备覆盖

| 设备 | Windows 已验证 | 仍需专项验证 |
| --- | --- | --- |

以上基础功能和正常 OTA 覆盖来自 2026-09 的开发设备测试，未固定同一套发布候选资产。脚本 OTA 证明设备升级路径，不涵盖安装版 Bridge 的下载、桌面确认和独占连接调度。

macOS 的 M5 配对、加密连接、用量和菜单操作已有验证；当前安装包的完整新用户路径，以及各目标的 macOS OTA 仍需验收。

RLCD 4.2 已验证一块开发板的横屏主页、详情页用量显示和已绑定 Bridge 的加密重连；首次配对、OTA、竖屏/翻转、按键、传感器、音频及 microSD 卡仍待实机验证。详见[目标说明](../firmware/targets/waveshare_rlcd_42/README.md)。

RLCD 4.2 的按键方案已按实机照片复核：板上三个按键自左至右为 KEY、PWR、BOOT，其中 PWR 只切电源电路，固件读不到，因此两个可读按键承担导航（KEY 上一页、BOOT 下一页），双键同按代替中间键（回主页／设置页换行），任一键长按 800 ms 进入设置页。板级逻辑测试镜像已在该板上运行，42 个用例全部通过；按键消抖、双键和弦与长按仍需人工按键确认。详见[目标说明](../firmware/targets/waveshare_rlcd_42/README.md)。

ZLTech 1.54 TFT 已在一台板上整片烧录并启动：v6.1 引导、新分区表（`ota_0` = 7 MB @ 0x20000）、8 MB PSRAM、ADC 校准、ST7789 240 × 240 面板初始化和 `QF-ZLT-154-FE16` 广播均从串口日志确认。Bridge 已与该板建立 Secure Connections 连接（`target=zltech_154tft`），`usage.v1` 数据到达面板并渲染出 DeepSeek 余额、MiniMax 5 小时与周配额；串口日志确认顶部三键（GPIO39 / GPIO0 / GPIO40）都会上报按键事件。屏幕观感由用户实拍照片核对：分带刷新导致的顶部截断与底部重影已消除，时钟改为本地时间，页码点与边框分离。OTA `ota.folder.v1`、电量标定与按键长按仍待专项验证。详见[目标说明](../firmware/targets/zltech_154tft/README.md)。


## 性能与显示参考

- [AMOLED 帧缓冲与 BLE 基准](validation/amoled-performance.md)：记录历史实验条件与测量边界。
- [ePaper 局刷数据](validation/epaper-refresh.md)：区分执行日志、帧缓冲和实际光学观察。

历史基准用于比较测试方法与约束，不代表当前固件的速度、内存余量或续航。

## 发布验收

同一候选版本需记录源码版本、平台、硬件、配置、Bridge/固件/manifest 的 SHA-256、操作和结果。重新构建的同版本文件应重新核对，不能沿用其他字节的验收结论。

仍需覆盖各维护目标（以[目标注册表](../bridge/src/quotaframe_bridge/targets.py)为准）的传输中断、损坏摘要、供电中断、失败后重试及启动失败回滚；正常升级成功不等于故障路径通过。完整清单见[软件与发布验收](validation/software-acceptance.md)。
