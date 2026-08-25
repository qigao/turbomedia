# FFmpeg 图式媒体流水线

## 决策背景

TurboMedia 已有 codec、demuxer、muxer、streamer 和 transport 接口，但它们的公开数据契约主要面向压缩包和既有协议适配，缺少完整的像素/采样格式、time base、drain 与 filter 协商语义。直接扩展这些接口会改变现有公开行为，并把 FFmpeg 类型扩散到多个模块。

本模块新增独立的 `TurboMedia::Pipeline`。YAML 图是配置事实源；验证器先把图编译为有界执行计划。文件/网络 URL 图由 FFmpeg 适配器执行；RTC 图通过显式注入的 `ServerRuntime` 在媒体源之间转发或转码 RTP。

## 候选方案

- 调用 `ffmpeg` 命令行进程：隔离较好，但部署必须额外携带可执行文件，错误与停止语义也难以映射到库接口。
- 复用现有 codec/demuxer/muxer/transport：短期代码少，但现有数据契约不足以无损表达一般转码，修改面和兼容风险更大。
- 直接使用 libavformat/libavcodec/libavfilter：依赖已经存在，可保留 AVPacket/AVFrame 的原生时序和所有权语义。v1 采用此方案。

## v1 图契约

配置必须声明 `api_version: turbo.media.pipeline/v1`。节点类型和工厂为：

| kind | factory | 配置归属 |
| --- | --- | --- |
| source | `ffmpeg.input` | 输入 URL 与 FFmpeg input options |
| demux | `ffmpeg.demux` | 可选输入 format |
| decoder | `ffmpeg.decode` | `media: audio\|video` |
| filter | `ffmpeg.filter` | 单输入单输出 filter chain |
| encoder | `ffmpeg.encode` | media、codec 及编码参数 |
| mux | `ffmpeg.mux` | 可选输出 format |
| sink | `ffmpeg.output` | 输出 URL 与 FFmpeg output options |

RTC/RTP 执行路径还提供四个配套工厂：

| kind | factory | 配置归属 |
| --- | --- | --- |
| source | `media.runtime_source` | `vhost/app/stream` 与有界队列参数 |
| demux | `rtp.depacketize` | 从完整 RTP packet 还原 payload/access unit |
| mux | `rtp.packetize` | 重新生成 RTP packet |
| sink | `media.runtime_sink` | 输出 `vhost/app/stream` |

四个骨架工厂必须来自同一执行路径，不能把 FFmpeg URL 工厂和 Runtime/RTP
工厂交叉拼接。Runtime/RTP v1 支持一个 H.264 视频 track 和一个 Opus 音频
track。每条分支既可直接 `depacketize -> packetize`，也可显式插入
`ffmpeg.decode -> [ffmpeg.filter] -> ffmpeg.encode`。H.264 以 RTP marker
聚合 Annex B access unit；Opus 每个 RTP payload 作为一个压缩 packet。
转码输出目前只接受 H.264 视频 encoder 和 Opus 音频 encoder；其他 codec 在
`prepare` 阶段 fail fast，不会静默退化成原包转发。输出保留输入 payload type
与 SSRC，H.264/Opus 时钟分别规范化为 90000/48000。

端点写成 `node_id.pad`。固定输入骨架为 `source.out -> demux.in`。FFmpeg
执行路径允许 1 到 8 组独立的 `mux.out -> sink.in`，Runtime/RTP 执行路径仍只
允许一组。每个媒体分支只能是以下两种之一：

```text
demux.audio/video -> 每个 mux.audio/video
demux.audio/video -> decoder -> [filter] -> encoder -> 每个 mux.audio/video
```

v1 最多处理一个最佳音频流和一个最佳视频流。已配置分支必须连接所有 mux，
不支持按输出选择不同轨道、编码参数、运行期改图或复杂 filter graph 标签。
filter 字符串拒绝 `;`、`[`、`]`，从而限制为单输入单输出链。

多输出是同步且有界的共同失败域：runner 按配置顺序把同一复制/编码 packet 写入
每个 sink，慢 sink 在 `io_timeout_ms` 内对整条流水线施加背压；任一输出失败会
使 pipeline 失败，不会跳过该输出、静默丢包或自动退化。外部输出无法事务回滚，
所以失败时各输出可能只拥有同一媒体流的不同长度前缀。`packets_written` 和
`bytes_written` 按成功的 sink delivery 累加；相同内容完整写入两个输出时，其值
通常是单输出的两倍。

完整配置见 [`examples/ffmpeg_pipeline.yml`](../examples/ffmpeg_pipeline.yml)。最小调用顺序为：

```c
turbo_pipeline_error_t error;
turbo_pipeline_t *pipeline =
    turbo_pipeline_create_from_yaml(yaml_text, yaml_size, &error);
if (!pipeline) return error.code;

turbo_pipeline_status_t status = turbo_pipeline_prepare(pipeline, &error);
if (status == TURBO_PIPELINE_OK)
    status = turbo_pipeline_run(pipeline, &error);

turbo_pipeline_destroy(pipeline);
return status;
```

直播推送与本地录制同时输出的受限 fan-out 示例见
[`examples/ffmpeg_multi_sink.yml`](../examples/ffmpeg_multi_sink.yml)。示例只共享已编码
视频分支；如需 HLS 或文件输入，可替换 source URL，图的 mux/sink 扇出契约不变。

Runtime/RTP 图在 `prepare` 前还必须绑定借用的 ServerRuntime：

```c
turbo_pipeline_status_t status =
    turbo_pipeline_bind_server_runtime(pipeline, runtime, &error);
if (status == TURBO_PIPELINE_OK)
    status = turbo_pipeline_prepare(pipeline, &error);
if (status == TURBO_PIPELINE_OK)
    status = turbo_pipeline_run(pipeline, &error);
```

RTC 直通和转码示例分别见
[`examples/runtime_rtp_relay.yml`](../examples/runtime_rtp_relay.yml) 与
[`examples/runtime_rtp_transcode.yml`](../examples/runtime_rtp_transcode.yml)。其中
`queue_capacity`、`max_packet_bytes` 和 `max_access_unit_bytes` 都是硬上限；
队列满或输入 RTP 超限时，发布者收到 `TURBO_MEDIA_ERR_FULL`，runner 同时以
`TURBO_PIPELINE_EBACKPRESSURE` 失败。Runtime 的 source key 及 track metadata
是 codec、payload type、clock rate 的唯一事实源。sink key 在 `prepare` 前必须
不存在；prepare 完成后该输出 source 提交给 ServerRuntime 持有，pipeline
销毁不会隐式删除它，调用方应在所有 player 关闭后显式移除。

构建后的同步 runner 可直接执行同一配置：

```text
turbo_pipeline_run path/to/graph.yml
turbo_pipeline_run --stop-after 10s path/to/live-graph.yml
```

runner 会在正常 EOF 后输出 packet、frame 与 byte 统计；配置、打开或运行失败时，
错误包含失败阶段、节点和 pipeline 状态码。实时流需要由嵌入方在其他线程调用
`turbo_pipeline_request_stop()` 实现协作停止。runner 的 `--stop-after` 接受正整数
`ms`、`s` 或 `m` 单位，计时从 `prepare` 完成后开始；正常 EOF 会立即取消并
join timer，不会等待剩余时长。

有限时长的公开网络 smoke 配置见
[`examples/public_https_to_mpegts.yml`](../examples/public_https_to_mpegts.yml)。
它使用 W3C 的 Sintel HTML video 测试资产验证 HTTPS MP4 输入到 MPEG-TS
文件输出；输出路径相对于 runner 的当前工作目录。

长时网络 smoke 配置还包括
[`examples/public_hls_to_matroska.yml`](../examples/public_hls_to_matroska.yml)。
它锁定 Apple 示例的单一 video media playlist，避免 master playlist 的多
rendition 枚举，并适合手动执行带 `--stop-after` 的有界验证，不进入默认 CTest。
FFmpeg 依赖已启用 vcpkg 的 `xml2`、`opus` 与 `openh264` feature，使 `dash`
demuxer、实时 `libopus` 和 `libopenh264` 编码器可用。公开 DASH 转封装配置见
[`examples/public_dash_to_matroska.yml`](../examples/public_dash_to_matroska.yml)。

## 状态、所有权与失败

- pipeline 独占不可变配置、FFmpeg context、packet 与 frame。
- Runtime/RTP pipeline 借用 ServerRuntime；Runtime、输入 source 和输出 source
  必须活得比 pipeline 久，且 prepared/running 期间不得删除或替换这些 source。
- 当前 ServerRuntime 的订阅表由所属事件循环串行维护；关闭顺序必须是先停止并
  quiesce WHIP/RTSP 等输入 publisher，再 `request_stop`、join runner，最后关闭
  WHEP player 和销毁 pipeline。输入仍在 publish 时并发 unsubscribe 不在 v1
  保证范围内。
- Runtime 输入回调只把完整 RTP packet 复制进预分配的有界 ring；执行线程是
  唯一 depay、FFmpeg 与 pay 消费者。`AVPacket`/`AVFrame` 只归该消费者线程
  所有；输出 packet 在同步 publish 返回前有效，不跨回调借用。
- `create` 只解析和验证，不产生外部副作用。
- `prepare` 按 input → output contexts → shared decoder/encoder/filter → output streams/IO 顺序建立资源；任何失败都释放已建立资源并进入 `FAILED`。
- FFmpeg `run` 是单线程 pull；Runtime/RTP `run` 是单消费者 pull。两条路径都
  不存在无界队列或跨线程裸指针。
- `request_stop` 是并发安全的原子请求；FFmpeg interrupt callback 同时检查停止标志和 I/O deadline。
- 正常 EOF 会 drain decoder、filter 和 encoder，再写 trailer。错误不伪装成成功，也不自动降级为 stream copy。
- Runtime/RTP 没有输入 EOF；显式 stop 会丢弃 ring 和编解码器中的残留帧，不把
  停止后的延迟帧发布到已关闭的 RTC session。
- stream-copy 显式启用 FFmpeg `AVFMT_FLAG_AUTO_BSF`，由目标 muxer 请求 H.264/AAC 等必要的码流封装转换。现有 `common/codec_helpers` 继续服务原有 muxer/demuxer/streamer；v1 不把两套码流状态混在同一分支。

公开 API 使用 opaque pointer；FFmpeg 类型不会越过模块边界。统计值采用原子快照，配置和运行状态分别只有 pipeline 一个事实源。

## 兼容性、迁移与回滚

该模块是可选新增组件，不改变现有 `Codec`、`Demuxer`、`Muxer` 或 `Streamer`
行为。Runtime/RTP 支持新增 Pipeline 绑定 API，并把 Transport 中原有的
`rtp-packet`/`rtp-payload` 函数正式导出；源代码签名不变。现有调用方无需迁移。
新调用方可以先用 stream-copy 图替换手工转封装，再逐个媒体分支启用
decoder/filter/encoder。

若 Pipeline 生产验证失败，可停止链接 `TurboMedia::Pipeline` 并恢复原有手工流水线；
没有持久化数据迁移。FFmpeg 图新增 `libavfilter` 运行时依赖；生产 WebRTC session
需要随应用部署 TurboMedia/TurboNet、BoringSSL、libSRTP、usrsctp 及其上游运行时依赖。

## 生产 WebRTC backend

`turbo_media_webrtc_session_create()` 默认使用仓库内的 TurboMedia PeerConnection；
测试或定制接入仍可通过 `turbo_media_webrtc_session_create_with_backend()` 注入 backend
ops。内置 backend 组合 TurboNet ICE、TurboMedia DTLS/SRTP、SDP 和媒体引擎，负责完整
RTP packet 收发；ServerRuntime 仍是媒体 track metadata 与帧分发的主事实源。WHEP
player 会先应用远端 offer，再按 offer 的 media type、codec 与 mid 添加本地 send track；
不支持的 codec 或找不到兼容 m-line 时立即失败。

创建 session 的 owner 线程必须持续调用 `turbo_media_webrtc_session_pump()`，以推进 ICE、
DTLS、SRTP、DataChannel transport 与媒体定时器。`event_queue_capacity`、
`event_queue_max_bytes` 继续作为兼容配置接受并执行硬上限校验；内置 backend 的回调由
owner thread 驱动，不再建立额外事件队列。`max_rtp_packet_bytes` 限制单个 RTP packet，
0 使用 65535 bytes 默认值，超过上限会使 session 失败。

STUN/TURN URI 遵循 TurboNet ICE 配置格式，trickle ICE candidate 继续使用现有单字符串
ABI；`allow_loopback` 直接传给 TurboNet ICE，便于本机端到端测试。加密实现由构建期
BoringSSL 检查保证，SRTP 使用 libSRTP。

## 验证范围

单元测试覆盖严格 YAML、重复节点、环、非法端点、受限 filter、完整 fan-out、
Runtime/RTP 单输出约束、打开失败清理和 prepare 后取消。集成测试覆盖单/双输出
WAV 转封装、单/双输出音频重采样/转码，以及 raw YUV
缩放/视频转码；本地 HLS VOD 测试覆盖 playlist/segment 输入、正常 EOF/drain 与
Matroska 转封装。本地 Live HLS 测试通过动态端口 loopback HTTP 提供无
`#EXT-X-ENDLIST` 的两版 EVENT playlist，验证初始窗口进入 RUNNING、增量 segment
读取、packet stats 推进、协作停止与输出回收；该测试是确定性功能验收，不替代公网
CDN、鉴权、断线重连和不同 HLS 变体的兼容性验证。安装 smoke consumer 验证
`find_package`、公开头文件、链接和动态
加载。RTC 集成测试覆盖 H.264/Opus depay/pay、Opus decode/filter/libopus
实时再编码，以及 fake WHIP publisher → Pipeline → fake WHEP player。内置 PeerConnection
集成测试覆盖 H.264 WHIP/WHEP offer/answer、remote track owner-thread 注册与 send
track/mid 映射。真实浏览器跨 NAT 的 WHIP/WHEP、TURN、丢包恢复与拥塞控制仍需
在部署网络中做外部互操作和负载验证。
