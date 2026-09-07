# Upstream Attribution

streamer/hls、streamer/dash 与 streamer/rtmp 目录的代码移植自
[ireader/media-server](https://github.com/ireader/media-server)（ZLG，MIT License）。

本地修改：

- HLS/DASH 分片写入与 m3u8/mpd 生成保留上游实现，接入 Salts 容器与 IO 接口；
- RTMP 传输层基于 CNet（TCP/TLS）重写，不再依赖上游自带的 socket 封装；
- 移除了本仓库不使用的历史 RTSP 相关代码。

注意：上游具体 commit 尚未在仓库内固定；MIT 版权声明随上游源码保留，发布前需补齐
完整 LICENSE 文本。
