#include "turbo_streamgraph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tm_sg_program {
    uint32_t node_count;
    uint32_t edge_count;
    tm_sg_node_v1 *nodes;
    tm_sg_edge_v1 *edges;
    uint32_t *order;
};

static tm_sg_status fail(tm_sg_diagnostic_v1 *d, tm_sg_status code,
                         uint32_t node, uint32_t edge, const char *message) {
    if (d) {
        d->status = code;
        d->node_index = node;
        d->edge_index = edge;
        (void)snprintf(d->message, sizeof(d->message), "%s", message);
    }
    return code;
}

static int valid_id(const char *s) {
    size_t i;
    if (!s || !s[0]) return 0;
    for (i = 0; i <= TM_SG_NAME_LIMIT; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (!c) return 1;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 0;
}

static char *clone_id(const char *src) {
    size_t n = strlen(src) + 1u;
    char *dst = (char *)malloc(n);
    if (dst) memcpy(dst, src, n);
    return dst;
}

void tm_sg_program_destroy(tm_sg_program *p) {
    uint32_t i, j;
    if (!p) return;
    for (i = 0; i < p->node_count; ++i) {
        tm_sg_port_v1 *ports = (tm_sg_port_v1 *)p->nodes[i].ports;
        if (ports) {
            for (j = 0; j < p->nodes[i].port_count; ++j)
                free((void *)ports[j].name);
        }
        free(ports);
        free((void *)p->nodes[i].id);
    }
    for (i = 0; i < p->edge_count; ++i) {
        free((void *)p->edges[i].source_node);
        free((void *)p->edges[i].source_port);
        free((void *)p->edges[i].target_node);
        free((void *)p->edges[i].target_port);
    }
    free(p->nodes);
    free(p->edges);
    free(p->order);
    free(p);
}

static int find_node(const tm_sg_program *p, const char *id) {
    uint32_t i;
    for (i = 0; i < p->node_count; ++i)
        if (strcmp(p->nodes[i].id, id) == 0) return (int)i;
    return -1;
}

static int find_port(const tm_sg_node_v1 *n, const char *id, uint32_t direction) {
    uint32_t i;
    for (i = 0; i < n->port_count; ++i)
        if (n->ports[i].direction == direction && strcmp(n->ports[i].name, id) == 0)
            return (int)i;
    return -1;
}

static tm_sg_status clone_descriptors(tm_sg_program *p, const tm_sg_node_v1 *nodes,
                                      const tm_sg_edge_v1 *edges, tm_sg_diagnostic_v1 *d) {
    uint32_t i, j, k;
    for (i = 0; i < p->node_count; ++i) {
        const tm_sg_node_v1 *source = &nodes[i];
        tm_sg_node_v1 *target = &p->nodes[i];
        if (source->version != TM_SG_ABI_VERSION)
            return fail(d, TM_SG_EVERSION, i, UINT32_MAX, "node version mismatch");
        if (source->size != sizeof(*source) || !valid_id(source->id) ||
            source->kind < TM_SG_SOURCE || source->kind > TM_SG_SINK ||
            !source->ports || !source->port_count ||
            source->port_count > TM_SG_MAX_PORTS_PER_NODE)
            return fail(d, TM_SG_EINVAL, i, UINT32_MAX, "invalid node descriptor");
        *target = *source;
        target->id = NULL;
        target->ports = NULL;
        target->id = clone_id(source->id);
        if (!target->id) return fail(d, TM_SG_ENOMEM, i, UINT32_MAX, "node ID allocation failed");
        target->ports = (tm_sg_port_v1 *)calloc(source->port_count, sizeof(tm_sg_port_v1));
        if (!target->ports)
            return fail(d, TM_SG_ENOMEM, i, UINT32_MAX, "port allocation failed");
        for (j = 0; j < i; ++j)
            if (strcmp(p->nodes[j].id, source->id) == 0)
                return fail(d, TM_SG_EDUPLICATE, i, UINT32_MAX, "duplicate node");
        for (j = 0; j < source->port_count; ++j) {
            const tm_sg_port_v1 *v = &source->ports[j];
            tm_sg_port_v1 *copy = &((tm_sg_port_v1 *)target->ports)[j];
            if (v->version != TM_SG_ABI_VERSION)
                return fail(d, TM_SG_EVERSION, i, UINT32_MAX, "port version mismatch");
            if (v->size != sizeof(*v) || !valid_id(v->name) ||
                (v->direction != TM_SG_INPUT && v->direction != TM_SG_OUTPUT) ||
                v->kind < TM_SG_BYTE_STREAM || v->kind > TM_SG_OBSERVATION ||
                v->memory_domain > TM_SG_MEMORY_GPU ||
                (!!v->timebase_num != !!v->timebase_den) ||
                (v->kind == TM_SG_OBSERVATION && !v->schema_id))
                return fail(d, TM_SG_EINVAL, i, UINT32_MAX, "invalid port descriptor");
            if ((source->kind == TM_SG_SOURCE && v->direction != TM_SG_OUTPUT) ||
                (source->kind == TM_SG_SINK && v->direction != TM_SG_INPUT))
                return fail(d, TM_SG_EGRAPH, i, UINT32_MAX, "node kind/port direction mismatch");
            for (k = 0; k < j; ++k)
                if (strcmp(source->ports[k].name, v->name) == 0)
                    return fail(d, TM_SG_EDUPLICATE, i, UINT32_MAX, "duplicate port");
            *copy = *v;
            copy->name = NULL;
            copy->name = clone_id(v->name);
            if (!copy->name) return fail(d, TM_SG_ENOMEM, i, UINT32_MAX, "port ID allocation failed");
        }
    }
    for (i = 0; i < p->edge_count; ++i) {
        const tm_sg_edge_v1 *v = &edges[i];
        tm_sg_edge_v1 *copy = &p->edges[i];
        if (v->version != TM_SG_ABI_VERSION)
            return fail(d, TM_SG_EVERSION, UINT32_MAX, i, "edge version mismatch");
        if (v->size != sizeof(*v) || !valid_id(v->source_node) ||
            !valid_id(v->source_port) || !valid_id(v->target_node) ||
            !valid_id(v->target_port))
            return fail(d, TM_SG_EINVAL, UINT32_MAX, i, "invalid edge descriptor");
        copy->size = v->size;
        copy->version = v->version;
        copy->source_node = clone_id(v->source_node);
        copy->source_port = clone_id(v->source_port);
        copy->target_node = clone_id(v->target_node);
        copy->target_port = clone_id(v->target_port);
        if (!copy->source_node || !copy->source_port ||
            !copy->target_node || !copy->target_port)
            return fail(d, TM_SG_ENOMEM, UINT32_MAX, i, "edge allocation failed");
    }
    return TM_SG_OK;
}

static tm_sg_status validate_topology(tm_sg_program *p, uint32_t max_fanout,
                                       tm_sg_diagnostic_v1 *d) {
    uint32_t i, j, source_count = 0u, sink_count = 0u;
    uint32_t *in = NULL, *out = NULL, *degrees = NULL;
    uint8_t *seen = NULL;
    tm_sg_status rc = TM_SG_OK;
    const size_t port_slots = (size_t)p->node_count * TM_SG_MAX_PORTS_PER_NODE;
    in = (uint32_t *)calloc(port_slots, sizeof(uint32_t));
    out = (uint32_t *)calloc(port_slots, sizeof(uint32_t));
    degrees = (uint32_t *)calloc(p->node_count, sizeof(uint32_t));
    seen = (uint8_t *)calloc(p->node_count, sizeof(uint8_t));
    if (!in || !out || !degrees || !seen) {
        rc = fail(d, TM_SG_ENOMEM, UINT32_MAX, UINT32_MAX, "topology allocation failed");
        goto done;
    }
    for (i = 0; i < p->edge_count; ++i) {
        const tm_sg_edge_v1 *e = &p->edges[i];
        const int sn = find_node(p, e->source_node);
        const int tn = find_node(p, e->target_node);
        int sp, tp;
        const tm_sg_port_v1 *a, *b;
        if (sn == tn && sn >= 0) {
            rc = fail(d, TM_SG_ECYCLE, UINT32_MAX, i, "self edge");
            goto done;
        }
        if (sn < 0 || tn < 0) {
            rc = fail(d, TM_SG_ENOTFOUND, UINT32_MAX, i, "edge node missing");
            goto done;
        }
        sp = find_port(&p->nodes[sn], e->source_port, TM_SG_OUTPUT);
        tp = find_port(&p->nodes[tn], e->target_port, TM_SG_INPUT);
        if (sp < 0 || tp < 0) {
            rc = fail(d, TM_SG_ENOTFOUND, UINT32_MAX, i, "port or direction missing");
            goto done;
        }
        a = &p->nodes[sn].ports[sp];
        b = &p->nodes[tn].ports[tp];
        if (a->kind != b->kind || a->schema_id != b->schema_id) {
            rc = fail(d, TM_SG_ETYPE, UINT32_MAX, i, "Port type/schema mismatch");
            goto done;
        }
        if ((a->format_id && b->format_id && a->format_id != b->format_id) ||
            (a->memory_domain && b->memory_domain && a->memory_domain != b->memory_domain) ||
            (a->timebase_num && b->timebase_num &&
             (uint64_t)a->timebase_num * b->timebase_den !=
             (uint64_t)b->timebase_num * a->timebase_den)) {
            rc = fail(d, TM_SG_EFORMAT, UINT32_MAX, i, "explicit media conversion required");
            goto done;
        }
        if (++in[(size_t)tn * TM_SG_MAX_PORTS_PER_NODE + (uint32_t)tp] > 1u ||
            ++out[(size_t)sn * TM_SG_MAX_PORTS_PER_NODE + (uint32_t)sp] > max_fanout) {
            rc = fail(d, TM_SG_ELIMIT, UINT32_MAX, i, "port ingress/fanout exceeded");
            goto done;
        }
        degrees[tn]++;
    }
    for (i = 0; i < p->node_count; ++i) {
        const tm_sg_node_v1 *n = &p->nodes[i];
        if (n->kind == TM_SG_SOURCE) source_count++;
        if (n->kind == TM_SG_SINK) sink_count++;
        for (j = 0; j < n->port_count; ++j) {
            const tm_sg_port_v1 *v = &n->ports[j];
            const uint32_t hits = v->direction == TM_SG_INPUT
                ? in[(size_t)i * TM_SG_MAX_PORTS_PER_NODE + j]
                : out[(size_t)i * TM_SG_MAX_PORTS_PER_NODE + j];
            if (!hits) {
                rc = fail(d, TM_SG_EGRAPH, i, UINT32_MAX, "unconnected declared Port");
                goto done;
            }
        }
    }
    if (!source_count || !sink_count) {
        rc = fail(d, TM_SG_EGRAPH, UINT32_MAX, UINT32_MAX, "Source and Sink required");
        goto done;
    }
    for (i = 0; i < p->node_count; ++i) {
        int next = -1;
        for (j = 0; j < p->node_count; ++j)
            if (!seen[j] && degrees[j] == 0u) { next = (int)j; break; }
        if (next < 0) {
            rc = fail(d, TM_SG_ECYCLE, UINT32_MAX, UINT32_MAX, "directed cycle");
            goto done;
        }
        seen[next] = 1u;
        p->order[i] = (uint32_t)next;
        for (j = 0; j < p->edge_count; ++j)
            if (strcmp(p->edges[j].source_node, p->nodes[next].id) == 0) {
                const int ti = find_node(p, p->edges[j].target_node);
                if (ti >= 0) degrees[ti]--;
            }
    }
done:
    free(in);
    free(out);
    free(degrees);
    free(seen);
    return rc;
}

tm_sg_status tm_sg_compile(const tm_sg_node_v1 *nodes, size_t node_count,
                           const tm_sg_edge_v1 *edges, size_t edge_count,
                           const tm_sg_limits_v1 *limits, tm_sg_program **out,
                           tm_sg_diagnostic_v1 *diagnostic) {
    tm_sg_program *p;
    tm_sg_status rc;
    uint32_t max_nodes = 32u, max_edges = 64u, max_ports = 16u, max_fanout = 8u;
    size_t i;
    if (!out || *out != NULL || (diagnostic &&
        (diagnostic->size != sizeof(*diagnostic) || diagnostic->version != TM_SG_ABI_VERSION)))
        return TM_SG_EINVAL;
    if (diagnostic) {
        diagnostic->status = TM_SG_OK;
        diagnostic->node_index = diagnostic->edge_index = UINT32_MAX;
        diagnostic->message[0] = '\0';
    }
    if (limits) {
        if (limits->version != TM_SG_ABI_VERSION)
            return fail(diagnostic, TM_SG_EVERSION, UINT32_MAX, UINT32_MAX, "limits version mismatch");
        if (limits->size != sizeof(*limits) || !limits->max_nodes || !limits->max_edges ||
            !limits->max_ports_per_node || !limits->max_fanout ||
            limits->max_nodes > TM_SG_MAX_NODES || limits->max_edges > TM_SG_MAX_EDGES ||
            limits->max_ports_per_node > TM_SG_MAX_PORTS_PER_NODE ||
            limits->max_fanout > TM_SG_MAX_FANOUT)
            return fail(diagnostic, TM_SG_ELIMIT, UINT32_MAX, UINT32_MAX, "invalid limits");
        max_nodes = limits->max_nodes;
        max_edges = limits->max_edges;
        max_ports = limits->max_ports_per_node;
        max_fanout = limits->max_fanout;
    }
    if (!nodes || !edges || !node_count || !edge_count ||
        node_count > max_nodes || edge_count > max_edges)
        return fail(diagnostic, TM_SG_ELIMIT, UINT32_MAX, UINT32_MAX, "graph limit exceeded");
    for (i = 0; i < node_count; ++i)
        if (nodes[i].port_count > max_ports)
            return fail(diagnostic, TM_SG_ELIMIT, (uint32_t)i, UINT32_MAX, "node port limit");
    p = (tm_sg_program *)calloc(1u, sizeof(*p));
    if (!p) return fail(diagnostic, TM_SG_ENOMEM, UINT32_MAX, UINT32_MAX, "program allocation");
    p->node_count = (uint32_t)node_count;
    p->edge_count = (uint32_t)edge_count;
    p->nodes = (tm_sg_node_v1 *)calloc(node_count, sizeof(tm_sg_node_v1));
    p->edges = (tm_sg_edge_v1 *)calloc(edge_count, sizeof(tm_sg_edge_v1));
    p->order = (uint32_t *)calloc(node_count, sizeof(uint32_t));
    if (!p->nodes || !p->edges || !p->order) {
        tm_sg_program_destroy(p);
        return fail(diagnostic, TM_SG_ENOMEM, UINT32_MAX, UINT32_MAX, "program arrays");
    }
    rc = clone_descriptors(p, nodes, edges, diagnostic);
    if (rc == TM_SG_OK) rc = validate_topology(p, max_fanout, diagnostic);
    if (rc != TM_SG_OK) {
        tm_sg_program_destroy(p);
        return rc;
    }
    *out = p;
    return TM_SG_OK;
}

tm_sg_status tm_sg_program_node_at(const tm_sg_program *p, uint32_t i,
                                   const tm_sg_node_v1 **out) {
    if (!p || !out || i >= p->node_count) return TM_SG_EINVAL;
    *out = &p->nodes[p->order[i]];
    return TM_SG_OK;
}
tm_sg_status tm_sg_program_edge_at(const tm_sg_program *p, uint32_t i,
                                   const tm_sg_edge_v1 **out) {
    if (!p || !out || i >= p->edge_count) return TM_SG_EINVAL;
    *out = &p->edges[i];
    return TM_SG_OK;
}
uint32_t tm_sg_program_node_count(const tm_sg_program *p) { return p ? p->node_count : 0u; }
uint32_t tm_sg_program_edge_count(const tm_sg_program *p) { return p ? p->edge_count : 0u; }
