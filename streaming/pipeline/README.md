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

完整配置见 [`examples/ffmpeg_pipeline.yml`](../../examples/ffmpeg_pipeline.yml)。最小调用顺序为：

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
[`examples/ffmpeg_multi_sink.yml`](../../examples/ffmpeg_multi_sink.yml)。示例只共享已编码
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
[`examples/runtime_rtp_relay.yml`](../../examples/runtime_rtp_relay.yml) 与
[`examples/runtime_rtp_transcode.yml`](../../examples/runtime_rtp_transcode.yml)。其中
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
[`examples/public_https_to_mpegts.yml`](../../examples/public_https_to_mpegts.yml)。
它使用 W3C 的 Sintel HTML video 测试资产验证 HTTPS MP4 输入到 MPEG-TS
文件输出；输出路径相对于 runner 的当前工作目录。

长时网络 smoke 配置还包括
[`examples/public_hls_to_matroska.yml`](../../examples/public_hls_to_matroska.yml)。
它锁定 Apple 示例的单一 video media playlist，避免 master playlist 的多
rendition 枚举，并适合手动执行带 `--stop-after` 的有界验证，不进入默认 CTest。
FFmpeg 依赖已启用 vcpkg 的 `xml2`、`opus` 与 `openh264` feature，使 `dash`
demuxer、实时 `libopus` 和 `libopenh264` 编码器可用。公开 DASH 转封装配置见
[`examples/public_dash_to_matroska.yml`](../../examples/public_dash_to_matroska.yml)。

## 应用逐帧编辑

视频转码图可在 `prepare` 前通过 `turbo_pipeline_set_video_frame_editor()`
注册同步回调。FFmpeg URL 和 Runtime/RTP 路径都在滤镜输出后、编码器接收前调用它。
滤镜链将输入转换为可写的 8 位 YUV420P 图像，所选编码器需支持该格式。回调可修改
Y、U、V 平面。时间戳使用编码器时钟，图像尺寸和时序由 Pipeline 管理。
每个送入编码器的帧只调用一次，编码器重试沿用已编辑的帧。

运行线程拥有帧。FFmpeg 在交给回调前确认存储可写，遇到共享帧时按需复制。
回调只在调用期间借用各平面的有效行字节，返回后这些指针会失效。
同一线程依次完成回调和编码，使用原有队列和背压，输出分支共享编辑后的编码结果。
应用应让回调及时返回，Pipeline 在回调返回后继续处理停止请求。
回调返回非零时，`run` 返回 `TURBO_PIPELINE_ECALLBACK`，错误详情包含编码器节点和
回调返回值。失败帧结束处理，已经发出的输出保持各自已写入的前缀。

例如，回调可以将每帧的亮度设为固定值。下面的代码按各行跨度定位，只改写有效像素。

```c
#include <string.h>
#include <turbo_pipeline.h>

static int set_luma(const turbo_pipeline_video_frame_t *frame, void *context) {
    const turbo_pipeline_video_plane_t *y = &frame->planes[0];
    const unsigned char value = *(const unsigned char *)context;
    for (size_t row = 0; row < y->rows; ++row) {
        memset(y->data + (ptrdiff_t)row * y->stride, value, y->row_bytes);
    }
    return 0;
}
```

应用创建视频转码图后调用
`turbo_pipeline_set_video_frame_editor(pipeline, set_luma, &value, &error)`，
检查成功后再 `prepare` 和 `run`。`value` 至少保留到 `run` 返回。

## 水印、画中画与拼屏

应用通过 `<turbo_pipeline_compositor.h>` 创建合成器，并在 `prepare` 前调用
`turbo_pipeline_bind_compositor()`。图层按数组顺序由底至顶绘制。省略主视频图层时，
Pipeline 的整张视频帧用作背景。为主视频和辅路分别配置目标矩形即可拼屏。
添加 RGBA 图片层可叠加水印、Logo 或图标，创建时复制并预处理图片。

以下配置在 1920×1080 的主画面右下角添加一路 640×360 的辅路，并在左上角放置 Logo。
`logo_rgba` 指向应用已解码的 160×64 RGBA8 图片。

```c
#include <turbo_pipeline_compositor.h>

static turbo_pipeline_compositor_t *bind_pip(
    turbo_pipeline_t *pipeline, const uint8_t *logo_rgba,
    turbo_pipeline_error_t *error) {
    const turbo_compositor_source_t sources[] = {
        {.width = 640, .height = 360, .slots = 3, .max_age_us = 200000}
    };
    const turbo_compositor_layer_t layers[] = {
        {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {1264, 696, 640, 360},
         .opacity = 255, .input.source = 0},
        {.kind = TURBO_COMPOSITOR_RGBA_IMAGE, .rect = {24, 24, 160, 64},
         .opacity = 255, .input.image = {160, 64, {logo_rgba, 160 * 64 * 4, 0, 160 * 4}}}
    };
    const turbo_compositor_config_t config = {
        .width = 1920, .height = 1080,
        .main_color = TURBO_COMPOSITOR_COLOR_FROM_FRAME,
        .main_anchor_pts = 0, .main_anchor_us = 0,
        .layers = layers, .layer_count = sizeof(layers) / sizeof(layers[0]),
        .sources = sources, .source_count = 1, .memory_limit = 8 * 1024 * 1024
    };
    turbo_pipeline_compositor_t *compositor = turbo_pipeline_compositor_create(&config, error);
    if (compositor &&
        turbo_pipeline_bind_compositor(pipeline, compositor, error) != TURBO_PIPELINE_OK) {
        turbo_pipeline_compositor_destroy(compositor);
        return NULL;
    }
    return compositor;
}
```

推流期间可以更新所有图层的位置。下面把同一组图层中的辅路移到左下角，
把 Logo 移到右上角。坐标按初始图层顺序提交，以画布左上角为原点，单位为像素。

```c
const turbo_compositor_position_t positions[] = {{16, 696}, {1736, 24}};
turbo_pipeline_status_t status =
    turbo_pipeline_compositor_set_positions(compositor, positions, 2);
```

位置必须为偶数，移动后的图层须完全位于画布内。合成线程每帧采样一次位置，
并在同一帧应用整组更新。两帧之间多次提交时，采用最近一次成功提交的位置。
图层宽高、不透明度和叠放顺序沿用创建时的配置。

应用通过 `turbo_pipeline_compositor_submit()` 提交该路已解码的 I420 平面，提交时间
使用和主路锚点一致的微秒时间轴。上例以主路编码器 PTS 0 为会话零点。三平面的
`first_row` 和 `stride` 表示实际存储布局，函数返回后输入缓冲区可复用。
队列满返回 `TURBO_PIPELINE_EBACKPRESSURE`。超过 200 ms 的辅路帧退出画面，
该图层显示黑色。若启用 `required`，缺帧会使运行失败。

合成使用 8 位 BT.709 SDR 限幅。主路可以使用带完整色彩元数据的 BT.709 或 BT.601，
也可为缺少元数据的源明确指定其已知色彩。辅路提交前应转换到 BT.709 限幅。
关闭时先调用 `turbo_pipeline_compositor_close()`，等待所有调用线程退出，再销毁
Pipeline 与合成器。主路音轨沿用原转码配置。

### N 路视频合到一个输出

在同一个视频转码 Pipeline 上绑定一个合成器。每个输出帧包含一条主路和
`source_count` 条辅路。输出画布、每路解码后的尺寸和每个格子的尺寸各自配置。

| 配置 | 含义 |
| --- | --- |
| `config.width, config.height` | 一路合成视频的输出分辨率。如 YAML 也指定编码宽高，须与画布一致。 |
| `sources[i].width, sources[i].height` | 第 `i` 路辅路实际提交的 I420 帧宽高，由解码结果决定。 |
| `layers[i].rect = {x, y, width, height}` | 图层在输出画布上的位置和区域大小，均用偶数像素。各路区域可独立设置。 |

例如，输出 1920×1080，左边给主路，右边纵向排三条辅路。每个画面等比缩放到自己的
区域，区域内剩余的部分填黑。按前面的示例创建并绑定合成器，然后把三个解码源的帧
分别提交到编号 0、1、2，并将各路时间戳映射到同一会话时间轴。

```c
const turbo_compositor_source_t sources[] = {
    {.width = 1280, .height = 720, .slots = 3, .max_age_us = 300000},
    {.width = 1920, .height = 1080, .slots = 3, .max_age_us = 300000},
    {.width = 720, .height = 1280, .slots = 3, .max_age_us = 300000}
};
const turbo_compositor_layer_t layers[] = {
    {.kind = TURBO_COMPOSITOR_MAIN_VIDEO, .rect = {0, 0, 1280, 1080}, .opacity = 255},
    {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {1280, 0, 640, 360}, .opacity = 255,
     .input.source = 0},
    {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {1280, 360, 640, 360}, .opacity = 255,
     .input.source = 1},
    {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {1280, 720, 640, 360}, .opacity = 255,
     .input.source = 2}
};
const turbo_compositor_config_t config = {
    .width = 1920, .height = 1080,
    .main_color = TURBO_COMPOSITOR_COLOR_FROM_FRAME,
    .main_anchor_pts = 0, .main_anchor_us = 0,
    .layers = layers, .layer_count = sizeof(layers) / sizeof(layers[0]),
    .sources = sources, .source_count = sizeof(sources) / sizeof(sources[0]),
    .memory_limit = 32 * 1024 * 1024
};
```

像素算法、容量计算与时间轴契约见
[视频叠加与多路合成设计](docs/video-compositor-design-zh.md)。

### Windows GPU 合成与亮度分析

用下面的构造选项启用 Direct3D 11，然后照常调用
`turbo_pipeline_bind_compositor()`。`luma_histogram = 1` 会统计每张合成画布的亮度，
应用可用 `turbo_pipeline_compositor_luma()` 读取最近一次结果、PTS 和时间基。
256 个桶对应 I420 的原始 Y 样本值，包含黑边和叠加层。关闭分析或尚未产出画布时，
查询返回 `TURBO_PIPELINE_ESTATE`。

```c
const turbo_compositor_options_t options = {
    .size = sizeof(options),
    .backend = TURBO_COMPOSITOR_D3D11,
    .luma_histogram = 1
};
turbo_pipeline_compositor_t *compositor =
    turbo_pipeline_compositor_create_ex(&config, &options, &error);
```

需要 Windows 和支持 Direct3D 11 feature level 11.0 的硬件。显式请求 GPU 时，设备
不可用或缓冲区超出预算会使创建失败。原构造函数使用 CPU，需要在 CPU 上读取直方图
时也可以给新构造函数传入 `TURBO_COMPOSITOR_CPU` 和 `luma_histogram = 1`。

GPU 执行图层混合和亮度统计。解码、预缩放、色彩转换和编码仍使用现有路径，每帧
上传 I420 数据并将合成结果读回给编码器。图层较少时，传输和同步会占用一部分帧时间。
缓冲区计入 `config.memory_limit`。 [公开接口](include/turbo_pipeline_compositor.h)

本机 Release 实测使用 RTX 4060 Laptop，输入为 48 帧 1920×1080 I420，叠加四层全屏
半透明 RGBA。开启亮度统计时，三次运行的中位耗时为 CPU 564 ms、GPU 234 ms。
计时覆盖 `turbo_pipeline_run()`，含读取、上传、读回和 rawvideo 输出到 null muxer，
初始化在计时前完成。

## 状态、所有权与失败

- pipeline 独占不可变配置、FFmpeg context、packet 与 frame。
- Runtime/RTP pipeline 借用 ServerRuntime 及输入、输出 source。保持对象有效到
  pipeline 销毁。订阅表由 Runtime 所属事件循环串行维护，`prepare` 与 `destroy`
  在该所有者上执行。runner 正常停止或出错时关闭队列接纳，原输入订阅忽略随后收到的包。
  等待 runner 及状态查询、停止调用退出，确认输入 publisher 已静止后，由 Runtime
  所有者销毁 pipeline 并退订。
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
- 视频转码的 `frame_rate > 0` 在用户滤镜之后执行固定帧率转换。选帧和补帧发生在
  逐帧编辑之前。省略或配置为 0 时保留滤镜输出的节奏，编码器采用输入时基。
  MPEG-4 Part 2 按其 16 位时基上限选取可表示的近似值。
- Runtime/RTP 转码按轨道延展 32 位输入 RTP 时间戳，首个时间戳沿用原值。轨道重启后
  创建并准备新的 pipeline 以建立时钟起点。Runtime 输出轨道尺寸和合成画布在 `prepare`
  期间统一校验，尺寸冲突时撤回该次准备的输出源。
- stream-copy 显式启用 FFmpeg `AVFMT_FLAG_AUTO_BSF`，由目标 muxer 请求 H.264/AAC 等必要的码流封装转换。现有 `media/containers/common/codec_helpers` 继续服务原有 muxer/demuxer/streamer；v1 不把两套码流状态混在同一分支。

公开 API 使用 opaque pointer；FFmpeg 类型不会越过模块边界。统计值采用原子快照，配置和运行状态分别只有 pipeline 一个事实源。

## 兼容性、迁移与回滚

该模块是可选新增组件，不改变现有 `Codec`、`Demuxer`、`Muxer` 或 `Streamer`
行为。Runtime/RTP 通过 `TurboMedia::RTP` 解析和封装媒体包。直接使用旧版 RTP 包接口的
调用方需要切换到 `<turbo_rtp_packet.h>` 和 `rtp_packet_parse`，并重新编译和链接。
新调用方可以先用 stream-copy 图替换手工转封装，再逐个媒体分支启用
decoder/filter/encoder。

若 Pipeline 生产验证失败，可停止链接 `TurboMedia::Pipeline` 并恢复原有手工流水线；
没有持久化数据迁移。FFmpeg 图新增 `libavfilter` 运行时依赖；生产 WebRTC session
需要随应用部署 TurboMedia、SaltsNet、SaltsCNet、libSRTP、usrsctp 及其上游运行时依赖。

## 生产 WebRTC backend

`turbo_media_webrtc_session_create()` 默认使用仓库内的 TurboMedia PeerConnection；
测试或定制接入仍可通过 `turbo_media_webrtc_session_create_with_backend()` 注入 backend
ops。内置 backend 组合 SaltsNet ICE、TurboMedia DTLS/SRTP、SDP 和媒体引擎，负责完整
RTP packet 收发；ServerRuntime 仍是媒体 track metadata 与帧分发的主事实源。WHEP
player 会先应用远端 offer，再按 offer 的 media type、codec 与 mid 添加本地 send track；
不支持的 codec 或找不到兼容 m-line 时立即失败。

创建 session 的 owner 线程必须持续调用 `turbo_media_webrtc_session_pump()`，以推进 ICE、
DTLS、SRTP、DataChannel transport 与媒体定时器。`event_queue_capacity`、
`event_queue_max_bytes` 继续作为兼容配置接受并执行硬上限校验；内置 backend 的回调由
owner thread 驱动，不再建立额外事件队列。`max_rtp_packet_bytes` 限制单个 RTP packet，
0 使用 65535 bytes 默认值，超过上限会使 session 失败。

STUN/TURN URI 遵循 SaltsNet ICE 配置格式，trickle ICE candidate 继续使用现有单字符串
ABI；`allow_loopback` 直接传给 SaltsNet ICE，便于本机端到端测试。加密实现由构建期
Salts Crypto 校验远端证书指纹并导出密钥，libSRTP 保护媒体报文。

## 验证范围

逐帧编辑用例检查 NV12 输入经过缩放和格式转换后的 YUV420P 各平面，
逐字节核对写出的像素，并验证 Runtime/RTP 视频路径、回调失败和控制面状态。
可用 `ctest --test-dir <build-dir> -R '^turbo_media_test_pipeline$' --output-on-failure`
运行这些用例。
合成器用例通过 `ctest --test-dir <build-dir> -R '^turbo_media_test_pipeline_compositor$'`
运行，验证 Alpha、负行跨度、合成像素、辅路选帧、满队列与 H.264 色彩标签。
有 Direct3D 11 硬件时，还会逐字节比较 CPU 和 GPU 的 I420 输出，并根据输出核对所有亮度桶和时间戳。

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
