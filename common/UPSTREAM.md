# Upstream Attribution

common/mov, common/mkv, common/flv 与 common/mpeg 目录的代码移植自
[ireader/media-server](https://github.com/ireader/media-server)（ZLG，MIT License）。

本地修改：

- 构建改为 CMake target，不再使用原项目 autotools；
- 与原项目 IO/内存接口耦合的部分已替换为 Salts 的 buffer/string API；
- 移除了本仓库不使用的历史 RTSP 相关文件（见 git log 的 legacy rtsp 提交）。

注意：上游具体 commit 尚未在仓库内固定，接入新版本前应核对差异；MIT 版权声明随
上游源码保留，本目录未附带完整 LICENSE 文本，发布前需补齐。
