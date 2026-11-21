# media-control（vendored）

上游：<https://github.com/ungive/media-control> 0.7.6（BSD-3-Clause，见 `LICENSE`）。
`lib/media-control/mediaremote-adapter.pl` 与 `Frameworks/MediaRemoteAdapter.framework`
来自同作者的 <https://github.com/ungive/mediaremote-adapter>。

## 用途

macOS 的系统 Now Playing 服务（`mediaremoted`）镜像了播放器通过
`MPNowPlayingInfoCenter` 发布的曲目、位置锚点、播放速率与播放状态。读了这份镜像，
暂停、快进、换歌都能在事件发生时看到，不必等播放器自己的音频输出状态变化，也不必轮询
它自己的数据库。

## 为什么必须经 `/usr/bin/perl` 跑

`mediaremoted` 只服务带 Apple 平台 entitlement 的客户端：ad-hoc 签名进程得到
`entitlements=0`，播放队列请求被拒（`kMRMediaRemoteFrameworkErrorDomain` 3，
"Operation not permitted"），`MRMediaRemoteGetNowPlayingInfo` 的回调收到 null。
`MediaRemoteAdapter.framework` 自身是 ad-hoc 签名、也没有 entitlement，所以它必须被
加载进一个 Apple 签名的宿主进程——系统自带的 `/usr/bin/perl` 就是这样一个宿主。
Bridge 自己的解释器（Homebrew Python、PyInstaller 冻结产物）都属被拒的一类，因此
`macos_nowplaying` 只启动子进程，不在本进程内加载这个 framework。

## 本地改动

无。三个文件按上游原样拷贝，未修改。
