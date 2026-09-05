# Upstream Attribution

	ransport/rtp 与 	ransport/sip 目录的代码移植自
[ireader/media-server](https://github.com/ireader/media-server)（ZLG，MIT License）。

本地修改：

- RTP/SIP 内部缓冲区与时间接口已适配 Salts/TurboNet（如 	urbo_gettimeofday、
  	urbo_secure_random）；
- 修复了 RTP 库内失败即 bort() 的行为（tcp-interval.c、tp-time.c、
  sip-string-view.h），改为可恢复的错误处理；
- RTP 与仓库内 RTC RTP ABI 采用不同 packet 布局，构建时通过
  -Bsymbolic-functions 绑定内部符号，避免符号抢占串包。

注意：上游具体 commit 尚未在仓库内固定；MIT 版权声明随上游源码保留，发布前需补齐
完整 LICENSE 文本。