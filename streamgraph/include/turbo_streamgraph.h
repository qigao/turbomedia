#ifndef TURBO_STREAMGRAPH_H
#define TURBO_STREAMGRAPH_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TM_SG_ABI_VERSION 1u
#define TM_SG_MAX_NODES 128u
#define TM_SG_MAX_EDGES 512u
#define TM_SG_MAX_PORTS_PER_NODE 32u
#define TM_SG_MAX_FANOUT 32u
#define TM_SG_NAME_LIMIT 63u

typedef enum tm_sg_status {
    TM_SG_OK = 0,
    TM_SG_EINVAL = -1,
    TM_SG_EVERSION = -2,
    TM_SG_ELIMIT = -3,
    TM_SG_ENOMEM = -4,
    TM_SG_EDUPLICATE = -5,
    TM_SG_ENOTFOUND = -6,
    TM_SG_ETYPE = -7,
    TM_SG_EFORMAT = -8,
    TM_SG_ECYCLE = -9,
    TM_SG_EGRAPH = -10
} tm_sg_status;

typedef enum tm_sg_node_kind {
    TM_SG_SOURCE = 1,
    TM_SG_PROCESSOR = 2,
    TM_SG_SINK = 3
} tm_sg_node_kind;

typedef enum tm_sg_port_kind {
    TM_SG_BYTE_STREAM = 1,
    TM_SG_RTP_PACKET,
    TM_SG_ENCODED_PACKET,
    TM_SG_VIDEO_FRAME,
    TM_SG_AUDIO_FRAME,
    TM_SG_OBSERVATION
} tm_sg_port_kind;

typedef enum tm_sg_port_direction {
    TM_SG_INPUT = 1,
    TM_SG_OUTPUT = 2
} tm_sg_port_direction;

typedef enum tm_sg_memory_domain {
    TM_SG_MEMORY_ANY = 0,
    TM_SG_MEMORY_CPU = 1,
    TM_SG_MEMORY_GPU = 2
} tm_sg_memory_domain;

/* Non-owning input descriptors; compiler deep-copies all IDs and arrays. */
typedef struct tm_sg_port_v1 {
    uint32_t size;
    uint32_t version;
    const char *name;
    uint32_t direction;
    uint32_t kind;
    uint32_t format_id;       /* 0 = explicitly unconstrained; not conversion */
    uint32_t memory_domain;   /* ANY requires a resolved provider-capability gate */
    uint32_t timebase_num;    /* either both 0 (unconstrained) or both > 0 */
    uint32_t timebase_den;
    uint64_t schema_id;       /* required for typed Observation ports */
} tm_sg_port_v1;

#define TM_SG_PORT_V1_INIT {sizeof(tm_sg_port_v1), TM_SG_ABI_VERSION, NULL, 0u, 0u, 0u, TM_SG_MEMORY_ANY, 0u, 0u, 0u}

typedef struct tm_sg_node_v1 {
    uint32_t size;
    uint32_t version;
    const char *id;
    uint32_t kind;
    uint32_t port_count;
    const tm_sg_port_v1 *ports;
} tm_sg_node_v1;

#define TM_SG_NODE_V1_INIT {sizeof(tm_sg_node_v1), TM_SG_ABI_VERSION, NULL, 0u, 0u, NULL}

typedef struct tm_sg_edge_v1 {
    uint32_t size;
    uint32_t version;
    const char *source_node;
    const char *source_port;
    const char *target_node;
    const char *target_port;
} tm_sg_edge_v1;

#define TM_SG_EDGE_V1_INIT {sizeof(tm_sg_edge_v1), TM_SG_ABI_VERSION, NULL, NULL, NULL, NULL}

typedef struct tm_sg_limits_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t max_nodes;
    uint32_t max_edges;
    uint32_t max_ports_per_node;
    uint32_t max_fanout;
} tm_sg_limits_v1;

#define TM_SG_LIMITS_V1_INIT {sizeof(tm_sg_limits_v1), TM_SG_ABI_VERSION, 32u, 64u, 16u, 8u}

typedef struct tm_sg_diagnostic_v1 {
    uint32_t size;
    uint32_t version;
    int32_t status;
    uint32_t node_index;
    uint32_t edge_index;
    char message[160];
} tm_sg_diagnostic_v1;

#define TM_SG_DIAGNOSTIC_V1_INIT {sizeof(tm_sg_diagnostic_v1), TM_SG_ABI_VERSION, TM_SG_OK, UINT32_MAX, UINT32_MAX, {0}}

typedef struct tm_sg_program tm_sg_program;

/* Pure preflight: no I/O, Codec, FFmpeg, native session or threads. */
TURBO_MEDIA_API tm_sg_status tm_sg_compile(const tm_sg_node_v1 *nodes, size_t node_count,
                                           const tm_sg_edge_v1 *edges, size_t edge_count,
                                           const tm_sg_limits_v1 *limits,
                                           tm_sg_program **out,
                                           tm_sg_diagnostic_v1 *diagnostic);

/* Borrowed immutable view; valid only until tm_sg_program_destroy(). */
TURBO_MEDIA_API tm_sg_status tm_sg_program_node_at(const tm_sg_program *program,
                                                   uint32_t topological_index,
                                                   const tm_sg_node_v1 **out_node);
TURBO_MEDIA_API tm_sg_status tm_sg_program_edge_at(const tm_sg_program *program,
                                                   uint32_t index,
                                                   const tm_sg_edge_v1 **out_edge);
TURBO_MEDIA_API uint32_t tm_sg_program_node_count(const tm_sg_program *program);
TURBO_MEDIA_API uint32_t tm_sg_program_edge_count(const tm_sg_program *program);
TURBO_MEDIA_API void tm_sg_program_destroy(tm_sg_program *program);

#ifdef __cplusplus
}
#endif
#endif
