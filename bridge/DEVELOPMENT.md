# Bridge 开发指南

安装、日常操作和故障恢复见[用户指南](README.md)。会话归属、独占调度和持久化规则以[接入架构](../docs/design/open-device-access.md)为准。

## 从源码运行

Windows 在仓库根目录执行：

```powershell
cd bridge
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -e .
```

macOS：

```bash
cd bridge
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -e .
source .venv/bin/activate
```

安装后可用 `quotaframe-bridge` 或 `python -m quotaframe_bridge` 运行控制台形态。macOS 源码菜单栏入口需要显式指定 Release 仓库（源码树不嵌入生产 channel JSON）：

```bash
export QUOTAFRAME_RELEASE_REPO=eMUQI/QuotaFrame
python -m quotaframe_bridge.ui.macos.app
```

这个环境变量只用于源码开发或恢复测试；冻结 `.app` 会在构建时嵌入同一仓库名。Bleak 只在真实 BLE 模式下加载，因此源码测试和 `--mock --dry-run` 不依赖 BLE 设备。

### macOS 源码运行的蓝牙权限

源码控制台和菜单栏入口应从 Terminal.app 或 iTerm 启动，首次使用蓝牙时允许对应终端访问；若曾拒绝，在「系统设置 > 隐私与安全性 > 蓝牙」中开启该终端的权限。缺少蓝牙用途说明或权限归属的启动环境可能导致 CoreBluetooth 终止进程。Bridge 会对明确拒绝的授权状态给出可读错误。

此要求仅适用于源码运行。DMG 安装的应用从 Applications 启动，并授予 QuotaFrame Bridge 蓝牙权限。

`--mock --dry-run` 不访问蓝牙，不需要蓝牙权限。

### macOS 源码运行的登录自启

[`scripts/quotaframe_launch_agent.sh`](scripts/quotaframe_launch_agent.sh) 把控制台形态注册为登录项，重启后自动接管面板：

```bash
bridge/scripts/quotaframe_launch_agent.sh install    # 生成 plist 并立即启动
bridge/scripts/quotaframe_launch_agent.sh status     # agent 状态、进程、日志大小
bridge/scripts/quotaframe_launch_agent.sh logs       # tail -f
bridge/scripts/quotaframe_launch_agent.sh restart
bridge/scripts/quotaframe_launch_agent.sh stop       # bootout，不是发信号
bridge/scripts/quotaframe_launch_agent.sh uninstall
```

生成的是 `~/Library/LaunchAgents/com.quotaframe.bridge.plist`，脚本自身的绝对路径写入其中，仓库里不保存机器相关路径。agent 以同一脚本的 `run` 子命令启动：先修剪日志再 `exec` 解释器，因此 launchd 跟踪的就是 Bridge 进程本身，信号和退出码都不经过中间层。

日志在 `~/Library/Logs/quotaframe/bridge-console.log`，与菜单栏形态的 `bridge.log` 同目录但不同文件名——后者由 `RotatingFileHandler` 轮转，若两者共用一个文件，轮转会把 launchd 仍持有的描述符改走。默认 `INFO`；需要协议级细节时 `QUOTAFRAME_LOG_LEVEL=DEBUG bridge/scripts/quotaframe_launch_agent.sh install` 重装即可。launchd 长期持有该描述符，所以每次启动在超过 8 MiB 时把它改名为 `bridge-console.log.1`，不做运行中轮转。

`KeepAlive` 只在非零退出后重启（`ThrottleInterval` 10 s），因此 `kill` 会自愈，而 `stop` 必须走 `bootout`。已加载 agent 时手工运行 `python -m quotaframe_bridge` 会因实例锁直接退出并提示另一个实例在运行，先 `stop` 再手工运行。进程被信号终止时无法执行清理，启动步骤会回收上一轮遗留的 MediaRemote 适配器子进程。

launchd 实例与终端实例使用同一个解释器，已授予该解释器的蓝牙权限继续有效。

## Mock 测试

无账户、无设备、无 BLE：

```powershell
python -m quotaframe_bridge --mock --dry-run
```

默认运行一个采集周期并输出两条 JSONL。多周期：

```powershell
python -m quotaframe_bridge --mock --dry-run --cycles 3 --interval 0
```

有任一受支持面板、没有真实账户时，先添加设备（只需一次），再以 Mock 数据运行：

```powershell
python -m quotaframe_bridge --add-device
python -m quotaframe_bridge --mock
```

没有已添加的设备时，不带 `--device-name` / `--name-prefix` 的运行会直接报错并提示使用 `--add-device`。`--list-devices` 列出已添加的设备，`--forget-device <地址>` 删除一条记录。

## CLI 与调度

以下命令使用源码控制台入口；完整参数见 `python -m quotaframe_bridge --help`。

同一系列有多个设备，或只想连接一台设备时，使用完整名称：

```powershell
quotaframe-bridge --device-name "<DEVICE_NAME>"
```

将 `<DEVICE_NAME>` 替换为实际设备的完整蓝牙广播名。

也可只扫描 Waveshare 设备：

```powershell
quotaframe-bridge --name-prefix QF-WS-S3-A216-
```

`--device-name` 和 `--name-prefix` 都会切换到单设备模式；默认按已添加的设备列表连接，不需要设置。

常用参数：

```text
--interval 60             CodexBar 采集周期（秒）
--publish-interval 30     向设备重发最新状态的周期（秒）
--scan-timeout 10
--connect-timeout 60
--source-timeout          每条 CLI 命令的超时（秒），Windows 默认 15，macOS 默认 90
--ack-timeout 5
--http-host 127.0.0.1     本地控制页面监听地址
--http-port 8788          本地控制页面端口，0 关闭
--log-level INFO
```

完成加密连接并通过协议状态校验后，Bridge 会输出：

```text
BLE connected: device=Waveshare AMOLED 2.16 secure=true protocol=1 capabilities=usage.v1
```

自动重连通过相同校验后也会再次输出；未完成或校验失败的连接不会输出成功记录。

`--interval` 从上一次采集完成后计时，`--publish-interval` 独立重发最近快照。采集中收到的手工刷新请求将在本轮完成后执行。新设备连接就绪后立即接收已有快照，无快照时等待首次结果。

## 本地控制页面

控制台形态默认在 `--http-host`:`--http-port`（默认 `127.0.0.1:8788`，端口 0 关闭）提供页面和 JSON 接口，实现在 `src/quotaframe_bridge/control/`：

```text
GET  /                   页面（令牌随查询串带入）
GET  /sessions           Claude Code 会话页面（同一令牌）
GET  /api/state          设备、额度、播放状态快照
GET  /api/cc/sessions    本机 Claude Code 会话快照
POST /api/screen/toggle  切换到时钟 / 额度
POST /api/screen/page    {"direction": "next" | "previous"}
POST /api/alert          {"mode": 0 | 1 | 2}，0 关闭、1 仅告警页、2 告警页加提示音
POST /api/refresh        立即采集
```

`backend.ServiceControl` 把服务适配成这组窄接口，命令调用的是 `toggle_screensavers()` / `turn_pages()` / `set_alert()` / `request_collection()`，也就是托盘用的同一入口。设备是否会接受命令由 `DeviceSession.screen_command_unavailable()` 与 `alert_command_unavailable()` 判定；它们和设备发送前的自检是同一个判断，因此返回 202 时会附每台设备的 `accepted` 与 `reason`，不会把被丢弃的命令报成成功。告警档位由设备写入自己的 NVS；`/api/state` 回报 `DeviceSession.alert_mode`，即本次连接内最近一次 ACK 的写入值，重连后回落到新读到的 `status.alert`，面板因此始终是权威来源。设备本地设置页改档位后，页面要等到下一次重连或再次应用才反映。

`server.py` 是标准库 `asyncio` 实现的 HTTP/1.1，不引入第三方框架：请求头上限 8 KiB、请求体上限 4 KiB、空闲连接 30 秒回收。`routes.py` 与套接字层分离，可脱离网络直接测试。令牌来自 `control.token`（`paths.data_directory()`，权限 0600），由 `token.py` 首次生成；绑定环回地址时额外校验 `Host` 头以防 DNS rebinding，绑定其他地址时只依赖令牌。

页面是无构建步骤的单个字符串（`page.py` 与会话页 `sessions_page.py`），文案取自 `catalog.py`，冻结包与源码树服务同一份内容。会话页由 `cc_sessions.py` 只读本机 `~/.claude`：`sessions/<pid>.json` 提供运行中会话与忙碌状态，`projects/*/*.jsonl` 只读文件头提取标题，两者按会话 ID 关联。`bridge/tests/test_control_http.py` 覆盖路由、令牌、命令派发与真实套接字往返，不需要蓝牙；会话读取器与页面在 `bridge/tests/test_cc_sessions.py`。

## 数据源接口

Bridge 调用所在平台的 CodexBar 结构化接口。两个上游的 JSON 契约不兼容，因此有两个 source adapter：

```text
Windows：codexbar-cli config providers --json
         codexbar-cli usage -p codex --json
         codexbar-cli usage -p claude --json
macOS：  codexbar config providers --json
         codexbar usage --provider codex --format json
         codexbar usage --provider claude --format json
```

Bridge 每轮读取启用列表，只查询已启用的来源。两路并发、各自计时，先收到的用量立即进入推送。查询失败时保留该来源上次有效的用量和原采样时间。Win-CodexBar 0.46.0 和 Bridge 固定下载的 1.2.12 通过 `config dump --format json` 提供 `enabled_providers`。macOS 需要支持 `config providers --json` 的 CLI。

随后只保留两个 provider 的 primary/secondary 窗口百分比、重置时间和采样时间，转换成 `usage.v1` 后通过加密 BLE 发送。原始 stdout/stderr、身份、费用、计划和凭据不会写入 BLE 消息或正常日志。

## Claude Code 会话

Bridge 另有一路只读数据源，读本机 `~/.claude` 下 Claude Code 自己的会话数据：

```text
~/.claude/sessions/<pid>.json      运行中会话：名称、忙碌状态、进程是否存活
~/.claude/projects/*/<id>.jsonl    会话记录，只读文件头提取标题
```

`sources/claude_sessions.py` 把快照收成面板要显示的两样东西：运行中会话的（显示名，忙碌与否）和最近会话的标题。会话 ID、工作目录、git 分支、时间戳和对话内容都不进入线上帧。转换后的 `sessions.v1` 帧由 `DeviceSession.publish_sessions()` 每 10 秒推给声明了该能力的设备；没有该能力的设备不受影响。

`control/cc_sessions.py` 同时支撑本地控制页的 `/sessions` 页面与 `/api/cc/sessions` 接口，两者读的是同一份数据。

## 更新与发布配置

tag 去掉前导 `v` 后必须与打包版本一致。固件目录独立于 Bridge 自身更新，默认读取配置发布仓库的 `releases/latest/download/manifest.json`；启动及每 24 小时检查一次，按 `firmware_project` 与 `target` 选择候选。只有点击更新才下载镜像，核对大小、SHA-256、应用头、芯片与分区容量。开发入口 `QUOTAFRAME_FIRMWARE_MANIFEST_URL` 仍须满足受控 HTTPS 来源规则，确认框显示实际镜像来源。当前目录 schema 为 `schema_version: 1` 加 `releases` 数组，需要随发布产物一起部署。

Windows 更新下载项按当前运行的 EXE 路径与当前用户安装登记是否一致，区分安装版和便携版；源码运行、安装登记不可读或资产缺失时回退到 Release 页面。macOS 打开 Release 页面。

## 测试

不安装项目也可从仓库根目录执行：

```powershell
$env:PYTHONPATH = "bridge/src"
python -m unittest discover -s bridge/tests -v
```

macOS：

```bash
PYTHONPATH=bridge/src python3 -m unittest discover -s bridge/tests -v
```

必须从仓库根目录运行：部分测试用 `bridge.tests.test_protocol` 的绝对导入共享 fixture。

当前测试包括两个平台的 CodexBar JSON 投影、隐私字段白名单、协议、分包、ACK、状态协商、重连、数据保留、平台目录布局、平台分发和 Mock CLI。

平台专属的测试会自动跳过而不是失败：托盘、注册表自启、Tk 弹窗和 Windows 控制台取码在非 Windows 上跳过，发布脚本测试在没有 PowerShell 时跳过。反过来，配置目录布局、CodexBar 两种方言和平台分发这几组在任一平台都会把两边都跑一遍，因此在 Windows 上开发也覆盖得到 macOS 分支。

## 构建独立可执行文件

以下构建命令均从仓库根目录运行；若当前位于 `bridge/`，先执行 `cd ..`。

不需要用户安装 Python 或创建虚拟环境时，可以打包成单文件 `.exe`：

```powershell
.\bridge\build_exe.ps1 -ReleaseRepository owner/releases
```

脚本会在 `bridge/.build-venv`（不提交到仓库）里安装 `bleak`、`pystray`、`pillow` 和 `pyinstaller`，产物是 `bridge/dist/quotaframe-bridge.exe`。运行形态和控制台限制见[用户指南](README.md#托盘配对和重连)。

安装器构建复用上述 EXE：

```powershell
.\bridge\build_installer.ps1 -Python python
```

脚本在 `bridge/build/tools` 下载并校验固定版本 Inno Setup，以便携模式准备编译器，然后生成 `bridge/dist/quotaframe-bridge-windows-v<version>-setup.exe`。也可通过 `-Iscc <path>` 使用本机编译器。安装器不会默认开启自启动，升级保留已有选择；卸载仅清理指向本次安装的自启项，保留用户配置、设备记录与依赖缓存。`bridge/test_installer.ps1` 可在独立 AppId、测试自启值和项目构建目录下验证安装、覆盖升级与卸载，结束后清理测试注册项。

冻结包包含 Python、托盘组件、Windows 蓝牙后端与 CLI 所需的 VC 运行库；用户无需单独安装 Python。

Apple Silicon Mac 先构建 `.app`：

```bash
./bridge/build_app.sh --release-repository owner/releases
```

脚本要求 arm64 主机，生成 `bridge/dist/QuotaFrame Bridge.app`，创建 `.icns`，并执行 ad-hoc 签名与结构校验。再生成 DMG：

```bash
./bridge/build_dmg.sh \
  --app "bridge/dist/QuotaFrame Bridge.app" \
  --output bridge/dist/quotaframe-bridge-macos-arm64-v<version>.dmg
```

DMG 包含应用 bundle 与 Applications 快捷方式。发布产物由 `v*` tag workflow 构建；Release 资产契约与手工装配见[产品发布说明](../docs/release/FIRMWARE_RELEASE.md)。
