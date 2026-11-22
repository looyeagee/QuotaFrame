# 验证范围

本页描述已有硬件验证覆盖，不能替代每个发布候选的验收。构建、软件模拟、串口执行和物理观察分别记录；发布版本应以对应 Release 的测试说明为准。

## 设备覆盖

| 设备 | Windows 已验证 | 仍需专项验证 |
| --- | --- | --- |

以上基础功能和正常 OTA 覆盖来自 2026-09 的开发设备测试，未固定同一套发布候选资产。脚本 OTA 证明设备升级路径，不涵盖安装版 Bridge 的下载、桌面确认和独占连接调度。

macOS 的 M5 配对、加密连接、用量和菜单操作已有验证；当前安装包的完整新用户路径，以及各目标的 macOS OTA 仍需验收。

RLCD 4.2 已验证一块开发板的横屏主页、详情页用量显示和已绑定 Bridge 的加密重连；首次配对、OTA、竖屏/翻转、按键、传感器、音频及 microSD 卡仍待实机验证。详见[目标说明](../firmware/targets/waveshare_rlcd_42/README.md)。

RLCD 4.2 的按键方案已按实机照片复核：板上三个按键自左至右为 KEY、PWR、BOOT，其中 PWR 只切电源电路，固件读不到，因此两个可读按键承担导航（KEY 上一页、BOOT 下一页），双键同按代替中间键（回主页／设置页换行），任一键长按 800 ms 进入设置页。板级逻辑测试镜像已在该板上复跑：加入竖屏详情页与音乐页后的 45 个用例全部通过（其中竖屏环、竖屏详情页、竖屏音乐页各一例）；用例验证的是视图模型与帧缓冲绘制，竖屏画面在玻璃上的观感仍需肉眼确认。按键消抖、双键和弦与长按仍需人工按键确认。详见[目标说明](../firmware/targets/waveshare_rlcd_42/README.md)。

新增的 Claude Code 会话页（`sessions.v1`）已在 AMOLED 1.8 上验证：Bridge 下发会话帧后页面进入环、页面内容与玻璃观感由实机确认。RLCD 4.2 的会话页尚未实机确认（面板能否收到会话帧、玻璃观感、会话页进入环后的翻页手感）；ZLTech 1.54 TFT 不声明该能力，页面环里始终没有这一页。同一批改动里，RLCD 4.2 的设置页去掉了亮度行（反射屏无背光，共四行），行距与页脚的空间也只在逻辑测试镜像里校验过，需要实机确认四行布局与页脚不重叠。

ZLTech 1.54 TFT 已在一台板上整片烧录并启动：v6.1 引导、新分区表（`ota_0` = 7 MB @ 0x20000）、8 MB PSRAM、ADC 校准、ST7789 240 × 240 面板初始化和 `QF-ZLT-154-FE16` 广播均从串口日志确认。Bridge 已与该板建立 Secure Connections 连接（`target=zltech_154tft`），`usage.v1` 数据到达面板并渲染出 DeepSeek 余额、MiniMax 5 小时与周配额；串口日志确认顶部三键（GPIO39 / GPIO0 / GPIO40）都会上报按键事件。屏幕观感由用户实拍照片核对：分带刷新导致的顶部截断与底部重影已消除，时钟改为本地时间，页码点与边框分离。OTA `ota.folder.v1`、电量标定与按键长按仍待专项验证。详见[目标说明](../firmware/targets/zltech_154tft/README.md)。

Waveshare AMOLED 1.8（V1，SH8601 + FT3168）已在一台板上整片擦除并烧录：v6.1 引导、新分区表（`ota_0` = 7 MB @ 0x20000）、8 MB PSRAM、PCF85063A RTC（`RTC=1`）、AXP2101 PMIC（IC 类型校验通过，`PMIC=1`）、FT3168 触摸（`touch=1`）与 SH8601 368 × 448 面板初始化（亮度 80%）均从串口日志确认，BLE 以 `QF-WS-AMO18-B556` 广播（`target=waveshare_amoled_18`）。触摸滑动翻页、轻点/长按手势、Bridge 配对后的 `usage.v1` 渲染、电量计读数与 OTA `ota.folder.v1` 仍待实机专项验证。详见[目标说明](../firmware/targets/waveshare_amoled_18/README.md)。

Waveshare AMOLED 1.8 的会话页随后按彩色屏重做并重刷：该目标的画布改为 RGB565 帧缓冲（PSRAM 中约 322 KB，SH8601 直接发送，不再有 1 → 16 位展开），会话页改为顶部计数 + 每行状态色条、会话名与忙 / 闲芯片，且删除了原分割线以下的最近会话标题。整片烧录后串口日志确认 8 MB PSRAM、SH8601 初始化与 `QF-WS-AMO18-B556` 广播，无帧缓冲分配失败日志；Bridge 重连后会话页进入环时为 `overview → deepseek → minimax → sessions → music → clock`，页面在会话页上稳定显示。该次刷机暴露并修复了第二处推送缺陷：连接建立时只在服务启动那一刻挂上唤醒钩子，托盘随后收养的设备没有钩子，重连后又因为"名单未变"被去重挡掉，导致重启后的面板迟迟收不到会话帧；现在新链路一律重发当前名单。同一版 Bridge 改为事件驱动：`watchdog` 监视 `~/.claude/sessions`，实测写盘到唤醒约 163 ms（含 150 ms 合并窗口），名单不变时不重复下发。彩色会话页的四行满配画面、空名单画面与其余页面的配色仍以主机渲染核对，玻璃上的观感待肉眼确认；RLCD 4.2 与 ZLTech 1.54 TFT 仍走 1 位画布，行为不变。

Waveshare AMOLED 1.8 的会话页与按目标设置行数改动（`v1.2.1-42-gd1d2b14`）在该板上分两次烧录。第一次烧入的是尚未实现会话页的版本，用来核对按目标门控：该板声明 `brightness.set.v1`、不声明 `sessions.v1`，页面环逐页走过 `miniMax → music → clock → overview`，没有出现会话页。该次刷机暴露并修复了控制页翻页对全部设备返回 500 的缺陷：`_dispatch` 把 `direction < 0` 的布尔值传给只接受 `-1/1` 的会话方法。第二次烧入实现会话页的版本后，能力声明变为包含 `sessions.v1`，页面环为 `overview → deepseek → minimax → sessions → music → clock`，Bridge 下发一条运行中（空闲）会话与两条最近标题后页面正常渲染；摘要行、忙/闲芯片与会话名的对齐、分隔线与最近标题的排布由实机确认。两次烧录的 v6.1 引导、8 MB PSRAM、`RTC=1 PMIC=1 touch=1`、SH8601 368 × 448 初始化（亮度 80%）与 `QF-WS-AMO18-B556` 广播均从串口日志确认，Bridge 均重新建立加密连接（`encrypted=1 bonded=1`）。控制页翻页与亮度在实机上验通（亮度 50 → 40 → 50 均被设备接受并回报）。RLCD 4.2 的会话页与四行设置页布局仍只在逻辑测试镜像里校验。

公共包重构（`usage_panel` / `usage_panel_app`）后的固件同样在两块板上整片烧录验证：v6.1 引导、新分区表、8 MB PSRAM、ADC 校准、ST7789 初始化和 `QF-ZLT-154-FE16` 广播从串口日志确认，Bridge 重新建立 Secure Connections 连接（`encrypted=1 bonded=1`）。该次刷入暴露了主任务栈溢出——画布与视图模型被放在 `app_main` 的局部变量里，超出 12 KB 栈后固件在第一条日志上触发中断看门狗并反复重启；改为静态存储后同板复测通过。RLCD 4.2 的重构固件在同一版修复上整片烧录验证：v6.1 引导、RTC 与 ADC 校准、ST7305 面板 TE 时序（58.7 ms）和 `QF-WS-S3-R42-C092` 广播从串口日志确认，Bridge 重新建立 Secure Connections 连接（`encrypted=1 bonded=1`），观察窗口内无看门狗复位。


## 性能与显示参考

- [AMOLED 帧缓冲与 BLE 基准](validation/amoled-performance.md)：记录历史实验条件与测量边界。
- [ePaper 局刷数据](validation/epaper-refresh.md)：区分执行日志、帧缓冲和实际光学观察。

历史基准用于比较测试方法与约束，不代表当前固件的速度、内存余量或续航。

## 发布验收

同一候选版本需记录源码版本、平台、硬件、配置、Bridge/固件/manifest 的 SHA-256、操作和结果。重新构建的同版本文件应重新核对，不能沿用其他字节的验收结论。

仍需覆盖各维护目标（以[目标注册表](../bridge/src/quotaframe_bridge/targets.py)为准）的传输中断、损坏摘要、供电中断、失败后重试及启动失败回滚；正常升级成功不等于故障路径通过。完整清单见[软件与发布验收](validation/software-acceptance.md)。
