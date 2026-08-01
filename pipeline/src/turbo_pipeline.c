#include "turbo_pipeline.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <data_bind.h>
#include <rtp-packet.h>
#include <rtp-payload.h>
#include <turbo_media_server.h>
#include <turbo_parser.h>
#include <turbo_thread.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>

#define PIPELINE_MAX_NODES 32u
#define PIPELINE_MAX_EDGES 64u
#define PIPELINE_MAX_OPTIONS 16u
#define PIPELINE_ID_SIZE 64u
#define PIPELINE_FACTORY_SIZE 64u
#define PIPELINE_URL_SIZE 1024u
#define PIPELINE_FORMAT_SIZE 64u
#define PIPELINE_CODEC_SIZE 64u
#define PIPELINE_FILTER_SIZE 1024u
#define PIPELINE_OPTION_KEY_SIZE 64u
#define PIPELINE_OPTION_VALUE_SIZE 256u
#define PIPELINE_DEFAULT_OPEN_TIMEOUT_MS 10000
#define PIPELINE_DEFAULT_IO_TIMEOUT_MS 10000
#define PIPELINE_MAX_TIMEOUT_MS 3600000
#define PIPELINE_RUNTIME_DEFAULT_QUEUE_CAPACITY 64u
#define PIPELINE_RUNTIME_MAX_QUEUE_CAPACITY 4096u
#define PIPELINE_RUNTIME_DEFAULT_PACKET_BYTES 2048u
#define PIPELINE_RUNTIME_MAX_PACKET_BYTES 65536u
#define PIPELINE_RUNTIME_DEFAULT_ACCESS_UNIT_BYTES (2u * 1024u * 1024u)
#define PIPELINE_RUNTIME_MAX_ACCESS_UNIT_BYTES (16u * 1024u * 1024u)
#define PIPELINE_RUNTIME_TRACK_COUNT 2u
#define PIPELINE_H264_START_CODE_SIZE 4u

typedef enum pipeline_execution_mode {
    PIPELINE_EXECUTION_FFMPEG = 0,
    PIPELINE_EXECUTION_RUNTIME_RTP
} pipeline_execution_mode_t;

static const char PIPELINE_SCHEMA[] =
    "message PipelineHeader { string api_version; string id; } "
    "message Limits { "
    "  optional int32 open_timeout_ms; "
    "  optional int32 io_timeout_ms; "
    "} "
    "message NodeConfig { "
    "  optional string url; "
    "  optional string format; "
    "  optional string media; "
    "  optional string codec; "
    "  optional string filters; "
    "  optional int64 bitrate; "
    "  optional int32 width; "
    "  optional int32 height; "
    "  optional int32 frame_rate; "
    "  optional int32 sample_rate; "
    "  optional int32 channels; "
    "  optional int32 gop_frames; "
    "  optional map<string,string> options; "
    "} "
    "message Node { "
    "  string id; "
    "  string kind; "
    "  string factory; "
    "} "
    "message Edge { string from; string to; }";

typedef enum pipeline_node_kind {
    PIPELINE_NODE_SOURCE = 0,
    PIPELINE_NODE_DEMUX,
    PIPELINE_NODE_DECODER,
    PIPELINE_NODE_FILTER,
    PIPELINE_NODE_ENCODER,
    PIPELINE_NODE_MUX,
    PIPELINE_NODE_SINK,
    PIPELINE_NODE_INVALID
} pipeline_node_kind_t;

typedef struct pipeline_option {
    char key[PIPELINE_OPTION_KEY_SIZE];
    char value[PIPELINE_OPTION_VALUE_SIZE];
} pipeline_option_t;

typedef struct pipeline_node_config {
    char url[PIPELINE_URL_SIZE];
    char format[PIPELINE_FORMAT_SIZE];
    char media[16];
    char codec[PIPELINE_CODEC_SIZE];
    char filters[PIPELINE_FILTER_SIZE];
    int64_t bitrate;
    int width;
    int height;
    int frame_rate;
    int sample_rate;
    int channels;
    int gop_frames;
    pipeline_option_t options[PIPELINE_MAX_OPTIONS];
    size_t option_count;
} pipeline_node_config_t;

typedef struct pipeline_node {
    char id[PIPELINE_ID_SIZE];
    pipeline_node_kind_t kind;
    char factory[PIPELINE_FACTORY_SIZE];
    pipeline_node_config_t config;
} pipeline_node_t;

typedef struct pipeline_edge {
    char from_id[PIPELINE_ID_SIZE];
    char from_pad[16];
    char to_id[PIPELINE_ID_SIZE];
    char to_pad[16];
} pipeline_edge_t;

typedef struct pipeline_runtime_track pipeline_runtime_track_t;

typedef struct pipeline_branch {
    enum AVMediaType media_type;
    int configured;
    int copy;
    int decoder_node;
    int filter_node;
    int encoder_node;
    int input_stream_index;
    AVRational input_time_base;
    AVStream *input_stream;
    AVStream *output_stream;
    AVCodecContext *decoder;
    AVCodecContext *encoder;
    AVFilterGraph *filter_graph;
    AVFilterContext *filter_source;
    AVFilterContext *filter_sink;
    AVRational filter_time_base;
    AVFrame *decoded_frame;
    AVFrame *filtered_frame;
    AVPacket *encoded_packet;
    pipeline_runtime_track_t *runtime_track;
} pipeline_branch_t;

typedef struct pipeline_runtime_queue_entry {
    turbo_media_frame_t frame;
    size_t slot;
} pipeline_runtime_queue_entry_t;

struct pipeline_runtime_track {
    int configured;
    int is_h264;
    int input_track_id;
    int output_track_id;
    turbo_media_track_info_t input_info;
    turbo_media_track_info_t output_info;
    void *decoder;
    void *encoder;
    uint32_t input_timestamp;
    int input_timestamp_valid;
    uint8_t *packet_buffer;
    size_t packet_capacity;
    uint8_t *access_unit;
    size_t access_unit_size;
    size_t access_unit_capacity;
    pipeline_branch_t *branch;
    turbo_pipeline_error_t *processing_error;
    struct turbo_pipeline *pipeline;
};

typedef struct pipeline_runtime_rtp {
    turbo_media_server_runtime_t *server_runtime;
    turbo_media_source_t *input_source;
    turbo_media_source_t *output_source;
    turbo_media_source_key_t input_key;
    turbo_media_source_key_t output_key;
    uint64_t subscription_id;
    pipeline_runtime_queue_entry_t *entries;
    uint8_t *packet_slots;
    size_t queue_capacity;
    size_t max_packet_bytes;
    size_t head;
    size_t tail;
    size_t count;
    int replay_cached;
    int accepting;
    int sync_initialized;
    int output_source_created;
    int output_source_committed;
    atomic_int async_status;
    turbo_mutex_t mutex;
    turbo_cond_t available;
    pipeline_runtime_track_t tracks[PIPELINE_RUNTIME_TRACK_COUNT];
} pipeline_runtime_rtp_t;

struct turbo_pipeline {
    char id[PIPELINE_ID_SIZE];
    pipeline_node_t nodes[PIPELINE_MAX_NODES];
    size_t node_count;
    pipeline_edge_t edges[PIPELINE_MAX_EDGES];
    size_t edge_count;
    int source_node;
    int demux_node;
    int mux_node;
    int sink_node;
    int open_timeout_ms;
    int io_timeout_ms;
    pipeline_execution_mode_t execution_mode;
    pipeline_branch_t audio;
    pipeline_branch_t video;
    AVFormatContext *input;
    AVFormatContext *output;
    AVDictionary *output_options;
    AVPacket *input_packet;
    pipeline_runtime_rtp_t runtime_rtp;
    int header_written;
    atomic_int state;
    atomic_int stop_requested;
    atomic_llong io_deadline_us;
    atomic_ullong packets_read;
    atomic_ullong packets_written;
    atomic_ullong frames_decoded;
    atomic_ullong frames_encoded;
    atomic_ullong bytes_read;
    atomic_ullong bytes_written;
};

static int pipeline_prepare_runtime_branch(
    turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
    pipeline_runtime_track_t *track, const turbo_media_track_info_t *input_info,
    turbo_media_track_info_t *output_info, turbo_pipeline_error_t *error);
static int pipeline_transcode_packet(turbo_pipeline_t *pipeline,
                                     pipeline_branch_t *branch,
                                     const AVPacket *packet,
                                     turbo_pipeline_error_t *error);

static void pipeline_error_clear(turbo_pipeline_error_t *error) {
    if (error) memset(error, 0, sizeof(*error));
}

static turbo_pipeline_status_t pipeline_error_set(turbo_pipeline_error_t *error,
                                                  turbo_pipeline_status_t code,
                                                  const char *node_id,
                                                  const char *format, ...) {
    va_list args;
    if (error) {
        memset(error, 0, sizeof(*error));
        error->code = code;
        if (node_id) av_strlcpy(error->node_id, node_id, sizeof(error->node_id));
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return code;
}

static turbo_pipeline_status_t pipeline_ffmpeg_error(turbo_pipeline_error_t *error,
                                                     const char *node_id,
                                                     const char *operation, int ffmpeg_code) {
    char detail[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(ffmpeg_code, detail, sizeof(detail));
    return pipeline_error_set(error, TURBO_PIPELINE_EFFMPEG, node_id, "%s: %s (%d)",
                              operation, detail, ffmpeg_code);
}

static int pipeline_copy_string(char *destination, size_t capacity, const char *source,
                                turbo_pipeline_error_t *error, const char *node_id,
                                const char *field) {
    size_t length;
    if (!source) return 1;
    length = strlen(source);
    if (length == 0 || length >= capacity) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node_id,
                           "%s length must be in [1, %zu]", field, capacity - 1u);
        return 0;
    }
    memcpy(destination, source, length + 1u);
    return 1;
}

static const DataBindValue *pipeline_field(const DataBindValue *object, const char *name) {
    return object ? data_bind_value_get(object, name) : NULL;
}

static int pipeline_key_allowed(const char *key, const char *const *allowed,
                                size_t allowed_count) {
    size_t i;
    if (!allowed) return 1;
    for (i = 0; i < allowed_count; ++i)
        if (strcmp(key, allowed[i]) == 0) return 1;
    return 0;
}

static int pipeline_validate_mapping_keys(const turbo_yaml_doc_t *document,
                                          const turbo_yaml_node_t *mapping,
                                          const char *path,
                                          const char *const *allowed,
                                          size_t allowed_count,
                                          turbo_pipeline_error_t *error) {
    size_t count;
    size_t i;
    if (!mapping || turbo_yaml_node_type(mapping) != TURBO_YAML_NODE_MAPPING) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "%s must be a YAML mapping", path);
        return 0;
    }
    count = turbo_yaml_mapping_size(mapping);
    for (i = 0; i < count; ++i) {
        turbo_yaml_node_t *key_node = turbo_yaml_mapping_key(mapping, i);
        char *key = turbo_yaml_scalar_dup(document, key_node);
        size_t j;
        if (!key) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                               "%s contains a non-string key", path);
            return 0;
        }
        if (!pipeline_key_allowed(key, allowed, allowed_count)) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                               "%s contains unknown field '%s'", path, key);
            turbo_yaml_string_free(key);
            return 0;
        }
        for (j = i + 1; j < count; ++j) {
            char *other = turbo_yaml_scalar_dup(
                document, turbo_yaml_mapping_key(mapping, j));
            int duplicate = other && strcmp(key, other) == 0;
            turbo_yaml_string_free(other);
            if (duplicate) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                                   "%s contains duplicate field '%s'", path, key);
                turbo_yaml_string_free(key);
                return 0;
            }
        }
        turbo_yaml_string_free(key);
    }
    return 1;
}

static int pipeline_validate_yaml_shape(const char *yaml, size_t yaml_size,
                                        turbo_pipeline_error_t *error) {
    static const char *const root_keys[] = {"api_version", "id", "limits", "nodes",
                                            "edges"};
    static const char *const limit_keys[] = {"open_timeout_ms", "io_timeout_ms"};
    static const char *const node_keys[] = {"id", "kind", "factory", "config"};
    static const char *const config_keys[] = {
        "url",         "format",      "media",       "codec",      "filters",
        "bitrate",     "width",       "height",      "frame_rate", "sample_rate",
        "channels",    "gop_frames",  "options"};
    static const char *const edge_keys[] = {"from", "to"};
    turbo_yaml_doc_t *document = NULL;
    turbo_yaml_node_t *root;
    turbo_yaml_node_t *limits;
    turbo_yaml_node_t *nodes;
    turbo_yaml_node_t *edges;
    size_t i;
    if (turbo_parse_yaml((const uint8_t *)yaml, yaml_size, &document) != 0 ||
        !document) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "YAML syntax validation failed");
        return 0;
    }
    root = turbo_yaml_root(document);
    if (!pipeline_validate_mapping_keys(document, root, "root", root_keys,
                                        sizeof(root_keys) / sizeof(root_keys[0]), error))
        goto fail;
    limits = turbo_yaml_mapping_get(document, root, "limits");
    if (limits &&
        !pipeline_validate_mapping_keys(document, limits, "limits", limit_keys,
                                        sizeof(limit_keys) / sizeof(limit_keys[0]), error))
        goto fail;
    nodes = turbo_yaml_mapping_get(document, root, "nodes");
    if (!nodes || turbo_yaml_node_type(nodes) != TURBO_YAML_NODE_SEQUENCE) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "nodes must be a YAML sequence");
        goto fail;
    }
    for (i = 0; i < turbo_yaml_sequence_size(nodes); ++i) {
        turbo_yaml_node_t *node = turbo_yaml_sequence_get(nodes, i);
        turbo_yaml_node_t *config;
        turbo_yaml_node_t *options;
        char path[64];
        snprintf(path, sizeof(path), "nodes[%zu]", i);
        if (!pipeline_validate_mapping_keys(document, node, path, node_keys,
                                            sizeof(node_keys) / sizeof(node_keys[0]),
                                            error))
            goto fail;
        config = turbo_yaml_mapping_get(document, node, "config");
        if (!config) continue;
        snprintf(path, sizeof(path), "nodes[%zu].config", i);
        if (!pipeline_validate_mapping_keys(
                document, config, path, config_keys,
                sizeof(config_keys) / sizeof(config_keys[0]), error))
            goto fail;
        options = turbo_yaml_mapping_get(document, config, "options");
        if (options) {
            snprintf(path, sizeof(path), "nodes[%zu].config.options", i);
            if (!pipeline_validate_mapping_keys(document, options, path, NULL, 0, error))
                goto fail;
        }
    }
    edges = turbo_yaml_mapping_get(document, root, "edges");
    if (!edges || turbo_yaml_node_type(edges) != TURBO_YAML_NODE_SEQUENCE) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "edges must be a YAML sequence");
        goto fail;
    }
    for (i = 0; i < turbo_yaml_sequence_size(edges); ++i) {
        char path[64];
        snprintf(path, sizeof(path), "edges[%zu]", i);
        if (!pipeline_validate_mapping_keys(
                document, turbo_yaml_sequence_get(edges, i), path, edge_keys,
                sizeof(edge_keys) / sizeof(edge_keys[0]), error))
            goto fail;
    }
    turbo_free_yaml(&document);
    return 1;

fail:
    turbo_free_yaml(&document);
    return 0;
}

static int pipeline_copy_optional_string(const DataBindValue *object, const char *name,
                                         char *destination, size_t capacity,
                                         turbo_pipeline_error_t *error, const char *node_id) {
    const DataBindValue *value = pipeline_field(object, name);
    if (!value) return 1;
    return pipeline_copy_string(destination, capacity, data_bind_value_as_string(value), error,
                                node_id, name);
}

static int pipeline_copy_options(const DataBindValue *config, pipeline_node_t *node,
                                 turbo_pipeline_error_t *error) {
    const DataBindValue *options = pipeline_field(config, "options");
    size_t i;
    if (!options) return 1;
    node->config.option_count = data_bind_value_count(options);
    if (node->config.option_count > PIPELINE_MAX_OPTIONS) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                           "options exceeds the limit of %u", PIPELINE_MAX_OPTIONS);
        return 0;
    }
    for (i = 0; i < node->config.option_count; ++i) {
        DataBindMapEntry entry = data_bind_value_map_entry_at(options, i);
        if (!pipeline_copy_string(node->config.options[i].key,
                                  sizeof(node->config.options[i].key), entry.key, error,
                                  node->id, "option key") ||
            !pipeline_copy_string(node->config.options[i].value,
                                  sizeof(node->config.options[i].value),
                                  data_bind_value_as_string(entry.value), error, node->id,
                                  "option value"))
            return 0;
    }
    return 1;
}

static pipeline_node_kind_t pipeline_parse_kind(const char *kind) {
    if (!kind) return PIPELINE_NODE_INVALID;
    if (strcmp(kind, "source") == 0) return PIPELINE_NODE_SOURCE;
    if (strcmp(kind, "demux") == 0) return PIPELINE_NODE_DEMUX;
    if (strcmp(kind, "decoder") == 0) return PIPELINE_NODE_DECODER;
    if (strcmp(kind, "filter") == 0) return PIPELINE_NODE_FILTER;
    if (strcmp(kind, "encoder") == 0) return PIPELINE_NODE_ENCODER;
    if (strcmp(kind, "mux") == 0) return PIPELINE_NODE_MUX;
    if (strcmp(kind, "sink") == 0) return PIPELINE_NODE_SINK;
    return PIPELINE_NODE_INVALID;
}

static int pipeline_factory_valid(pipeline_node_kind_t kind, const char *factory) {
    static const char *ffmpeg_factories[] = {
        "ffmpeg.input", "ffmpeg.demux", "ffmpeg.decode", "ffmpeg.filter",
        "ffmpeg.encode", "ffmpeg.mux", "ffmpeg.output"};
    if (!factory || kind >= PIPELINE_NODE_INVALID) return 0;
    if (strcmp(factory, ffmpeg_factories[kind]) == 0) return 1;
    return (kind == PIPELINE_NODE_SOURCE &&
            strcmp(factory, "media.runtime_source") == 0) ||
           (kind == PIPELINE_NODE_DEMUX &&
            strcmp(factory, "rtp.depacketize") == 0) ||
           (kind == PIPELINE_NODE_MUX &&
            strcmp(factory, "rtp.packetize") == 0) ||
           (kind == PIPELINE_NODE_SINK &&
            strcmp(factory, "media.runtime_sink") == 0);
}

static int pipeline_read_node_config(const DataBindValue *config, pipeline_node_t *node,
                                     turbo_pipeline_error_t *error) {
    const DataBindValue *field;
    if (!config) return 1;
    if (!pipeline_copy_optional_string(config, "url", node->config.url,
                                       sizeof(node->config.url), error, node->id) ||
        !pipeline_copy_optional_string(config, "format", node->config.format,
                                       sizeof(node->config.format), error, node->id) ||
        !pipeline_copy_optional_string(config, "media", node->config.media,
                                       sizeof(node->config.media), error, node->id) ||
        !pipeline_copy_optional_string(config, "codec", node->config.codec,
                                       sizeof(node->config.codec), error, node->id) ||
        !pipeline_copy_optional_string(config, "filters", node->config.filters,
                                       sizeof(node->config.filters), error, node->id) ||
        !pipeline_copy_options(config, node, error))
        return 0;

#define PIPELINE_READ_INT(name, member)                                                    \
    do {                                                                                   \
        field = pipeline_field(config, name);                                               \
        if (field) node->config.member = data_bind_value_as_int(field);                     \
    } while (0)
    field = pipeline_field(config, "bitrate");
    if (field) node->config.bitrate = data_bind_value_as_int64(field);
    PIPELINE_READ_INT("width", width);
    PIPELINE_READ_INT("height", height);
    PIPELINE_READ_INT("frame_rate", frame_rate);
    PIPELINE_READ_INT("sample_rate", sample_rate);
    PIPELINE_READ_INT("channels", channels);
    PIPELINE_READ_INT("gop_frames", gop_frames);
#undef PIPELINE_READ_INT
    return 1;
}

static int pipeline_parse_endpoint(const char *text, char *id, size_t id_capacity, char *pad,
                                   size_t pad_capacity, turbo_pipeline_error_t *error) {
    const char *separator = text ? strrchr(text, '.') : NULL;
    size_t id_length;
    size_t pad_length;
    if (!separator || separator == text || separator[1] == '\0') {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                           "endpoint '%s' must use node_id.pad syntax", text ? text : "");
        return 0;
    }
    id_length = (size_t)(separator - text);
    pad_length = strlen(separator + 1);
    if (id_length >= id_capacity || pad_length >= pad_capacity) {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                           "endpoint '%s' exceeds the configured identifier limit", text);
        return 0;
    }
    memcpy(id, text, id_length);
    id[id_length] = '\0';
    memcpy(pad, separator + 1, pad_length + 1u);
    return 1;
}

static int pipeline_find_node(const turbo_pipeline_t *pipeline, const char *id) {
    size_t i;
    for (i = 0; i < pipeline->node_count; ++i)
        if (strcmp(pipeline->nodes[i].id, id) == 0) return (int)i;
    return -1;
}

static int pipeline_find_unique_edge(const turbo_pipeline_t *pipeline, const char *from_id,
                                     const char *from_pad, int *edge_index,
                                     turbo_pipeline_error_t *error) {
    size_t i;
    int found = -1;
    for (i = 0; i < pipeline->edge_count; ++i) {
        if (strcmp(pipeline->edges[i].from_id, from_id) == 0 &&
            strcmp(pipeline->edges[i].from_pad, from_pad) == 0) {
            if (found >= 0) {
                pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, from_id,
                                   "output pad '%s' has more than one edge", from_pad);
                return -1;
            }
            found = (int)i;
        }
    }
    *edge_index = found;
    return 0;
}

static int pipeline_media_matches(const pipeline_node_t *node, const char *media) {
    return strcmp(node->config.media, media) == 0;
}

static int pipeline_trace_branch(turbo_pipeline_t *pipeline, const char *media,
                                 enum AVMediaType media_type, unsigned char *used_nodes,
                                 unsigned char *used_edges, pipeline_branch_t *branch,
                                 turbo_pipeline_error_t *error) {
    const pipeline_node_t *demux = &pipeline->nodes[pipeline->demux_node];
    int edge_index;
    int node_index;
    const pipeline_edge_t *edge;
    const pipeline_node_t *node;

    branch->media_type = media_type;
    branch->decoder_node = -1;
    branch->filter_node = -1;
    branch->encoder_node = -1;
    branch->input_stream_index = -1;

    if (pipeline_find_unique_edge(pipeline, demux->id, media, &edge_index, error) != 0)
        return 0;
    if (edge_index < 0) return 1;

    branch->configured = 1;
    used_edges[edge_index] = 1;
    edge = &pipeline->edges[edge_index];
    node_index = pipeline_find_node(pipeline, edge->to_id);
    node = &pipeline->nodes[node_index];

    if (node->kind == PIPELINE_NODE_MUX) {
        if (node_index != pipeline->mux_node || strcmp(edge->to_pad, media) != 0) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                               "stream-copy branch must end at mux.%s", media);
            return 0;
        }
        branch->copy = 1;
        return 1;
    }

    if (node->kind != PIPELINE_NODE_DECODER || strcmp(edge->to_pad, "in") != 0 ||
        !pipeline_media_matches(node, media)) {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                           "%s branch must connect to a matching decoder.in or mux.%s",
                           media, media);
        return 0;
    }
    branch->decoder_node = node_index;
    used_nodes[node_index] = 1;

    if (pipeline_find_unique_edge(pipeline, node->id, "out", &edge_index, error) != 0 ||
        edge_index < 0) {
        if (!error || error->code == TURBO_PIPELINE_OK)
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                               "decoder.out must have exactly one edge");
        return 0;
    }
    used_edges[edge_index] = 1;
    edge = &pipeline->edges[edge_index];
    node_index = pipeline_find_node(pipeline, edge->to_id);
    node = &pipeline->nodes[node_index];

    if (node->kind == PIPELINE_NODE_FILTER) {
        if (strcmp(edge->to_pad, "in") != 0 || !pipeline_media_matches(node, media)) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                               "filter must use a matching media and its in pad");
            return 0;
        }
        branch->filter_node = node_index;
        used_nodes[node_index] = 1;
        if (strchr(node->config.filters, ';') || strchr(node->config.filters, '[') ||
            strchr(node->config.filters, ']')) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                               "v1 filters must be a single-input/single-output chain");
            return 0;
        }
        if (pipeline_find_unique_edge(pipeline, node->id, "out", &edge_index, error) != 0 ||
            edge_index < 0) {
            if (!error || error->code == TURBO_PIPELINE_OK)
                pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                                   "filter.out must have exactly one edge");
            return 0;
        }
        used_edges[edge_index] = 1;
        edge = &pipeline->edges[edge_index];
        node_index = pipeline_find_node(pipeline, edge->to_id);
        node = &pipeline->nodes[node_index];
    }

    if (node->kind != PIPELINE_NODE_ENCODER || strcmp(edge->to_pad, "in") != 0 ||
        !pipeline_media_matches(node, media) || node->config.codec[0] == '\0') {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                           "%s transcode branch requires a matching encoder.in with codec",
                           media);
        return 0;
    }
    branch->encoder_node = node_index;
    used_nodes[node_index] = 1;

    if (pipeline_find_unique_edge(pipeline, node->id, "out", &edge_index, error) != 0 ||
        edge_index < 0) {
        if (!error || error->code == TURBO_PIPELINE_OK)
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                               "encoder.out must have exactly one edge");
        return 0;
    }
    used_edges[edge_index] = 1;
    edge = &pipeline->edges[edge_index];
    if (pipeline_find_node(pipeline, edge->to_id) != pipeline->mux_node ||
        strcmp(edge->to_pad, media) != 0) {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                           "encoder.out must connect to mux.%s", media);
        return 0;
    }
    return 1;
}

static int pipeline_validate_dag(const turbo_pipeline_t *pipeline,
                                 turbo_pipeline_error_t *error) {
    unsigned char indegree[PIPELINE_MAX_NODES] = {0};
    unsigned char removed[PIPELINE_MAX_NODES] = {0};
    size_t removed_count = 0;
    size_t i;
    for (i = 0; i < pipeline->edge_count; ++i) {
        int target = pipeline_find_node(pipeline, pipeline->edges[i].to_id);
        if (target < 0) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                               "edge target node '%s' does not exist",
                               pipeline->edges[i].to_id);
            return 0;
        }
        if (pipeline_find_node(pipeline, pipeline->edges[i].from_id) < 0) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                               "edge source node '%s' does not exist",
                               pipeline->edges[i].from_id);
            return 0;
        }
        if (indegree[target] == UCHAR_MAX) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH,
                               pipeline->nodes[target].id, "node indegree exceeds limit");
            return 0;
        }
        ++indegree[target];
    }
    while (removed_count < pipeline->node_count) {
        int progressed = 0;
        for (i = 0; i < pipeline->node_count; ++i) {
            size_t j;
            if (removed[i] || indegree[i] != 0) continue;
            removed[i] = 1;
            ++removed_count;
            progressed = 1;
            for (j = 0; j < pipeline->edge_count; ++j) {
                if (strcmp(pipeline->edges[j].from_id, pipeline->nodes[i].id) == 0) {
                    int target = pipeline_find_node(pipeline, pipeline->edges[j].to_id);
                    --indegree[target];
                }
            }
        }
        if (!progressed) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                               "graph contains a cycle");
            return 0;
        }
    }
    return 1;
}

static int pipeline_validate_backbone(turbo_pipeline_t *pipeline, unsigned char *used_nodes,
                                      unsigned char *used_edges,
                                      turbo_pipeline_error_t *error) {
    int source_edge;
    int mux_edge;
    const pipeline_node_t *source = &pipeline->nodes[pipeline->source_node];
    const pipeline_node_t *mux = &pipeline->nodes[pipeline->mux_node];
    if (pipeline_find_unique_edge(pipeline, source->id, "out", &source_edge, error) != 0 ||
        source_edge < 0) {
        if (!error || error->code == TURBO_PIPELINE_OK)
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, source->id,
                               "source.out must have exactly one edge");
        return 0;
    }
    if (pipeline_find_node(pipeline, pipeline->edges[source_edge].to_id) !=
            pipeline->demux_node ||
        strcmp(pipeline->edges[source_edge].to_pad, "in") != 0) {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, source->id,
                           "source.out must connect to demux.in");
        return 0;
    }
    if (pipeline_find_unique_edge(pipeline, mux->id, "out", &mux_edge, error) != 0 ||
        mux_edge < 0) {
        if (!error || error->code == TURBO_PIPELINE_OK)
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, mux->id,
                               "mux.out must have exactly one edge");
        return 0;
    }
    if (pipeline_find_node(pipeline, pipeline->edges[mux_edge].to_id) !=
            pipeline->sink_node ||
        strcmp(pipeline->edges[mux_edge].to_pad, "in") != 0) {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, mux->id,
                           "mux.out must connect to sink.in");
        return 0;
    }
    used_nodes[pipeline->source_node] = 1;
    used_nodes[pipeline->demux_node] = 1;
    used_nodes[pipeline->mux_node] = 1;
    used_nodes[pipeline->sink_node] = 1;
    used_edges[source_edge] = 1;
    used_edges[mux_edge] = 1;
    return 1;
}

static int pipeline_config_has_numbers(const pipeline_node_config_t *config) {
    return config->bitrate != 0 || config->width != 0 || config->height != 0 ||
           config->frame_rate != 0 || config->sample_rate != 0 ||
           config->channels != 0 || config->gop_frames != 0;
}

static int pipeline_validate_node_config(const pipeline_node_t *node,
                                         turbo_pipeline_error_t *error) {
    const pipeline_node_config_t *config = &node->config;
    int media_is_audio = strcmp(config->media, "audio") == 0;
    int media_is_video = strcmp(config->media, "video") == 0;
    if (config->bitrate < 0 || config->width < 0 || config->height < 0 ||
        config->frame_rate < 0 || config->sample_rate < 0 ||
        config->channels < 0 || config->gop_frames < 0) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                           "numeric configuration values cannot be negative");
        return 0;
    }
    switch (node->kind) {
        case PIPELINE_NODE_SOURCE:
            if (!config->url[0] || config->format[0] || config->media[0] ||
                config->codec[0] || config->filters[0] ||
                pipeline_config_has_numbers(config)) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   "source accepts only url and options");
                return 0;
            }
            break;
        case PIPELINE_NODE_DEMUX:
            if (config->url[0] || config->media[0] || config->codec[0] ||
                config->filters[0] || pipeline_config_has_numbers(config) ||
                (strcmp(node->factory, "rtp.depacketize") == 0 &&
                 (config->format[0] || config->option_count != 0))) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   strcmp(node->factory, "rtp.depacketize") == 0
                                       ? "RTP depacketizer accepts no config in v1"
                                       : "demux accepts only format and options");
                return 0;
            }
            break;
        case PIPELINE_NODE_DECODER:
            if ((!media_is_audio && !media_is_video) || config->url[0] ||
                config->format[0] || config->codec[0] || config->filters[0] ||
                pipeline_config_has_numbers(config)) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   "decoder accepts media and decoder options");
                return 0;
            }
            break;
        case PIPELINE_NODE_FILTER:
            if ((!media_is_audio && !media_is_video) || !config->filters[0] ||
                config->url[0] || config->format[0] || config->codec[0] ||
                config->option_count != 0 || pipeline_config_has_numbers(config)) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   "filter requires media and filters only");
                return 0;
            }
            break;
        case PIPELINE_NODE_ENCODER:
            if ((!media_is_audio && !media_is_video) || !config->codec[0] ||
                config->url[0] || config->format[0] || config->filters[0] ||
                (media_is_audio &&
                 (config->width || config->height || config->frame_rate ||
                  config->gop_frames)) ||
                (media_is_video && (config->sample_rate || config->channels))) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   "encoder configuration does not match its media type");
                return 0;
            }
            break;
        case PIPELINE_NODE_MUX:
            if (config->url[0] || config->media[0] || config->codec[0] ||
                config->filters[0] || pipeline_config_has_numbers(config) ||
                (strcmp(node->factory, "rtp.packetize") == 0 &&
                 (config->format[0] || config->option_count != 0))) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   strcmp(node->factory, "rtp.packetize") == 0
                                       ? "RTP packetizer accepts no config in v1"
                                       : "mux accepts only format and options");
                return 0;
            }
            break;
        case PIPELINE_NODE_SINK:
            if (!config->url[0] || config->format[0] || config->media[0] ||
                config->codec[0] || config->filters[0] ||
                pipeline_config_has_numbers(config)) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   "sink accepts only url and options");
                return 0;
            }
            break;
        default:
            return 0;
    }
    return 1;
}

static int pipeline_validate_graph(turbo_pipeline_t *pipeline,
                                   turbo_pipeline_error_t *error) {
    unsigned char used_nodes[PIPELINE_MAX_NODES] = {0};
    unsigned char used_edges[PIPELINE_MAX_EDGES] = {0};
    size_t i;
    int kind_counts[PIPELINE_NODE_INVALID] = {0};

    for (i = 0; i < pipeline->node_count; ++i) {
        pipeline_node_t *node = &pipeline->nodes[i];
        size_t j;
        if (node->kind == PIPELINE_NODE_INVALID ||
            !pipeline_factory_valid(node->kind, node->factory)) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                               "factory '%s' is not valid for this node kind",
                               node->factory);
            return 0;
        }
        if (!pipeline_validate_node_config(node, error)) return 0;
        ++kind_counts[node->kind];
        for (j = i + 1; j < pipeline->node_count; ++j) {
            if (strcmp(node->id, pipeline->nodes[j].id) == 0) {
                pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, node->id,
                                   "node id is duplicated");
                return 0;
            }
        }
    }
    if (kind_counts[PIPELINE_NODE_SOURCE] != 1 ||
        kind_counts[PIPELINE_NODE_DEMUX] != 1 ||
        kind_counts[PIPELINE_NODE_MUX] != 1 ||
        kind_counts[PIPELINE_NODE_SINK] != 1) {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                           "v1 requires exactly one source, demux, mux and sink node");
        return 0;
    }
    for (i = 0; i < pipeline->node_count; ++i) {
        switch (pipeline->nodes[i].kind) {
            case PIPELINE_NODE_SOURCE: pipeline->source_node = (int)i; break;
            case PIPELINE_NODE_DEMUX: pipeline->demux_node = (int)i; break;
            case PIPELINE_NODE_MUX: pipeline->mux_node = (int)i; break;
            case PIPELINE_NODE_SINK: pipeline->sink_node = (int)i; break;
            default: break;
        }
    }
    if (pipeline->nodes[pipeline->source_node].config.url[0] == '\0' ||
        pipeline->nodes[pipeline->sink_node].config.url[0] == '\0') {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "source and sink nodes require non-empty URLs");
        return 0;
    }
    if (strcmp(pipeline->nodes[pipeline->source_node].factory, "ffmpeg.input") == 0 &&
        strcmp(pipeline->nodes[pipeline->demux_node].factory, "ffmpeg.demux") == 0 &&
        strcmp(pipeline->nodes[pipeline->mux_node].factory, "ffmpeg.mux") == 0 &&
        strcmp(pipeline->nodes[pipeline->sink_node].factory, "ffmpeg.output") == 0) {
        pipeline->execution_mode = PIPELINE_EXECUTION_FFMPEG;
    } else if (
        strcmp(pipeline->nodes[pipeline->source_node].factory,
               "media.runtime_source") == 0 &&
        strcmp(pipeline->nodes[pipeline->demux_node].factory,
               "rtp.depacketize") == 0 &&
        strcmp(pipeline->nodes[pipeline->mux_node].factory,
               "rtp.packetize") == 0 &&
        strcmp(pipeline->nodes[pipeline->sink_node].factory,
               "media.runtime_sink") == 0) {
        pipeline->execution_mode = PIPELINE_EXECUTION_RUNTIME_RTP;
    } else {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                           "source, demux, mux and sink factories must form one "
                           "FFmpeg or Runtime/RTP execution path");
        return 0;
    }
    if (!pipeline_validate_dag(pipeline, error) ||
        !pipeline_validate_backbone(pipeline, used_nodes, used_edges, error) ||
        !pipeline_trace_branch(pipeline, "audio", AVMEDIA_TYPE_AUDIO, used_nodes,
                               used_edges, &pipeline->audio, error) ||
        !pipeline_trace_branch(pipeline, "video", AVMEDIA_TYPE_VIDEO, used_nodes,
                               used_edges, &pipeline->video, error))
        return 0;
    if (!pipeline->audio.configured && !pipeline->video.configured) {
        pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, NULL,
                           "graph must contain an audio or video branch");
        return 0;
    }
    for (i = 0; i < pipeline->node_count; ++i) {
        if (!used_nodes[i]) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, pipeline->nodes[i].id,
                               "node is not part of a supported v1 execution path");
            return 0;
        }
    }
    for (i = 0; i < pipeline->edge_count; ++i) {
        if (!used_edges[i]) {
            pipeline_error_set(error, TURBO_PIPELINE_EGRAPH, pipeline->edges[i].from_id,
                               "edge from %s.%s is not part of a supported v1 path",
                               pipeline->edges[i].from_id, pipeline->edges[i].from_pad);
            return 0;
        }
    }
    return 1;
}

static int pipeline_read_yaml(turbo_pipeline_t *pipeline, const char *yaml, size_t yaml_size,
                              turbo_pipeline_error_t *error) {
    DataBind *codec = NULL;
    DataBindError bind_error = {sizeof(bind_error)};
    DataBindValue *header = NULL;
    DataBindValue *nodes = NULL;
    DataBindValue *edges = NULL;
    DataBindValue *limits = NULL;
    size_t i;
    DataBindStatus status;

    if (data_bind_create_from_text(PIPELINE_SCHEMA, sizeof(PIPELINE_SCHEMA) - 1u, &codec,
                                   &bind_error) != DATA_BIND_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "internal pipeline schema is invalid: %s", bind_error.message);
        return 0;
    }
    bind_error.size = sizeof(bind_error);
    if (data_bind_parse_yaml(codec, "PipelineHeader", yaml, yaml_size, &header,
                             &bind_error) != DATA_BIND_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "YAML validation failed at %d:%d%s%s: %s", bind_error.line,
                           bind_error.column, bind_error.path[0] ? " path " : "",
                           bind_error.path, bind_error.message);
        data_bind_free(codec);
        return 0;
    }
    if (strcmp(data_bind_value_as_string(pipeline_field(header, "api_version")),
               "turbo.media.pipeline/v1") != 0) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "api_version must be turbo.media.pipeline/v1");
        goto fail;
    }
    if (!pipeline_copy_string(pipeline->id, sizeof(pipeline->id),
                              data_bind_value_as_string(pipeline_field(header, "id")), error,
                              NULL, "id"))
        goto fail;

    pipeline->open_timeout_ms = PIPELINE_DEFAULT_OPEN_TIMEOUT_MS;
    pipeline->io_timeout_ms = PIPELINE_DEFAULT_IO_TIMEOUT_MS;
    bind_error.size = sizeof(bind_error);
    status = data_bind_parse_yaml_path(codec, "Limits", yaml, yaml_size, "/limits",
                                       &limits, &bind_error);
    if (status == DATA_BIND_OK) {
        const DataBindValue *value = pipeline_field(limits, "open_timeout_ms");
        if (value) pipeline->open_timeout_ms = data_bind_value_as_int(value);
        value = pipeline_field(limits, "io_timeout_ms");
        if (value) pipeline->io_timeout_ms = data_bind_value_as_int(value);
    } else if (!strstr(bind_error.message, "matched no values")) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "limits validation failed: %s", bind_error.message);
        goto fail;
    }
    if (pipeline->open_timeout_ms <= 0 ||
        pipeline->open_timeout_ms > PIPELINE_MAX_TIMEOUT_MS ||
        pipeline->io_timeout_ms <= 0 || pipeline->io_timeout_ms > PIPELINE_MAX_TIMEOUT_MS) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "timeouts must be in [1, %d] milliseconds",
                           PIPELINE_MAX_TIMEOUT_MS);
        goto fail;
    }

    bind_error.size = sizeof(bind_error);
    if (data_bind_parse_yaml_path_all(codec, "Node", yaml, yaml_size, "/nodes[*]",
                                      &nodes, &bind_error) != DATA_BIND_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "nodes validation failed: %s", bind_error.message);
        goto fail;
    }
    pipeline->node_count = data_bind_value_count(nodes);
    if (pipeline->node_count == 0 || pipeline->node_count > PIPELINE_MAX_NODES) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "nodes count must be in [1, %u]", PIPELINE_MAX_NODES);
        goto fail;
    }
    for (i = 0; i < pipeline->node_count; ++i) {
        const DataBindValue *value = data_bind_value_at(nodes, i);
        pipeline_node_t *node = &pipeline->nodes[i];
        DataBindValue *config = NULL;
        char config_path[64];
        const char *kind;
        if (!pipeline_copy_string(node->id, sizeof(node->id),
                                  data_bind_value_as_string(pipeline_field(value, "id")),
                                  error, NULL, "node id"))
            goto fail;
        kind = data_bind_value_as_string(pipeline_field(value, "kind"));
        node->kind = pipeline_parse_kind(kind);
        if (!pipeline_copy_string(node->factory, sizeof(node->factory),
                                  data_bind_value_as_string(pipeline_field(value, "factory")),
                                  error, node->id, "factory"))
            goto fail;
        snprintf(config_path, sizeof(config_path), "/nodes[%zu]/config", i);
        bind_error.size = sizeof(bind_error);
        status = data_bind_parse_yaml_path(codec, "NodeConfig", yaml, yaml_size,
                                           config_path, &config, &bind_error);
        if (status == DATA_BIND_OK) {
            if (!pipeline_read_node_config(config, node, error)) {
                data_bind_value_free(config);
                goto fail;
            }
            data_bind_value_free(config);
        } else if (!strstr(bind_error.message, "matched no values")) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                               "node config validation failed: %s",
                               bind_error.message);
            goto fail;
        }
    }

    bind_error.size = sizeof(bind_error);
    if (data_bind_parse_yaml_path_all(codec, "Edge", yaml, yaml_size, "/edges[*]",
                                      &edges, &bind_error) != DATA_BIND_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "edges validation failed: %s", bind_error.message);
        goto fail;
    }
    pipeline->edge_count = data_bind_value_count(edges);
    if (pipeline->edge_count == 0 || pipeline->edge_count > PIPELINE_MAX_EDGES) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, NULL,
                           "edges count must be in [1, %u]", PIPELINE_MAX_EDGES);
        goto fail;
    }
    for (i = 0; i < pipeline->edge_count; ++i) {
        const DataBindValue *value = data_bind_value_at(edges, i);
        const char *from = data_bind_value_as_string(pipeline_field(value, "from"));
        const char *to = data_bind_value_as_string(pipeline_field(value, "to"));
        if (!pipeline_parse_endpoint(from, pipeline->edges[i].from_id,
                                     sizeof(pipeline->edges[i].from_id),
                                     pipeline->edges[i].from_pad,
                                     sizeof(pipeline->edges[i].from_pad), error) ||
            !pipeline_parse_endpoint(to, pipeline->edges[i].to_id,
                                     sizeof(pipeline->edges[i].to_id),
                                     pipeline->edges[i].to_pad,
                                     sizeof(pipeline->edges[i].to_pad), error))
            goto fail;
    }
    data_bind_value_free(edges);
    data_bind_value_free(nodes);
    data_bind_value_free(limits);
    data_bind_value_free(header);
    data_bind_free(codec);
    return 1;

fail:
    data_bind_value_free(edges);
    data_bind_value_free(nodes);
    data_bind_value_free(limits);
    data_bind_value_free(header);
    data_bind_free(codec);
    return 0;
}

static int pipeline_interrupt(void *opaque) {
    turbo_pipeline_t *pipeline = (turbo_pipeline_t *)opaque;
    long long deadline;
    if (!pipeline) return 0;
    if (atomic_load_explicit(&pipeline->stop_requested, memory_order_relaxed)) return 1;
    deadline = atomic_load_explicit(&pipeline->io_deadline_us, memory_order_relaxed);
    return deadline > 0 && av_gettime_relative() >= deadline;
}

static void pipeline_set_deadline(turbo_pipeline_t *pipeline, int timeout_ms) {
    long long deadline = av_gettime_relative() + (long long)timeout_ms * 1000LL;
    atomic_store_explicit(&pipeline->io_deadline_us, deadline, memory_order_relaxed);
}

static void pipeline_clear_deadline(turbo_pipeline_t *pipeline) {
    atomic_store_explicit(&pipeline->io_deadline_us, 0, memory_order_relaxed);
}

static const char *pipeline_option_value(const pipeline_node_t *node,
                                         const char *key) {
    size_t i;
    const char *value = NULL;
    for (i = 0; i < node->config.option_count; ++i) {
        if (strcmp(node->config.options[i].key, key) == 0) {
            if (value) return NULL;
            value = node->config.options[i].value;
        }
    }
    return value;
}

static int pipeline_runtime_size_option(const pipeline_node_t *node,
                                        const char *key,
                                        size_t default_value,
                                        size_t maximum,
                                        size_t *result,
                                        turbo_pipeline_error_t *error) {
    const char *value = pipeline_option_value(node, key);
    char *end = NULL;
    unsigned long long parsed;
    if (!value) {
        *result = default_value;
        return 1;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
        parsed > maximum) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                           "option '%s' must be an integer in [1, %zu]",
                           key, maximum);
        return 0;
    }
    *result = (size_t)parsed;
    return 1;
}

static int pipeline_runtime_bool_option(const pipeline_node_t *node,
                                        const char *key,
                                        int default_value,
                                        int *result,
                                        turbo_pipeline_error_t *error) {
    const char *value = pipeline_option_value(node, key);
    if (!value) {
        *result = default_value;
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        *result = 0;
        return 1;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        *result = 1;
        return 1;
    }
    pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                       "option '%s' must be true, false, 1 or 0", key);
    return 0;
}

static int pipeline_runtime_options_valid(const pipeline_node_t *node,
                                          const char *const *allowed,
                                          size_t allowed_count,
                                          turbo_pipeline_error_t *error) {
    size_t i;
    size_t j;
    for (i = 0; i < node->config.option_count; ++i) {
        int found = 0;
        for (j = 0; j < allowed_count; ++j) {
            if (strcmp(node->config.options[i].key, allowed[j]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                               "unsupported Runtime/RTP option '%s'",
                               node->config.options[i].key);
            return 0;
        }
        for (j = i + 1; j < node->config.option_count; ++j) {
            if (strcmp(node->config.options[i].key,
                       node->config.options[j].key) == 0) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                                   "Runtime/RTP option '%s' is duplicated",
                                   node->config.options[i].key);
                return 0;
            }
        }
    }
    return 1;
}

static int pipeline_runtime_parse_key(const pipeline_node_t *node,
                                      turbo_media_source_key_t *key,
                                      turbo_pipeline_error_t *error) {
    const char *first = strchr(node->config.url, '/');
    const char *second = first ? strchr(first + 1, '/') : NULL;
    char vhost[TURBO_MEDIA_MAX_VHOST_LEN];
    char app[TURBO_MEDIA_MAX_APP_LEN];
    size_t vhost_size;
    size_t app_size;
    if (!first || !second || first == node->config.url || second == first + 1 ||
        second[1] == '\0' || strchr(second + 1, '/')) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                           "Runtime URL must use vhost/app/stream syntax");
        return 0;
    }
    vhost_size = (size_t)(first - node->config.url);
    app_size = (size_t)(second - first - 1);
    if (vhost_size >= sizeof(vhost) || app_size >= sizeof(app)) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                           "Runtime URL component exceeds its size limit");
        return 0;
    }
    memcpy(vhost, node->config.url, vhost_size);
    vhost[vhost_size] = '\0';
    memcpy(app, first + 1, app_size);
    app[app_size] = '\0';
    if (turbo_media_source_key_init(key, vhost, app, second + 1) !=
        TURBO_MEDIA_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                           "Runtime URL contains an invalid source key");
        return 0;
    }
    return 1;
}

static void *pipeline_runtime_packet_alloc(void *parameter, int bytes) {
    pipeline_runtime_track_t *track = (pipeline_runtime_track_t *)parameter;
    if (!track || bytes <= 0 || (size_t)bytes > track->packet_capacity)
        return NULL;
    return track->packet_buffer;
}

static void pipeline_runtime_packet_free(void *parameter, void *packet) {
    (void)parameter;
    (void)packet;
}

static int pipeline_runtime_encoded_packet(void *parameter,
                                           const void *packet,
                                           int bytes,
                                           uint32_t timestamp,
                                           int flags) {
    pipeline_runtime_track_t *track = (pipeline_runtime_track_t *)parameter;
    turbo_pipeline_t *pipeline;
    turbo_media_frame_t frame;
    int rc;
    (void)flags;
    if (!track || !packet || bytes <= 0) return TURBO_MEDIA_ERR_INVALID;
    pipeline = track->pipeline;
    memset(&frame, 0, sizeof(frame));
    frame.track_id = track->output_track_id;
    frame.data = (const uint8_t *)packet;
    frame.size = (size_t)bytes;
    frame.pts = timestamp;
    frame.dts = timestamp;
    rc = turbo_media_source_publish(pipeline->runtime_rtp.output_source, &frame);
    if (rc != TURBO_MEDIA_OK) return rc;
    atomic_fetch_add_explicit(&pipeline->packets_written, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&pipeline->bytes_written,
                              (unsigned long long)frame.size,
                              memory_order_relaxed);
    return TURBO_MEDIA_OK;
}

static int pipeline_runtime_emit_access_unit(pipeline_runtime_track_t *track,
                                             const uint8_t *data,
                                             size_t size,
                                             uint32_t timestamp) {
    turbo_pipeline_t *pipeline = track->pipeline;
    AVPacket *packet;
    int rc;
    if (size == 0 || size > INT_MAX) return TURBO_MEDIA_ERR_INVALID;
    if (track->branch->copy) {
        atomic_fetch_add_explicit(&pipeline->frames_encoded, 1,
                                  memory_order_relaxed);
        rc = rtp_payload_encode_input(track->encoder, data, (int)size,
                                      timestamp);
        return rc == 0 ? TURBO_MEDIA_OK : TURBO_MEDIA_ERR_STATE;
    }
    packet = av_packet_alloc();
    if (!packet) {
        pipeline_error_set(track->processing_error, TURBO_PIPELINE_ENOMEM,
                           pipeline->nodes[track->branch->decoder_node].id,
                           "cannot allocate Runtime transcode packet");
        return TURBO_MEDIA_ERR_NOMEM;
    }
    rc = av_new_packet(packet, (int)size);
    if (rc < 0) {
        pipeline_ffmpeg_error(track->processing_error,
                              pipeline->nodes[track->branch->decoder_node].id,
                              "allocate Runtime transcode payload", rc);
        av_packet_free(&packet);
        return TURBO_MEDIA_ERR_NOMEM;
    }
    memcpy(packet->data, data, size);
    packet->pts = timestamp;
    packet->dts = timestamp;
    rc = pipeline_transcode_packet(pipeline, track->branch, packet,
                                   track->processing_error);
    av_packet_free(&packet);
    return rc ? TURBO_MEDIA_OK : TURBO_MEDIA_ERR_STATE;
}

static int pipeline_runtime_decoded_payload(void *parameter,
                                            const void *packet,
                                            int bytes,
                                            uint32_t timestamp,
                                            int flags) {
    static const uint8_t start_code[PIPELINE_H264_START_CODE_SIZE] =
        {0x00, 0x00, 0x00, 0x01};
    pipeline_runtime_track_t *track = (pipeline_runtime_track_t *)parameter;
    turbo_pipeline_t *pipeline;
    if (!track || !packet || bytes <= 0 || flags != 0)
        return TURBO_MEDIA_ERR_INVALID;
    pipeline = track->pipeline;
    if (track->branch->copy)
        atomic_fetch_add_explicit(&pipeline->frames_decoded, 1,
                                  memory_order_relaxed);
    if (!track->is_h264)
        return pipeline_runtime_emit_access_unit(
            track, (const uint8_t *)packet, (size_t)bytes, timestamp);
    if (track->input_timestamp_valid && track->input_timestamp != timestamp &&
        track->access_unit_size != 0)
        return TURBO_MEDIA_ERR_STATE;
    track->input_timestamp = timestamp;
    track->input_timestamp_valid = 1;
    if (track->access_unit_capacity < PIPELINE_H264_START_CODE_SIZE ||
        track->access_unit_size >
            track->access_unit_capacity - PIPELINE_H264_START_CODE_SIZE ||
        (size_t)bytes >
            track->access_unit_capacity - track->access_unit_size -
                PIPELINE_H264_START_CODE_SIZE)
        return TURBO_MEDIA_ERR_FULL;
    memcpy(track->access_unit + track->access_unit_size, start_code,
           sizeof(start_code));
    track->access_unit_size += sizeof(start_code);
    memcpy(track->access_unit + track->access_unit_size, packet, (size_t)bytes);
    track->access_unit_size += (size_t)bytes;
    return TURBO_MEDIA_OK;
}

static pipeline_runtime_track_t *pipeline_runtime_find_track(
    pipeline_runtime_rtp_t *runtime, int input_track_id) {
    size_t i;
    for (i = 0; i < PIPELINE_RUNTIME_TRACK_COUNT; ++i) {
        if (runtime->tracks[i].configured &&
            runtime->tracks[i].input_track_id == input_track_id)
            return &runtime->tracks[i];
    }
    return NULL;
}

static int pipeline_runtime_process_packet(turbo_pipeline_t *pipeline,
                                           const turbo_media_frame_t *frame,
                                           turbo_pipeline_error_t *error) {
    pipeline_runtime_track_t *track =
        pipeline_runtime_find_track(&pipeline->runtime_rtp, frame->track_id);
    struct rtp_packet_t parsed;
    struct rtp_payload_t handler;
    int rc;
    if (!track || frame->size > INT_MAX ||
        rtp_packet_deserialize(&parsed, frame->data, (int)frame->size) != 0 ||
        (int)parsed.rtp.pt != track->input_info.payload_type)
        return TURBO_MEDIA_ERR_INVALID;
    if (!track->encoder) {
        memset(&handler, 0, sizeof(handler));
        handler.alloc = pipeline_runtime_packet_alloc;
        handler.free = pipeline_runtime_packet_free;
        handler.packet = pipeline_runtime_encoded_packet;
        track->encoder = rtp_payload_encode_create(
            track->output_info.payload_type, track->output_info.codec_name,
            (uint16_t)parsed.rtp.seq, parsed.rtp.ssrc, &handler, track);
        if (!track->encoder) return TURBO_MEDIA_ERR_NOMEM;
    }
    track->processing_error = error;
    rc = rtp_payload_decode_input(track->decoder, frame->data, (int)frame->size);
    if (rc < 0) {
        track->access_unit_size = 0;
        track->input_timestamp_valid = 0;
        track->processing_error = NULL;
        return TURBO_MEDIA_ERR_STATE;
    }
    if (track->is_h264 && parsed.rtp.m) {
        rc = pipeline_runtime_emit_access_unit(
            track, track->access_unit, track->access_unit_size,
            parsed.rtp.timestamp);
        track->access_unit_size = 0;
        track->input_timestamp_valid = 0;
        if (rc != TURBO_MEDIA_OK) {
            track->processing_error = NULL;
            return rc;
        }
    }
    track->processing_error = NULL;
    return TURBO_MEDIA_OK;
}

static int pipeline_runtime_on_frame(turbo_media_source_t *source,
                                     const turbo_media_frame_t *frame,
                                     void *user_data) {
    turbo_pipeline_t *pipeline = (turbo_pipeline_t *)user_data;
    pipeline_runtime_rtp_t *runtime;
    pipeline_runtime_queue_entry_t *entry;
    uint8_t *slot;
    (void)source;
    if (!pipeline || !frame || (frame->size > 0 && !frame->data))
        return TURBO_MEDIA_ERR_INVALID;
    runtime = &pipeline->runtime_rtp;
    if (!pipeline_runtime_find_track(runtime, frame->track_id))
        return TURBO_MEDIA_OK;
    turbo_mutex_lock(&runtime->mutex);
    if (!runtime->accepting ||
        atomic_load_explicit(&pipeline->stop_requested,
                             memory_order_acquire)) {
        turbo_mutex_unlock(&runtime->mutex);
        return TURBO_MEDIA_ERR_STATE;
    }
    if (frame->size == 0 || frame->size > runtime->max_packet_bytes ||
        runtime->count == runtime->queue_capacity) {
        atomic_store_explicit(&runtime->async_status,
                              TURBO_PIPELINE_EBACKPRESSURE,
                              memory_order_release);
        turbo_cond_signal(&runtime->available);
        turbo_mutex_unlock(&runtime->mutex);
        return TURBO_MEDIA_ERR_FULL;
    }
    entry = &runtime->entries[runtime->tail];
    slot = runtime->packet_slots + runtime->tail * runtime->max_packet_bytes;
    memcpy(slot, frame->data, frame->size);
    entry->frame = *frame;
    entry->frame.data = slot;
    entry->slot = runtime->tail;
    runtime->tail = (runtime->tail + 1u) % runtime->queue_capacity;
    runtime->count++;
    turbo_cond_signal(&runtime->available);
    turbo_mutex_unlock(&runtime->mutex);
    return TURBO_MEDIA_OK;
}

static int pipeline_runtime_track_prepare(turbo_pipeline_t *pipeline,
                                          pipeline_runtime_track_t *track,
                                          pipeline_branch_t *branch,
                                          const turbo_media_track_info_t *info,
                                          size_t access_unit_capacity,
                                          turbo_pipeline_error_t *error) {
    struct rtp_payload_t handler;
    turbo_media_track_info_t output_info;
    int output_track_id;
    int is_h264 = av_strcasecmp(info->codec_name, "H264") == 0;
    int is_opus = av_strcasecmp(info->codec_name, "opus") == 0;
    if (!is_h264 && !is_opus) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG,
                           pipeline->nodes[pipeline->demux_node].id,
                           "RTP codec '%s' is unsupported; v1 supports H264 and opus",
                           info->codec_name);
        return 0;
    }
    if (info->payload_type < 0 || info->payload_type > 127 ||
        info->clock_rate <= 0) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG,
                           pipeline->nodes[pipeline->demux_node].id,
                           "track %d has invalid RTP payload metadata",
                           info->track_id);
        return 0;
    }
    memset(track, 0, sizeof(*track));
    track->pipeline = pipeline;
    track->branch = branch;
    track->is_h264 = is_h264;
    track->input_track_id = info->track_id;
    track->input_info = *info;
    output_info = *info;
    if (!branch->copy &&
        !pipeline_prepare_runtime_branch(pipeline, branch, track, info,
                                         &output_info, error))
        return 0;
    track->output_info = output_info;
    track->packet_capacity =
        pipeline->runtime_rtp.max_packet_bytes > (size_t)rtp_packet_getsize()
            ? pipeline->runtime_rtp.max_packet_bytes
            : (size_t)rtp_packet_getsize();
    track->packet_buffer = (uint8_t *)malloc(track->packet_capacity);
    if (is_h264) {
        track->access_unit_capacity = access_unit_capacity;
        track->access_unit = (uint8_t *)malloc(access_unit_capacity);
    }
    if (!track->packet_buffer || (is_h264 && !track->access_unit)) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM,
                           pipeline->nodes[pipeline->demux_node].id,
                           "cannot allocate RTP track buffers");
        return 0;
    }
    memset(&handler, 0, sizeof(handler));
    handler.alloc = pipeline_runtime_packet_alloc;
    handler.free = pipeline_runtime_packet_free;
    handler.packet = pipeline_runtime_decoded_payload;
    track->decoder = rtp_payload_decode_create(
        info->payload_type, info->codec_name, &handler, track);
    if (!track->decoder) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG,
                           pipeline->nodes[pipeline->demux_node].id,
                           "cannot create RTP depacketizer for codec '%s'",
                           info->codec_name);
        return 0;
    }
    if (turbo_media_source_add_track(pipeline->runtime_rtp.output_source,
                                     &track->output_info,
                                     &output_track_id) != TURBO_MEDIA_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ESTATE,
                           pipeline->nodes[pipeline->sink_node].id,
                           "cannot register output track %d", info->track_id);
        return 0;
    }
    track->output_track_id = output_track_id;
    track->configured = 1;
    return 1;
}

static int pipeline_runtime_prepare(turbo_pipeline_t *pipeline,
                                    turbo_pipeline_error_t *error) {
    static const char *const source_options[] = {
        "queue_capacity", "max_packet_bytes", "max_access_unit_bytes",
        "replay_cached"};
    pipeline_runtime_rtp_t *runtime = &pipeline->runtime_rtp;
    const pipeline_node_t *source_node = &pipeline->nodes[pipeline->source_node];
    const pipeline_node_t *demux_node = &pipeline->nodes[pipeline->demux_node];
    const pipeline_node_t *mux_node = &pipeline->nodes[pipeline->mux_node];
    const pipeline_node_t *sink_node = &pipeline->nodes[pipeline->sink_node];
    turbo_media_source_t *existing_output = NULL;
    size_t access_unit_capacity;
    size_t track_count;
    size_t i;
    int audio_found = 0;
    int video_found = 0;
    int rc;
    if (!runtime->server_runtime) {
        pipeline_error_set(error, TURBO_PIPELINE_ESTATE, source_node->id,
                           "bind ServerRuntime before preparing this graph");
        return 0;
    }
    if (!pipeline_runtime_options_valid(
            source_node, source_options,
            sizeof(source_options) / sizeof(source_options[0]), error) ||
        !pipeline_runtime_options_valid(demux_node, NULL, 0, error) ||
        !pipeline_runtime_options_valid(mux_node, NULL, 0, error) ||
        !pipeline_runtime_options_valid(sink_node, NULL, 0, error) ||
        !pipeline_runtime_size_option(
            source_node, "queue_capacity",
            PIPELINE_RUNTIME_DEFAULT_QUEUE_CAPACITY,
            PIPELINE_RUNTIME_MAX_QUEUE_CAPACITY, &runtime->queue_capacity,
            error) ||
        !pipeline_runtime_size_option(
            source_node, "max_packet_bytes",
            PIPELINE_RUNTIME_DEFAULT_PACKET_BYTES,
            PIPELINE_RUNTIME_MAX_PACKET_BYTES, &runtime->max_packet_bytes,
            error) ||
        !pipeline_runtime_size_option(
            source_node, "max_access_unit_bytes",
            PIPELINE_RUNTIME_DEFAULT_ACCESS_UNIT_BYTES,
            PIPELINE_RUNTIME_MAX_ACCESS_UNIT_BYTES, &access_unit_capacity,
            error) ||
        !pipeline_runtime_bool_option(source_node, "replay_cached", 0,
                                      &runtime->replay_cached, error) ||
        !pipeline_runtime_parse_key(source_node, &runtime->input_key, error) ||
        !pipeline_runtime_parse_key(sink_node, &runtime->output_key, error))
        return 0;
    if (runtime->max_packet_bytes <= RTP_FIXED_HEADER ||
        access_unit_capacity <= PIPELINE_H264_START_CODE_SIZE) {
        pipeline_error_set(
            error, TURBO_PIPELINE_ECONFIG, source_node->id,
            "max_packet_bytes must exceed %u and max_access_unit_bytes must "
            "exceed %u",
            RTP_FIXED_HEADER, PIPELINE_H264_START_CODE_SIZE);
        return 0;
    }
    if (turbo_media_source_key_equal(&runtime->input_key,
                                     &runtime->output_key)) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, sink_node->id,
                           "Runtime input and output source keys must differ");
        return 0;
    }
    rc = turbo_media_server_runtime_find_source(
        runtime->server_runtime, &runtime->input_key, &runtime->input_source);
    if (rc != TURBO_MEDIA_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ESTATE, source_node->id,
                           "Runtime input source does not exist");
        return 0;
    }
    rc = turbo_media_server_runtime_find_source(
        runtime->server_runtime, &runtime->output_key, &existing_output);
    if (rc == TURBO_MEDIA_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ESTATE, sink_node->id,
                           "Runtime output source already exists");
        return 0;
    }
    if (rc != TURBO_MEDIA_OK && rc != TURBO_MEDIA_ERR_NOT_FOUND) {
        pipeline_error_set(error, TURBO_PIPELINE_ESTATE, sink_node->id,
                           "cannot inspect Runtime output source");
        return 0;
    }
    rc = turbo_media_server_runtime_get_or_create_source(
        runtime->server_runtime, &runtime->output_key, &runtime->output_source);
    if (rc != TURBO_MEDIA_OK) {
        pipeline_error_set(error, TURBO_PIPELINE_ESTATE, sink_node->id,
                           "cannot create Runtime output source");
        return 0;
    }
    runtime->output_source_created = 1;
    runtime->entries = (pipeline_runtime_queue_entry_t *)calloc(
        runtime->queue_capacity, sizeof(*runtime->entries));
    if (runtime->queue_capacity > SIZE_MAX / runtime->max_packet_bytes) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, source_node->id,
                           "Runtime queue allocation overflows size_t");
        return 0;
    }
    runtime->packet_slots =
        (uint8_t *)malloc(runtime->queue_capacity * runtime->max_packet_bytes);
    if (!runtime->entries || !runtime->packet_slots) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, source_node->id,
                           "cannot allocate bounded Runtime input queue");
        return 0;
    }
    turbo_mutex_init(&runtime->mutex);
    turbo_cond_init(&runtime->available);
    runtime->sync_initialized = 1;
    atomic_init(&runtime->async_status, TURBO_PIPELINE_OK);
    track_count = turbo_media_source_track_count(runtime->input_source);
    for (i = 0; i < track_count; ++i) {
        turbo_media_track_info_t info;
        pipeline_runtime_track_t *track;
        if (turbo_media_source_get_track_at(runtime->input_source, i, &info) !=
            TURBO_MEDIA_OK) {
            pipeline_error_set(error, TURBO_PIPELINE_ESTATE, source_node->id,
                               "cannot read Runtime input track %zu", i);
            return 0;
        }
        if (info.type == TURBO_MEDIA_TRACK_AUDIO && pipeline->audio.configured) {
            if (audio_found) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, source_node->id,
                                   "Runtime/RTP v1 supports one audio track");
                return 0;
            }
            track = &runtime->tracks[0];
            audio_found = 1;
        } else if (info.type == TURBO_MEDIA_TRACK_VIDEO &&
                   pipeline->video.configured) {
            if (video_found) {
                pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, source_node->id,
                                   "Runtime/RTP v1 supports one video track");
                return 0;
            }
            track = &runtime->tracks[1];
            video_found = 1;
        } else {
            continue;
        }
        if (!pipeline_runtime_track_prepare(
                pipeline, track,
                info.type == TURBO_MEDIA_TRACK_AUDIO ? &pipeline->audio
                                                     : &pipeline->video,
                &info, access_unit_capacity, error))
            return 0;
    }
    if ((pipeline->audio.configured && !audio_found) ||
        (pipeline->video.configured && !video_found)) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, source_node->id,
                           "Runtime input tracks do not match graph branches");
        return 0;
    }
    runtime->accepting = 1;
    rc = turbo_media_source_subscribe(
        runtime->input_source, pipeline_runtime_on_frame, pipeline,
        runtime->replay_cached, &runtime->subscription_id);
    if (rc != TURBO_MEDIA_OK) {
        runtime->accepting = 0;
        pipeline_error_set(error, TURBO_PIPELINE_ESTATE, source_node->id,
                           "cannot subscribe to Runtime input source");
        return 0;
    }
    runtime->output_source_committed = 1;
    return 1;
}

static void pipeline_runtime_release(turbo_pipeline_t *pipeline) {
    pipeline_runtime_rtp_t *runtime;
    turbo_media_server_runtime_t *server_runtime;
    size_t i;
    if (!pipeline) return;
    runtime = &pipeline->runtime_rtp;
    server_runtime = runtime->server_runtime;
    if (runtime->sync_initialized) {
        turbo_mutex_lock(&runtime->mutex);
        runtime->accepting = 0;
        turbo_cond_broadcast(&runtime->available);
        turbo_mutex_unlock(&runtime->mutex);
    }
    if (runtime->subscription_id && runtime->input_source) {
        (void)turbo_media_source_unsubscribe(runtime->input_source,
                                             runtime->subscription_id);
        runtime->subscription_id = 0;
    }
    for (i = 0; i < PIPELINE_RUNTIME_TRACK_COUNT; ++i) {
        pipeline_runtime_track_t *track = &runtime->tracks[i];
        if (track->decoder) rtp_payload_decode_destroy(track->decoder);
        if (track->encoder) rtp_payload_encode_destroy(track->encoder);
        free(track->packet_buffer);
        free(track->access_unit);
    }
    if (runtime->output_source_created &&
        !runtime->output_source_committed && server_runtime) {
        (void)turbo_media_server_runtime_remove_source(
            server_runtime, &runtime->output_key);
    }
    if (runtime->sync_initialized) {
        turbo_cond_destroy(&runtime->available);
        turbo_mutex_destroy(&runtime->mutex);
    }
    free(runtime->entries);
    free(runtime->packet_slots);
    memset(runtime, 0, sizeof(*runtime));
    runtime->server_runtime = server_runtime;
}

static void pipeline_branch_release(pipeline_branch_t *branch) {
    if (!branch) return;
    av_packet_free(&branch->encoded_packet);
    av_frame_free(&branch->filtered_frame);
    av_frame_free(&branch->decoded_frame);
    avfilter_graph_free(&branch->filter_graph);
    branch->filter_source = NULL;
    branch->filter_sink = NULL;
    avcodec_free_context(&branch->encoder);
    avcodec_free_context(&branch->decoder);
    branch->input_stream = NULL;
    branch->output_stream = NULL;
    branch->input_stream_index = -1;
}

static void pipeline_release_runtime(turbo_pipeline_t *pipeline) {
    if (!pipeline) return;
    if (pipeline->execution_mode == PIPELINE_EXECUTION_RUNTIME_RTP) {
        pipeline_runtime_release(pipeline);
        pipeline_branch_release(&pipeline->audio);
        pipeline_branch_release(&pipeline->video);
        return;
    }
    pipeline_clear_deadline(pipeline);
    av_dict_free(&pipeline->output_options);
    av_packet_free(&pipeline->input_packet);
    pipeline_branch_release(&pipeline->audio);
    pipeline_branch_release(&pipeline->video);
    if (pipeline->output) {
        if (pipeline->output->pb &&
            !(pipeline->output->oformat->flags & AVFMT_NOFILE))
            avio_closep(&pipeline->output->pb);
        avformat_free_context(pipeline->output);
        pipeline->output = NULL;
    }
    if (pipeline->input) avformat_close_input(&pipeline->input);
    pipeline->header_written = 0;
}

static int pipeline_dictionary_add(AVDictionary **dictionary, const pipeline_node_t *node,
                                   turbo_pipeline_error_t *error) {
    size_t i;
    for (i = 0; i < node->config.option_count; ++i) {
        if (av_dict_get(*dictionary, node->config.options[i].key, NULL, 0)) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                               "FFmpeg option '%s' is specified more than once",
                               node->config.options[i].key);
            return 0;
        }
        if (av_dict_set(dictionary, node->config.options[i].key,
                        node->config.options[i].value, 0) < 0) {
            pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, node->id,
                               "cannot allocate FFmpeg options");
            return 0;
        }
    }
    return 1;
}

static turbo_pipeline_status_t pipeline_reject_unused_option(
    const AVDictionary *dictionary, const char *node_id, const char *stage,
    turbo_pipeline_error_t *error) {
    const AVDictionaryEntry *entry = av_dict_iterate(dictionary, NULL);
    if (!entry) return TURBO_PIPELINE_OK;
    return pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node_id,
                              "FFmpeg option '%s' was not consumed during %s", entry->key,
                              stage);
}

static int pipeline_supported_config(const AVCodec *codec, enum AVCodecConfig config,
                                     const void **values, int *count) {
    int result = avcodec_get_supported_config(NULL, codec, config, 0, values, count);
    return result >= 0;
}

static enum AVPixelFormat pipeline_choose_pixel_format(const AVCodec *codec,
                                                       enum AVPixelFormat preferred) {
    const enum AVPixelFormat *formats = NULL;
    int count = 0;
    int i;
    if (!pipeline_supported_config(codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                   (const void **)&formats, &count) ||
        !formats || count == 0)
        return preferred != AV_PIX_FMT_NONE ? preferred : AV_PIX_FMT_YUV420P;
    for (i = 0; i < count; ++i)
        if (formats[i] == preferred) return preferred;
    return formats[0];
}

static enum AVSampleFormat pipeline_choose_sample_format(const AVCodec *codec,
                                                         enum AVSampleFormat preferred) {
    const enum AVSampleFormat *formats = NULL;
    int count = 0;
    int i;
    if (!pipeline_supported_config(codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                   (const void **)&formats, &count) ||
        !formats || count == 0)
        return preferred != AV_SAMPLE_FMT_NONE ? preferred : AV_SAMPLE_FMT_FLTP;
    for (i = 0; i < count; ++i)
        if (formats[i] == preferred) return preferred;
    return formats[0];
}

static int pipeline_choose_sample_rate(const AVCodec *codec, int preferred) {
    const int *rates = NULL;
    int count = 0;
    int i;
    if (!pipeline_supported_config(codec, AV_CODEC_CONFIG_SAMPLE_RATE,
                                   (const void **)&rates, &count) ||
        !rates || count == 0)
        return preferred > 0 ? preferred : 48000;
    for (i = 0; i < count; ++i)
        if (rates[i] == preferred) return preferred;
    return rates[0];
}

static int pipeline_choose_channel_layout(const AVCodec *codec,
                                          const AVChannelLayout *preferred,
                                          AVChannelLayout *chosen) {
    const AVChannelLayout *layouts = NULL;
    int count = 0;
    int i;
    if (!pipeline_supported_config(codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT,
                                   (const void **)&layouts, &count) ||
        !layouts || count == 0)
        return av_channel_layout_copy(chosen, preferred) >= 0;
    for (i = 0; i < count; ++i) {
        if (av_channel_layout_compare(&layouts[i], preferred) == 0)
            return av_channel_layout_copy(chosen, preferred) >= 0;
    }
    return av_channel_layout_copy(chosen, &layouts[0]) >= 0;
}

static int pipeline_open_decoder(turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
                                 turbo_pipeline_error_t *error) {
    const AVCodec *decoder;
    const pipeline_node_t *node = &pipeline->nodes[branch->decoder_node];
    AVDictionary *options = NULL;
    int result;
    decoder = avcodec_find_decoder(branch->input_stream->codecpar->codec_id);
    if (!decoder) {
        pipeline_error_set(error, TURBO_PIPELINE_EFFMPEG, node->id,
                           "no FFmpeg decoder for input codec");
        return 0;
    }
    branch->decoder = avcodec_alloc_context3(decoder);
    if (!branch->decoder) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, node->id,
                           "cannot allocate decoder context");
        return 0;
    }
    result = avcodec_parameters_to_context(branch->decoder,
                                           branch->input_stream->codecpar);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node->id, "copy decoder parameters", result);
        return 0;
    }
    branch->decoder->pkt_timebase = branch->input_stream->time_base;
    if (!pipeline_dictionary_add(&options, node, error)) {
        av_dict_free(&options);
        return 0;
    }
    result = avcodec_open2(branch->decoder, decoder, &options);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node->id, "open decoder", result);
        av_dict_free(&options);
        return 0;
    }
    if (pipeline_reject_unused_option(options, node->id, "decoder open", error) !=
        TURBO_PIPELINE_OK) {
        av_dict_free(&options);
        return 0;
    }
    av_dict_free(&options);
    return 1;
}

static int pipeline_open_runtime_decoder(
    turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
    const turbo_media_track_info_t *info, turbo_pipeline_error_t *error) {
    const pipeline_node_t *node = &pipeline->nodes[branch->decoder_node];
    enum AVCodecID codec_id =
        branch->media_type == AVMEDIA_TYPE_VIDEO ? AV_CODEC_ID_H264
                                                 : AV_CODEC_ID_OPUS;
    const AVCodec *decoder = avcodec_find_decoder(codec_id);
    AVDictionary *options = NULL;
    int result;
    if (!decoder || decoder->type != branch->media_type) {
        pipeline_error_set(error, TURBO_PIPELINE_EFFMPEG, node->id,
                           "no FFmpeg decoder for Runtime RTP codec '%s'",
                           info->codec_name);
        return 0;
    }
    branch->decoder = avcodec_alloc_context3(decoder);
    if (!branch->decoder) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, node->id,
                           "cannot allocate Runtime decoder context");
        return 0;
    }
    branch->input_time_base = (AVRational){1, info->clock_rate};
    branch->decoder->pkt_timebase = branch->input_time_base;
    branch->decoder->time_base = branch->input_time_base;
    if (branch->media_type == AVMEDIA_TYPE_AUDIO) {
        int channels = info->channels > 0 ? info->channels : 2;
        branch->decoder->sample_rate =
            info->sample_rate > 0 ? info->sample_rate : info->clock_rate;
        av_channel_layout_default(&branch->decoder->ch_layout, channels);
    }
    if (!pipeline_dictionary_add(&options, node, error)) {
        av_dict_free(&options);
        return 0;
    }
    result = avcodec_open2(branch->decoder, decoder, &options);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node->id, "open Runtime decoder", result);
        av_dict_free(&options);
        return 0;
    }
    if (pipeline_reject_unused_option(options, node->id,
                                      "Runtime decoder open", error) !=
        TURBO_PIPELINE_OK) {
        av_dict_free(&options);
        return 0;
    }
    av_dict_free(&options);
    return 1;
}

static int pipeline_configure_video_encoder(turbo_pipeline_t *pipeline,
                                            pipeline_branch_t *branch,
                                            const AVCodec *encoder,
                                            const AVFrame *first_frame,
                                            turbo_pipeline_error_t *error) {
    const pipeline_node_config_t *config =
        &pipeline->nodes[branch->encoder_node].config;
    AVRational frame_rate = {0, 1};
    int input_width = first_frame ? first_frame->width : branch->decoder->width;
    int input_height = first_frame ? first_frame->height : branch->decoder->height;
    enum AVPixelFormat input_format =
        first_frame ? (enum AVPixelFormat)first_frame->format
                    : branch->decoder->pix_fmt;
    AVRational input_aspect =
        first_frame ? first_frame->sample_aspect_ratio
                    : branch->decoder->sample_aspect_ratio;
    if (pipeline->input && branch->input_stream)
        frame_rate =
            av_guess_frame_rate(pipeline->input, branch->input_stream, NULL);
    if (config->frame_rate > 0) frame_rate = (AVRational){config->frame_rate, 1};
    if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = (AVRational){25, 1};
    branch->encoder->width = config->width > 0 ? config->width : input_width;
    branch->encoder->height = config->height > 0 ? config->height : input_height;
    if (branch->encoder->width <= 0 || branch->encoder->height <= 0) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG,
                           pipeline->nodes[branch->encoder_node].id,
                           "video width and height are unavailable");
        return 0;
    }
    branch->encoder->framerate = frame_rate;
    branch->encoder->time_base = av_inv_q(frame_rate);
    branch->encoder->pix_fmt =
        pipeline_choose_pixel_format(encoder, input_format);
    branch->encoder->bit_rate = config->bitrate;
    branch->encoder->gop_size = config->gop_frames;
    branch->encoder->sample_aspect_ratio = input_aspect;
    return 1;
}

static int pipeline_configure_audio_encoder(turbo_pipeline_t *pipeline,
                                            pipeline_branch_t *branch,
                                            const AVCodec *encoder,
                                            const AVFrame *first_frame,
                                            turbo_pipeline_error_t *error) {
    const pipeline_node_config_t *config =
        &pipeline->nodes[branch->encoder_node].config;
    AVChannelLayout preferred = {0};
    const AVChannelLayout *input_layout =
        first_frame && first_frame->ch_layout.nb_channels > 0
            ? &first_frame->ch_layout
            : &branch->decoder->ch_layout;
    int input_sample_rate =
        first_frame && first_frame->sample_rate > 0
            ? first_frame->sample_rate
            : branch->decoder->sample_rate;
    enum AVSampleFormat input_sample_format =
        first_frame ? (enum AVSampleFormat)first_frame->format
                    : branch->decoder->sample_fmt;
    int channels =
        config->channels > 0 ? config->channels : input_layout->nb_channels;
    if (channels <= 0) channels = 2;
    if (config->channels > 0 ||
        input_layout->nb_channels <= 0) {
        av_channel_layout_default(&preferred, channels);
    } else if (av_channel_layout_copy(&preferred, input_layout) < 0) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM,
                           pipeline->nodes[branch->encoder_node].id,
                           "cannot copy audio channel layout");
        return 0;
    }
    branch->encoder->sample_rate = pipeline_choose_sample_rate(
        encoder,
        config->sample_rate > 0 ? config->sample_rate : input_sample_rate);
    branch->encoder->sample_fmt =
        pipeline_choose_sample_format(encoder, input_sample_format);
    if (!pipeline_choose_channel_layout(encoder, &preferred,
                                        &branch->encoder->ch_layout)) {
        av_channel_layout_uninit(&preferred);
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM,
                           pipeline->nodes[branch->encoder_node].id,
                           "cannot select audio channel layout");
        return 0;
    }
    av_channel_layout_uninit(&preferred);
    branch->encoder->time_base = (AVRational){1, branch->encoder->sample_rate};
    branch->encoder->bit_rate = config->bitrate;
    return 1;
}

static int pipeline_open_encoder(turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
                                 const AVFrame *first_frame,
                                 turbo_pipeline_error_t *error) {
    const pipeline_node_t *node = &pipeline->nodes[branch->encoder_node];
    const AVCodec *encoder = avcodec_find_encoder_by_name(node->config.codec);
    AVDictionary *options = NULL;
    int result;
    if (!encoder || encoder->type != branch->media_type) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node->id,
                           "FFmpeg encoder '%s' is unavailable or has the wrong media type",
                           node->config.codec);
        return 0;
    }
    branch->encoder = avcodec_alloc_context3(encoder);
    if (!branch->encoder) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, node->id,
                           "cannot allocate encoder context");
        return 0;
    }
    if (branch->media_type == AVMEDIA_TYPE_VIDEO) {
        if (!pipeline_configure_video_encoder(pipeline, branch, encoder,
                                              first_frame, error))
            return 0;
    } else if (!pipeline_configure_audio_encoder(pipeline, branch, encoder,
                                                 first_frame, error)) {
        return 0;
    }
    if (pipeline->output &&
        (pipeline->output->oformat->flags & AVFMT_GLOBALHEADER))
        branch->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (!pipeline_dictionary_add(&options, node, error)) {
        av_dict_free(&options);
        return 0;
    }
    result = avcodec_open2(branch->encoder, encoder, &options);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node->id, "open encoder", result);
        av_dict_free(&options);
        return 0;
    }
    if (pipeline_reject_unused_option(options, node->id, "encoder open", error) !=
        TURBO_PIPELINE_OK) {
        av_dict_free(&options);
        return 0;
    }
    av_dict_free(&options);
    return 1;
}

static int pipeline_prepare_runtime_branch(
    turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
    pipeline_runtime_track_t *track, const turbo_media_track_info_t *input_info,
    turbo_media_track_info_t *output_info, turbo_pipeline_error_t *error) {
    const pipeline_node_t *encoder_node =
        &pipeline->nodes[branch->encoder_node];
    const AVCodec *encoder =
        avcodec_find_encoder_by_name(encoder_node->config.codec);
    enum AVCodecID required_codec =
        branch->media_type == AVMEDIA_TYPE_VIDEO ? AV_CODEC_ID_H264
                                                 : AV_CODEC_ID_OPUS;
    const char *rtp_codec =
        branch->media_type == AVMEDIA_TYPE_VIDEO ? "H264" : "opus";
    int clock_rate =
        branch->media_type == AVMEDIA_TYPE_VIDEO ? 90000 : 48000;
    if (!encoder || encoder->type != branch->media_type) {
        pipeline_error_set(
            error, TURBO_PIPELINE_ECONFIG, encoder_node->id,
            "FFmpeg encoder '%s' is unavailable or has the wrong media type",
            encoder_node->config.codec);
        return 0;
    }
    if (encoder->id != required_codec) {
        pipeline_error_set(
            error, TURBO_PIPELINE_ECONFIG, encoder_node->id,
            "Runtime/RTP transcode output must use %s; encoder '%s' produces %s",
            rtp_codec, encoder_node->config.codec,
            avcodec_get_name(encoder->id));
        return 0;
    }
    branch->runtime_track = track;
    if (!pipeline_open_runtime_decoder(pipeline, branch, input_info, error))
        return 0;
    branch->decoded_frame = av_frame_alloc();
    branch->filtered_frame = av_frame_alloc();
    branch->encoded_packet = av_packet_alloc();
    if (!branch->decoded_frame || !branch->filtered_frame ||
        !branch->encoded_packet) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, encoder_node->id,
                           "cannot allocate Runtime transcode frames or packet");
        return 0;
    }
    *output_info = *input_info;
    memset(output_info->codec_name, 0, sizeof(output_info->codec_name));
    av_strlcpy(output_info->codec_name, rtp_codec,
               sizeof(output_info->codec_name));
    output_info->clock_rate = clock_rate;
    output_info->extradata = NULL;
    output_info->extradata_size = 0;
    if (branch->media_type == AVMEDIA_TYPE_AUDIO) {
        const pipeline_node_config_t *config = &encoder_node->config;
        output_info->sample_rate =
            config->sample_rate > 0 ? config->sample_rate : clock_rate;
        output_info->channels =
            config->channels > 0 ? config->channels
                                 : (input_info->channels > 0
                                        ? input_info->channels
                                        : 2);
    } else {
        const pipeline_node_config_t *config = &encoder_node->config;
        if (config->width > 0) output_info->width = config->width;
        if (config->height > 0) output_info->height = config->height;
        if (config->frame_rate > 0)
            output_info->framerate = config->frame_rate;
    }
    return 1;
}

static int pipeline_prepare_branch(turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
                                   turbo_pipeline_error_t *error) {
    const char *media = av_get_media_type_string(branch->media_type);
    int stream_index;
    int result;
    if (!branch->configured) return 1;
    stream_index = av_find_best_stream(pipeline->input, branch->media_type, -1, -1, NULL, 0);
    if (stream_index < 0) {
        pipeline_ffmpeg_error(error, pipeline->nodes[pipeline->demux_node].id,
                              media ? media : "find input stream", stream_index);
        return 0;
    }
    branch->input_stream_index = stream_index;
    branch->input_stream = pipeline->input->streams[stream_index];
    branch->input_time_base = branch->input_stream->time_base;
    branch->output_stream = avformat_new_stream(pipeline->output, NULL);
    if (!branch->output_stream) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM,
                           pipeline->nodes[pipeline->mux_node].id,
                           "cannot allocate %s output stream", media);
        return 0;
    }
    if (branch->copy) {
        result = avcodec_parameters_copy(branch->output_stream->codecpar,
                                         branch->input_stream->codecpar);
        if (result < 0) {
            pipeline_ffmpeg_error(error, pipeline->nodes[pipeline->mux_node].id,
                                  "copy stream parameters", result);
            return 0;
        }
        branch->output_stream->codecpar->codec_tag = 0;
        branch->output_stream->time_base = branch->input_stream->time_base;
        return 1;
    }
    if (!pipeline_open_decoder(pipeline, branch, error) ||
        !pipeline_open_encoder(pipeline, branch, NULL, error))
        return 0;
    result = avcodec_parameters_from_context(branch->output_stream->codecpar,
                                             branch->encoder);
    if (result < 0) {
        pipeline_ffmpeg_error(error, pipeline->nodes[branch->encoder_node].id,
                              "export encoder parameters", result);
        return 0;
    }
    branch->output_stream->time_base = branch->encoder->time_base;
    branch->decoded_frame = av_frame_alloc();
    branch->filtered_frame = av_frame_alloc();
    branch->encoded_packet = av_packet_alloc();
    if (!branch->decoded_frame || !branch->filtered_frame || !branch->encoded_packet) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM,
                           pipeline->nodes[branch->encoder_node].id,
                           "cannot allocate transcode frames or packet");
        return 0;
    }
    return 1;
}

static int pipeline_open_input(turbo_pipeline_t *pipeline,
                               turbo_pipeline_error_t *error) {
    const pipeline_node_t *source = &pipeline->nodes[pipeline->source_node];
    const pipeline_node_t *demux = &pipeline->nodes[pipeline->demux_node];
    const AVInputFormat *input_format = NULL;
    AVDictionary *options = NULL;
    int result;
    if (demux->config.format[0]) {
        input_format = av_find_input_format(demux->config.format);
        if (!input_format) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, demux->id,
                               "FFmpeg input format '%s' is unavailable",
                               demux->config.format);
            return 0;
        }
    }
    pipeline->input = avformat_alloc_context();
    if (!pipeline->input) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, source->id,
                           "cannot allocate input context");
        return 0;
    }
    pipeline->input->interrupt_callback.callback = pipeline_interrupt;
    pipeline->input->interrupt_callback.opaque = pipeline;
    if (!pipeline_dictionary_add(&options, source, error) ||
        !pipeline_dictionary_add(&options, demux, error)) {
        av_dict_free(&options);
        return 0;
    }
    pipeline_set_deadline(pipeline, pipeline->open_timeout_ms);
    result = avformat_open_input(&pipeline->input, source->config.url, input_format, &options);
    pipeline_clear_deadline(pipeline);
    if (result < 0) {
        pipeline_ffmpeg_error(error, source->id, "open input", result);
        av_dict_free(&options);
        return 0;
    }
    if (pipeline_reject_unused_option(options, source->id, "input open", error) !=
        TURBO_PIPELINE_OK) {
        av_dict_free(&options);
        return 0;
    }
    av_dict_free(&options);
    pipeline_set_deadline(pipeline, pipeline->open_timeout_ms);
    result = avformat_find_stream_info(pipeline->input, NULL);
    pipeline_clear_deadline(pipeline);
    if (result < 0) {
        pipeline_ffmpeg_error(error, demux->id, "read stream information", result);
        return 0;
    }
    return 1;
}

static int pipeline_prepare_output(turbo_pipeline_t *pipeline,
                                   turbo_pipeline_error_t *error) {
    const pipeline_node_t *mux = &pipeline->nodes[pipeline->mux_node];
    const pipeline_node_t *sink = &pipeline->nodes[pipeline->sink_node];
    int result = avformat_alloc_output_context2(
        &pipeline->output, NULL, mux->config.format[0] ? mux->config.format : NULL,
        sink->config.url);
    if (result < 0 || !pipeline->output) {
        pipeline_ffmpeg_error(error, mux->id, "allocate output context",
                              result < 0 ? result : AVERROR(EINVAL));
        return 0;
    }
    /* Cross-container stream copy still needs codec framing adaptation. */
    pipeline->output->flags |= AVFMT_FLAG_AUTO_BSF;
    pipeline->output->interrupt_callback.callback = pipeline_interrupt;
    pipeline->output->interrupt_callback.opaque = pipeline;
    if (!pipeline_dictionary_add(&pipeline->output_options, sink, error) ||
        !pipeline_dictionary_add(&pipeline->output_options, mux, error))
        return 0;
    if (!pipeline_prepare_branch(pipeline, &pipeline->audio, error) ||
        !pipeline_prepare_branch(pipeline, &pipeline->video, error))
        return 0;
    if (!(pipeline->output->oformat->flags & AVFMT_NOFILE)) {
        pipeline_set_deadline(pipeline, pipeline->open_timeout_ms);
        result = avio_open2(&pipeline->output->pb, sink->config.url, AVIO_FLAG_WRITE,
                            &pipeline->output->interrupt_callback,
                            &pipeline->output_options);
        pipeline_clear_deadline(pipeline);
        if (result < 0) {
            pipeline_ffmpeg_error(error, sink->id, "open output", result);
            return 0;
        }
    }
    return 1;
}

turbo_pipeline_t *turbo_pipeline_create_from_yaml(const char *yaml, size_t yaml_size,
                                                   turbo_pipeline_error_t *error) {
    turbo_pipeline_t *pipeline;
    pipeline_error_clear(error);
    if (!yaml || yaml_size == 0) {
        pipeline_error_set(error, TURBO_PIPELINE_EINVAL, NULL,
                           "yaml and yaml_size are required");
        return NULL;
    }
    pipeline = (turbo_pipeline_t *)calloc(1, sizeof(*pipeline));
    if (!pipeline) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, NULL,
                           "cannot allocate pipeline");
        return NULL;
    }
    pipeline->source_node = -1;
    pipeline->demux_node = -1;
    pipeline->mux_node = -1;
    pipeline->sink_node = -1;
    pipeline->audio.input_stream_index = -1;
    pipeline->video.input_stream_index = -1;
    atomic_init(&pipeline->state, TURBO_PIPELINE_STATE_CREATED);
    atomic_init(&pipeline->stop_requested, 0);
    atomic_init(&pipeline->io_deadline_us, 0);
    atomic_init(&pipeline->packets_read, 0);
    atomic_init(&pipeline->packets_written, 0);
    atomic_init(&pipeline->frames_decoded, 0);
    atomic_init(&pipeline->frames_encoded, 0);
    atomic_init(&pipeline->bytes_read, 0);
    atomic_init(&pipeline->bytes_written, 0);
    if (!pipeline_validate_yaml_shape(yaml, yaml_size, error) ||
        !pipeline_read_yaml(pipeline, yaml, yaml_size, error) ||
        !pipeline_validate_graph(pipeline, error)) {
        free(pipeline);
        return NULL;
    }
    return pipeline;
}

turbo_pipeline_status_t turbo_pipeline_bind_server_runtime(
    turbo_pipeline_t *pipeline,
    turbo_media_server_runtime_t *runtime,
    turbo_pipeline_error_t *error) {
    pipeline_error_clear(error);
    if (!pipeline || !runtime)
        return pipeline_error_set(error, TURBO_PIPELINE_EINVAL, NULL,
                                  "pipeline and ServerRuntime are required");
    if (atomic_load_explicit(&pipeline->state, memory_order_acquire) !=
        TURBO_PIPELINE_STATE_CREATED)
        return pipeline_error_set(error, TURBO_PIPELINE_ESTATE, NULL,
                                  "ServerRuntime binding requires CREATED state");
    if (pipeline->execution_mode != PIPELINE_EXECUTION_RUNTIME_RTP)
        return pipeline_error_set(
            error, TURBO_PIPELINE_ESTATE, NULL,
            "ServerRuntime can only be bound to a Runtime/RTP graph");
    pipeline->runtime_rtp.server_runtime = runtime;
    return TURBO_PIPELINE_OK;
}

turbo_pipeline_status_t turbo_pipeline_prepare(turbo_pipeline_t *pipeline,
                                               turbo_pipeline_error_t *error) {
    pipeline_error_clear(error);
    if (!pipeline)
        return pipeline_error_set(error, TURBO_PIPELINE_EINVAL, NULL,
                                  "pipeline is required");
    if (atomic_load_explicit(&pipeline->state, memory_order_acquire) !=
        TURBO_PIPELINE_STATE_CREATED)
        return pipeline_error_set(error, TURBO_PIPELINE_ESTATE, NULL,
                                  "prepare requires CREATED state");
    atomic_store_explicit(&pipeline->stop_requested, 0, memory_order_release);
    if (pipeline->execution_mode == PIPELINE_EXECUTION_RUNTIME_RTP) {
        if (!pipeline_runtime_prepare(pipeline, error)) {
            pipeline_release_runtime(pipeline);
            atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_FAILED,
                                  memory_order_release);
            return error ? error->code : TURBO_PIPELINE_ESTATE;
        }
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_PREPARED,
                              memory_order_release);
        return TURBO_PIPELINE_OK;
    }
    if (avformat_network_init() < 0) {
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_FAILED,
                              memory_order_release);
        return pipeline_error_set(error, TURBO_PIPELINE_EFFMPEG, NULL,
                                  "FFmpeg network initialization failed");
    }
    if (!pipeline_open_input(pipeline, error) ||
        !pipeline_prepare_output(pipeline, error)) {
        pipeline_release_runtime(pipeline);
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_FAILED,
                              memory_order_release);
        return error ? error->code : TURBO_PIPELINE_EFFMPEG;
    }
    pipeline->input_packet = av_packet_alloc();
    if (!pipeline->input_packet) {
        pipeline_release_runtime(pipeline);
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_FAILED,
                              memory_order_release);
        return pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, NULL,
                                  "cannot allocate input packet");
    }
    atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_PREPARED,
                          memory_order_release);
    return TURBO_PIPELINE_OK;
}

static int pipeline_filter_init(turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
                                const AVFrame *frame, turbo_pipeline_error_t *error) {
    const AVFilter *source_filter;
    const AVFilter *sink_filter;
    const char *user_filters = NULL;
    const char *source_name;
    const char *sink_name;
    const char *node_id;
    char source_args[512];
    char filter_description[1536];
    char layout_name[128];
    AVFilterInOut *inputs = NULL;
    AVFilterInOut *outputs = NULL;
    AVRational time_base = branch->input_time_base;
    int result;

    if (branch->filter_graph) return 1;
    if (branch->filter_node >= 0) {
        user_filters = pipeline->nodes[branch->filter_node].config.filters;
        node_id = pipeline->nodes[branch->filter_node].id;
    } else {
        node_id = pipeline->nodes[branch->encoder_node].id;
    }
    branch->filter_graph = avfilter_graph_alloc();
    if (!branch->filter_graph) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, node_id,
                           "cannot allocate filter graph");
        return 0;
    }

    if (branch->media_type == AVMEDIA_TYPE_VIDEO) {
        const char *pixel_format = av_get_pix_fmt_name(branch->encoder->pix_fmt);
        AVRational aspect = frame->sample_aspect_ratio.num > 0
                                ? frame->sample_aspect_ratio
                                : (AVRational){1, 1};
        source_name = "buffer";
        sink_name = "buffersink";
        source_filter = avfilter_get_by_name(source_name);
        sink_filter = avfilter_get_by_name(sink_name);
        result = snprintf(source_args, sizeof(source_args),
                          "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:"
                          "pixel_aspect=%d/%d",
                          frame->width, frame->height, frame->format, time_base.num,
                          time_base.den, aspect.num, aspect.den);
        if (!pixel_format || result < 0 || (size_t)result >= sizeof(source_args)) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node_id,
                               "cannot describe video filter input");
            goto fail;
        }
        result = snprintf(filter_description, sizeof(filter_description), "%s%sformat=%s",
                          user_filters && user_filters[0] ? user_filters : "null",
                          user_filters && user_filters[0] ? "," : ",", pixel_format);
    } else {
        const AVChannelLayout *layout = frame->ch_layout.nb_channels > 0
                                            ? &frame->ch_layout
                                            : &branch->decoder->ch_layout;
        int sample_rate = frame->sample_rate > 0 ? frame->sample_rate
                                                 : branch->decoder->sample_rate;
        const char *input_sample_format =
            av_get_sample_fmt_name((enum AVSampleFormat)frame->format);
        const char *output_sample_format =
            av_get_sample_fmt_name(branch->encoder->sample_fmt);
        source_name = "abuffer";
        sink_name = "abuffersink";
        source_filter = avfilter_get_by_name(source_name);
        sink_filter = avfilter_get_by_name(sink_name);
        if (sample_rate <= 0 || !input_sample_format || !output_sample_format ||
            av_channel_layout_describe(layout, layout_name, sizeof(layout_name)) < 0) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node_id,
                               "cannot describe audio filter input");
            goto fail;
        }
        result = snprintf(source_args, sizeof(source_args),
                          "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:"
                          "channel_layout=%s",
                          time_base.num, time_base.den, sample_rate, input_sample_format,
                          layout_name);
        if (result < 0 || (size_t)result >= sizeof(source_args)) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node_id,
                               "audio filter input description is too long");
            goto fail;
        }
        if (av_channel_layout_describe(&branch->encoder->ch_layout, layout_name,
                                       sizeof(layout_name)) < 0) {
            pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node_id,
                               "cannot describe encoder channel layout");
            goto fail;
        }
        result = snprintf(
            filter_description, sizeof(filter_description),
            "%s,aformat=sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
            user_filters && user_filters[0] ? user_filters : "anull",
            output_sample_format, branch->encoder->sample_rate, layout_name);
    }
    if (result < 0 || (size_t)result >= sizeof(filter_description)) {
        pipeline_error_set(error, TURBO_PIPELINE_ECONFIG, node_id,
                           "filter description exceeds the runtime limit");
        goto fail;
    }
    if (!source_filter || !sink_filter) {
        pipeline_error_set(error, TURBO_PIPELINE_EFFMPEG, node_id,
                           "required FFmpeg buffer filters are unavailable");
        goto fail;
    }
    result = avfilter_graph_create_filter(&branch->filter_source, source_filter, "in",
                                          source_args, NULL, branch->filter_graph);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node_id, "create filter source", result);
        goto fail;
    }
    result = avfilter_graph_create_filter(&branch->filter_sink, sink_filter, "out", NULL,
                                          NULL, branch->filter_graph);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node_id, "create filter sink", result);
        goto fail;
    }

    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, node_id,
                           "cannot allocate filter endpoints");
        goto fail;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = branch->filter_source;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = branch->filter_sink;
    inputs->pad_idx = 0;
    if (!outputs->name || !inputs->name) {
        pipeline_error_set(error, TURBO_PIPELINE_ENOMEM, node_id,
                           "cannot allocate filter endpoint names");
        goto fail;
    }
    result = avfilter_graph_parse_ptr(branch->filter_graph, filter_description, &inputs,
                                      &outputs, NULL);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node_id, "parse filter chain", result);
        goto fail;
    }
    result = avfilter_graph_config(branch->filter_graph, NULL);
    if (result < 0) {
        pipeline_ffmpeg_error(error, node_id, "configure filter graph", result);
        goto fail;
    }
    branch->filter_time_base = av_buffersink_get_time_base(branch->filter_sink);
    if (branch->media_type == AVMEDIA_TYPE_AUDIO && branch->encoder->frame_size > 0)
        av_buffersink_set_frame_size(branch->filter_sink,
                                     (unsigned)branch->encoder->frame_size);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return 1;

fail:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&branch->filter_graph);
    branch->filter_source = NULL;
    branch->filter_sink = NULL;
    return 0;
}

static int pipeline_receive_encoded(turbo_pipeline_t *pipeline,
                                    pipeline_branch_t *branch,
                                    turbo_pipeline_error_t *error) {
    const char *node_id = pipeline->nodes[branch->encoder_node].id;
    for (;;) {
        int result = avcodec_receive_packet(branch->encoder, branch->encoded_packet);
        int packet_size;
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 1;
        if (result < 0) {
            pipeline_ffmpeg_error(error, node_id, "receive encoded packet", result);
            return 0;
        }
        if (pipeline->execution_mode == PIPELINE_EXECUTION_RUNTIME_RTP) {
            pipeline_runtime_track_t *track = branch->runtime_track;
            int64_t rtp_timestamp;
            packet_size = branch->encoded_packet->size;
            if (!track || !track->encoder ||
                branch->encoded_packet->pts == AV_NOPTS_VALUE) {
                av_packet_unref(branch->encoded_packet);
                pipeline_error_set(
                    error, TURBO_PIPELINE_ESTATE,
                    pipeline->nodes[pipeline->mux_node].id,
                    "Runtime encoded packet has no RTP track or presentation timestamp");
                return 0;
            }
            rtp_timestamp = av_rescale_q(
                branch->encoded_packet->pts, branch->encoder->time_base,
                (AVRational){1, track->output_info.clock_rate});
            result = rtp_payload_encode_input(
                track->encoder, branch->encoded_packet->data,
                branch->encoded_packet->size, (uint32_t)rtp_timestamp);
            av_packet_unref(branch->encoded_packet);
            if (result != 0) {
                pipeline_error_set(
                    error, TURBO_PIPELINE_EIO,
                    pipeline->nodes[pipeline->mux_node].id,
                    "RTP packetization failed for encoded %s packet (%d bytes)",
                    av_get_media_type_string(branch->media_type), packet_size);
                return 0;
            }
            continue;
        }
        av_packet_rescale_ts(branch->encoded_packet, branch->encoder->time_base,
                             branch->output_stream->time_base);
        branch->encoded_packet->stream_index = branch->output_stream->index;
        branch->encoded_packet->pos = -1;
        packet_size = branch->encoded_packet->size;
        pipeline_set_deadline(pipeline, pipeline->io_timeout_ms);
        result = av_interleaved_write_frame(pipeline->output, branch->encoded_packet);
        pipeline_clear_deadline(pipeline);
        av_packet_unref(branch->encoded_packet);
        if (result < 0) {
            pipeline_ffmpeg_error(error, pipeline->nodes[pipeline->sink_node].id,
                                  "write encoded packet", result);
            return 0;
        }
        atomic_fetch_add_explicit(&pipeline->packets_written, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&pipeline->bytes_written, (unsigned long long)packet_size,
                                  memory_order_relaxed);
    }
}

static int pipeline_send_encoded_frame(turbo_pipeline_t *pipeline,
                                       pipeline_branch_t *branch, AVFrame *frame,
                                       turbo_pipeline_error_t *error) {
    int result;
    for (;;) {
        result = avcodec_send_frame(branch->encoder, frame);
        if (result != AVERROR(EAGAIN)) break;
        if (!pipeline_receive_encoded(pipeline, branch, error)) return 0;
    }
    if (result < 0 && result != AVERROR_EOF) {
        pipeline_ffmpeg_error(error, pipeline->nodes[branch->encoder_node].id,
                              frame ? "send frame to encoder" : "drain encoder", result);
        return 0;
    }
    if (frame)
        atomic_fetch_add_explicit(&pipeline->frames_encoded, 1, memory_order_relaxed);
    return pipeline_receive_encoded(pipeline, branch, error);
}

static int pipeline_receive_filtered(turbo_pipeline_t *pipeline,
                                     pipeline_branch_t *branch,
                                     turbo_pipeline_error_t *error) {
    const char *node_id = branch->filter_node >= 0
                              ? pipeline->nodes[branch->filter_node].id
                              : pipeline->nodes[branch->encoder_node].id;
    for (;;) {
        int result = av_buffersink_get_frame(branch->filter_sink,
                                             branch->filtered_frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 1;
        if (result < 0) {
            pipeline_ffmpeg_error(error, node_id, "receive filtered frame", result);
            return 0;
        }
        if (branch->filtered_frame->pts != AV_NOPTS_VALUE)
            branch->filtered_frame->pts =
                av_rescale_q(branch->filtered_frame->pts, branch->filter_time_base,
                             branch->encoder->time_base);
        if (!pipeline_send_encoded_frame(pipeline, branch, branch->filtered_frame, error)) {
            av_frame_unref(branch->filtered_frame);
            return 0;
        }
        av_frame_unref(branch->filtered_frame);
    }
}

static int pipeline_process_decoded_frame(turbo_pipeline_t *pipeline,
                                          pipeline_branch_t *branch,
                                          turbo_pipeline_error_t *error) {
    int result;
    if (branch->decoded_frame->best_effort_timestamp != AV_NOPTS_VALUE)
        branch->decoded_frame->pts = branch->decoded_frame->best_effort_timestamp;
    if (!branch->encoder &&
        !pipeline_open_encoder(pipeline, branch, branch->decoded_frame, error))
        return 0;
    if (!pipeline_filter_init(pipeline, branch, branch->decoded_frame, error)) return 0;
    atomic_fetch_add_explicit(&pipeline->frames_decoded, 1, memory_order_relaxed);
    result = av_buffersrc_add_frame_flags(branch->filter_source, branch->decoded_frame,
                                          AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_unref(branch->decoded_frame);
    if (result < 0) {
        pipeline_ffmpeg_error(error,
                              branch->filter_node >= 0
                                  ? pipeline->nodes[branch->filter_node].id
                                  : pipeline->nodes[branch->encoder_node].id,
                              "send frame to filter", result);
        return 0;
    }
    return pipeline_receive_filtered(pipeline, branch, error);
}

static int pipeline_receive_decoded(turbo_pipeline_t *pipeline,
                                    pipeline_branch_t *branch,
                                    turbo_pipeline_error_t *error) {
    for (;;) {
        int result = avcodec_receive_frame(branch->decoder, branch->decoded_frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 1;
        if (result < 0) {
            pipeline_ffmpeg_error(error, pipeline->nodes[branch->decoder_node].id,
                                  "receive decoded frame", result);
            return 0;
        }
        if (!pipeline_process_decoded_frame(pipeline, branch, error)) return 0;
    }
}

static int pipeline_transcode_packet(turbo_pipeline_t *pipeline,
                                     pipeline_branch_t *branch, const AVPacket *packet,
                                     turbo_pipeline_error_t *error) {
    int result;
    for (;;) {
        result = avcodec_send_packet(branch->decoder, packet);
        if (result != AVERROR(EAGAIN)) break;
        if (!pipeline_receive_decoded(pipeline, branch, error)) return 0;
    }
    if (result < 0) {
        pipeline_ffmpeg_error(error, pipeline->nodes[branch->decoder_node].id,
                              "send packet to decoder", result);
        return 0;
    }
    return pipeline_receive_decoded(pipeline, branch, error);
}

static int pipeline_copy_packet(turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
                                AVPacket *packet, turbo_pipeline_error_t *error) {
    int result;
    int packet_size = packet->size;
    av_packet_rescale_ts(packet, branch->input_stream->time_base,
                         branch->output_stream->time_base);
    packet->stream_index = branch->output_stream->index;
    packet->pos = -1;
    pipeline_set_deadline(pipeline, pipeline->io_timeout_ms);
    result = av_interleaved_write_frame(pipeline->output, packet);
    pipeline_clear_deadline(pipeline);
    if (result < 0) {
        pipeline_ffmpeg_error(error, pipeline->nodes[pipeline->sink_node].id,
                              "write copied packet", result);
        return 0;
    }
    atomic_fetch_add_explicit(&pipeline->packets_written, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&pipeline->bytes_written, (unsigned long long)packet_size,
                              memory_order_relaxed);
    return 1;
}

static int pipeline_drain_branch(turbo_pipeline_t *pipeline, pipeline_branch_t *branch,
                                 turbo_pipeline_error_t *error) {
    int result;
    if (!branch->configured || branch->copy) return 1;
    result = avcodec_send_packet(branch->decoder, NULL);
    if (result < 0 && result != AVERROR_EOF) {
        pipeline_ffmpeg_error(error, pipeline->nodes[branch->decoder_node].id,
                              "drain decoder", result);
        return 0;
    }
    if (!pipeline_receive_decoded(pipeline, branch, error)) return 0;
    if (branch->filter_graph) {
        result = av_buffersrc_add_frame_flags(branch->filter_source, NULL, 0);
        if (result < 0 && result != AVERROR_EOF) {
            pipeline_ffmpeg_error(
                error,
                branch->filter_node >= 0 ? pipeline->nodes[branch->filter_node].id
                                         : pipeline->nodes[branch->encoder_node].id,
                "drain filter", result);
            return 0;
        }
        if (!pipeline_receive_filtered(pipeline, branch, error)) return 0;
    }
    return pipeline_send_encoded_frame(pipeline, branch, NULL, error);
}

static turbo_pipeline_status_t pipeline_write_header(turbo_pipeline_t *pipeline,
                                                     turbo_pipeline_error_t *error) {
    int result;
    pipeline_set_deadline(pipeline, pipeline->open_timeout_ms);
    result = avformat_write_header(pipeline->output, &pipeline->output_options);
    pipeline_clear_deadline(pipeline);
    if (result < 0)
        return pipeline_ffmpeg_error(error, pipeline->nodes[pipeline->mux_node].id,
                                     "write output header", result);
    pipeline->header_written = 1;
    return pipeline_reject_unused_option(
        pipeline->output_options, pipeline->nodes[pipeline->mux_node].id,
        "output header", error);
}

static turbo_pipeline_status_t pipeline_write_trailer(turbo_pipeline_t *pipeline,
                                                      turbo_pipeline_error_t *error) {
    int result;
    if (!pipeline->header_written) return TURBO_PIPELINE_OK;
    pipeline_set_deadline(pipeline, pipeline->io_timeout_ms);
    result = av_write_trailer(pipeline->output);
    pipeline_clear_deadline(pipeline);
    pipeline->header_written = 0;
    if (result < 0)
        return pipeline_ffmpeg_error(error, pipeline->nodes[pipeline->mux_node].id,
                                     "write output trailer", result);
    return TURBO_PIPELINE_OK;
}

static turbo_pipeline_status_t pipeline_run_runtime_rtp(
    turbo_pipeline_t *pipeline, turbo_pipeline_error_t *error) {
    pipeline_runtime_rtp_t *runtime = &pipeline->runtime_rtp;
    turbo_pipeline_status_t status = TURBO_PIPELINE_OK;
    int stopped = 0;
    for (;;) {
        turbo_media_frame_t frame;
        int rc;
        turbo_mutex_lock(&runtime->mutex);
        while (runtime->count == 0 &&
               !atomic_load_explicit(&pipeline->stop_requested,
                                     memory_order_acquire) &&
               atomic_load_explicit(&runtime->async_status,
                                    memory_order_acquire) ==
                   TURBO_PIPELINE_OK)
            turbo_cond_wait(&runtime->available, &runtime->mutex);
        status = (turbo_pipeline_status_t)atomic_load_explicit(
            &runtime->async_status, memory_order_acquire);
        if (status != TURBO_PIPELINE_OK) {
            runtime->accepting = 0;
            runtime->count = 0;
            turbo_mutex_unlock(&runtime->mutex);
            break;
        }
        if (atomic_load_explicit(&pipeline->stop_requested,
                                 memory_order_acquire)) {
            runtime->accepting = 0;
            runtime->count = 0;
            stopped = 1;
            turbo_mutex_unlock(&runtime->mutex);
            break;
        }
        frame = runtime->entries[runtime->head].frame;
        turbo_mutex_unlock(&runtime->mutex);

        rc = pipeline_runtime_process_packet(pipeline, &frame, error);

        turbo_mutex_lock(&runtime->mutex);
        runtime->head = (runtime->head + 1u) % runtime->queue_capacity;
        runtime->count--;
        turbo_mutex_unlock(&runtime->mutex);
        atomic_fetch_add_explicit(&pipeline->packets_read, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&pipeline->bytes_read,
                                  (unsigned long long)frame.size,
                                  memory_order_relaxed);
        if (rc != TURBO_MEDIA_OK) {
            status = error && error->code != TURBO_PIPELINE_OK
                         ? error->code
                         : TURBO_PIPELINE_EIO;
            break;
        }
    }
    pipeline_release_runtime(pipeline);
    if (stopped) {
        atomic_store_explicit(&pipeline->stop_requested, 0,
                              memory_order_release);
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_STOPPED,
                              memory_order_release);
        return pipeline_error_set(error, TURBO_PIPELINE_ESTOPPED, NULL,
                                  "pipeline stopped by request");
    }
    if (status == TURBO_PIPELINE_EBACKPRESSURE) {
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_FAILED,
                              memory_order_release);
        return pipeline_error_set(
            error, status, pipeline->nodes[pipeline->source_node].id,
            "Runtime RTP input queue is full or a packet exceeds max_packet_bytes");
    }
    atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_FAILED,
                          memory_order_release);
    if (error && error->code != TURBO_PIPELINE_OK) return status;
    return pipeline_error_set(
        error, status == TURBO_PIPELINE_OK ? TURBO_PIPELINE_EIO : status,
        pipeline->nodes[pipeline->demux_node].id,
        "Runtime RTP depacketize/packetize or sink publish failed");
}

turbo_pipeline_status_t turbo_pipeline_run(turbo_pipeline_t *pipeline,
                                           turbo_pipeline_error_t *error) {
    turbo_pipeline_status_t status;
    int stopped = 0;
    int result;
    pipeline_error_clear(error);
    if (!pipeline)
        return pipeline_error_set(error, TURBO_PIPELINE_EINVAL, NULL,
                                  "pipeline is required");
    if (atomic_load_explicit(&pipeline->state, memory_order_acquire) !=
        TURBO_PIPELINE_STATE_PREPARED)
        return pipeline_error_set(error, TURBO_PIPELINE_ESTATE, NULL,
                                  "run requires PREPARED state");
    atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_RUNNING,
                          memory_order_release);
    if (atomic_load_explicit(&pipeline->stop_requested, memory_order_acquire)) {
        pipeline_release_runtime(pipeline);
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_STOPPED,
                              memory_order_release);
        return pipeline_error_set(error, TURBO_PIPELINE_ESTOPPED, NULL,
                                  "pipeline stopped by request");
    }
    if (pipeline->execution_mode == PIPELINE_EXECUTION_RUNTIME_RTP)
        return pipeline_run_runtime_rtp(pipeline, error);
    status = pipeline_write_header(pipeline, error);
    if (status != TURBO_PIPELINE_OK) goto fail;

    for (;;) {
        pipeline_branch_t *branch = NULL;
        if (atomic_load_explicit(&pipeline->stop_requested, memory_order_acquire)) {
            stopped = 1;
            break;
        }
        pipeline_set_deadline(pipeline, pipeline->io_timeout_ms);
        result = av_read_frame(pipeline->input, pipeline->input_packet);
        pipeline_clear_deadline(pipeline);
        if (result == AVERROR_EOF) {
            if (atomic_load_explicit(&pipeline->stop_requested,
                                     memory_order_acquire))
                stopped = 1;
            break;
        }
        if (result < 0) {
            if (atomic_load_explicit(&pipeline->stop_requested,
                                     memory_order_acquire)) {
                stopped = 1;
                break;
            }
            status = pipeline_ffmpeg_error(error,
                                           pipeline->nodes[pipeline->source_node].id,
                                           "read input packet", result);
            goto fail;
        }
        atomic_fetch_add_explicit(&pipeline->packets_read, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&pipeline->bytes_read,
                                  (unsigned long long)pipeline->input_packet->size,
                                  memory_order_relaxed);
        if (pipeline->audio.configured &&
            pipeline->input_packet->stream_index == pipeline->audio.input_stream_index)
            branch = &pipeline->audio;
        else if (pipeline->video.configured &&
                 pipeline->input_packet->stream_index == pipeline->video.input_stream_index)
            branch = &pipeline->video;

        if (branch) {
            int ok = branch->copy
                         ? pipeline_copy_packet(pipeline, branch,
                                                pipeline->input_packet, error)
                         : pipeline_transcode_packet(pipeline, branch,
                                                     pipeline->input_packet, error);
            av_packet_unref(pipeline->input_packet);
            if (!ok) {
                status = error ? error->code : TURBO_PIPELINE_EFFMPEG;
                goto fail;
            }
        } else {
            av_packet_unref(pipeline->input_packet);
        }
    }

    if (atomic_load_explicit(&pipeline->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&pipeline->state, memory_order_acquire) ==
            TURBO_PIPELINE_STATE_STOPPING)
        stopped = 1;
    if (!stopped &&
        (!pipeline_drain_branch(pipeline, &pipeline->audio, error) ||
         !pipeline_drain_branch(pipeline, &pipeline->video, error))) {
        status = error ? error->code : TURBO_PIPELINE_EFFMPEG;
        goto fail;
    }
    if (stopped)
        atomic_store_explicit(&pipeline->stop_requested, 0, memory_order_release);
    status = pipeline_write_trailer(pipeline, error);
    if (status != TURBO_PIPELINE_OK) goto fail;
    pipeline_release_runtime(pipeline);
    atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_STOPPED,
                          memory_order_release);
    if (stopped)
        return pipeline_error_set(error, TURBO_PIPELINE_ESTOPPED, NULL,
                                  "pipeline stopped by request");
    return TURBO_PIPELINE_OK;

fail:
    av_packet_unref(pipeline->input_packet);
    pipeline_release_runtime(pipeline);
    if (atomic_load_explicit(&pipeline->stop_requested, memory_order_acquire)) {
        atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_STOPPED,
                              memory_order_release);
        return pipeline_error_set(error, TURBO_PIPELINE_ESTOPPED, NULL,
                                  "pipeline stopped by request");
    }
    atomic_store_explicit(&pipeline->state, TURBO_PIPELINE_STATE_FAILED,
                          memory_order_release);
    return status;
}

turbo_pipeline_status_t turbo_pipeline_request_stop(turbo_pipeline_t *pipeline) {
    int state;
    int expected;
    if (!pipeline) return TURBO_PIPELINE_EINVAL;
    state = atomic_load_explicit(&pipeline->state, memory_order_acquire);
    if (state != TURBO_PIPELINE_STATE_PREPARED &&
        state != TURBO_PIPELINE_STATE_RUNNING &&
        state != TURBO_PIPELINE_STATE_STOPPING)
        return TURBO_PIPELINE_ESTATE;
    atomic_store_explicit(&pipeline->stop_requested, 1, memory_order_release);
    if (pipeline->execution_mode == PIPELINE_EXECUTION_RUNTIME_RTP &&
        pipeline->runtime_rtp.sync_initialized) {
        turbo_mutex_lock(&pipeline->runtime_rtp.mutex);
        pipeline->runtime_rtp.accepting = 0;
        turbo_cond_broadcast(&pipeline->runtime_rtp.available);
        turbo_mutex_unlock(&pipeline->runtime_rtp.mutex);
    }
    if (state == TURBO_PIPELINE_STATE_RUNNING) {
        expected = TURBO_PIPELINE_STATE_RUNNING;
        atomic_compare_exchange_strong_explicit(
            &pipeline->state, &expected, TURBO_PIPELINE_STATE_STOPPING,
            memory_order_acq_rel, memory_order_acquire);
    }
    return TURBO_PIPELINE_OK;
}

turbo_pipeline_state_t turbo_pipeline_state(const turbo_pipeline_t *pipeline) {
    if (!pipeline) return TURBO_PIPELINE_STATE_FAILED;
    return (turbo_pipeline_state_t)atomic_load_explicit(&pipeline->state,
                                                        memory_order_acquire);
}

turbo_pipeline_status_t turbo_pipeline_stats(const turbo_pipeline_t *pipeline,
                                             turbo_pipeline_stats_t *stats) {
    if (!pipeline || !stats) return TURBO_PIPELINE_EINVAL;
    stats->packets_read =
        atomic_load_explicit(&pipeline->packets_read, memory_order_relaxed);
    stats->packets_written =
        atomic_load_explicit(&pipeline->packets_written, memory_order_relaxed);
    stats->frames_decoded =
        atomic_load_explicit(&pipeline->frames_decoded, memory_order_relaxed);
    stats->frames_encoded =
        atomic_load_explicit(&pipeline->frames_encoded, memory_order_relaxed);
    stats->bytes_read =
        atomic_load_explicit(&pipeline->bytes_read, memory_order_relaxed);
    stats->bytes_written =
        atomic_load_explicit(&pipeline->bytes_written, memory_order_relaxed);
    return TURBO_PIPELINE_OK;
}

void turbo_pipeline_destroy(turbo_pipeline_t *pipeline) {
    int state;
    if (!pipeline) return;
    state = atomic_load_explicit(&pipeline->state, memory_order_acquire);
    if (state == TURBO_PIPELINE_STATE_RUNNING ||
        state == TURBO_PIPELINE_STATE_STOPPING) {
        turbo_pipeline_request_stop(pipeline);
        return;
    }
    pipeline_release_runtime(pipeline);
    free(pipeline);
}
