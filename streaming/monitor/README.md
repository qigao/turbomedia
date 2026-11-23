# 视频监控台

`turbo_monitor_console` 是 Windows 和 Linux 的本机值班应用。首个版本接入
1 至 8 路 H.264 RTSP 视频，提供定时刷新的浏览器预览、断流告警、确认和故障前录像回放。
每路独立连接和重连。画面暂时失联时，保留最后一张预览并显示断流文字和最后收帧时间。
源恢复后，界面更新同一条告警并保留确认状态。

## 启动

构建目标为 `turbo_monitor_console`。下面使用源码目录中的网页资源和四路 RTSP 地址，
`--archive` 指向已有的本机目录。

```powershell
& <build>/bin/turbo_monitor_console.exe --port 8088 `
  --web streaming/monitor/web --archive C:/recordings `
  --source 入口 rtsp://192.168.10.21/live/main `
  --source 仓库 rtsp://192.168.10.22/live/main `
  --source 通道 rtsp://192.168.10.23/live/main `
  --source 月台 rtsp://192.168.10.24/live/main
```

浏览器打开 `http://127.0.0.1:8088/`。`--port` 省略时为 8088。应用监听
`127.0.0.1`。按 Ctrl+C 会先停止 HTTP 服务，再停止输入并等待已受理的录像任务。
安装组件名为 `MonitorConsole`。安装后网页资源位于
`share/TurboMedia/monitor`，将该目录传给 `--web`。

## 预览和录像

- RTSP 使用 TCP。每路最多每 400 ms 更新一次 JPEG 预览。页面提供暂停和恢复预览，
  暂停期间仍更新接入状态及告警。
- 连续 3 秒收不到有效解码帧或拉流返回终止错误时打开断流告警，重连失败期间沿用这条
  告警。首帧成功解码后记录恢复时间。不支持的编码会显示格式错误。
- 在活跃连接中以关键帧为起点暂存约 8 秒压缩视频。每个缓冲最多 600 包和
  8 MiB 压缩负载。当连接断开时，独立任务按源封装已有数据为 MP4。播放按钮只在
  封装发布完成后显示。等待首个关键帧、开机即失联或写盘失败时，界面显示没有可回放的片段。
- 每次启动建立独立目录，最多受理 64 个录像。媒体文件在运行期间保持不可变，
  便于浏览器按字节范围读取。每路最近一条告警和确认状态保存在当次进程内。
  已封装的 MP4 文件留在归档目录，可用播放器打开。

## 验证

运行以下 CTest。测试使用本仓库的 RTSP 服务端和四个本地发布者，暂停其中一个，
验证其它三个仍更新预览，确认断流告警，下载并解码故障前录像，再验证同一告警的恢复。
本机需有 Python 3.8 或更高版本以运行端到端测试。

```powershell
ctest --test-dir <build> -R '^turbo_monitor_console_e2e$' --output-on-failure
```

[综合监控台设计](../../docs/design/monitoring-console-zh.md)
