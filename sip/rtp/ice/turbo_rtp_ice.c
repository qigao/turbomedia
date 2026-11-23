#include "turbo_rtp_ice.h"

#include <core/error.h>
#include <core/fmt.h>
#include <turbo_rtp_packet.h>
#include <stdlib.h>
#include <string.h>

enum {
    RTP_ICE_COMPONENT = 1,
    RTP_ICE_RTCP_FIRST_TYPE = 192,
    RTP_ICE_RTCP_LAST_TYPE = 223,
    RTP_ICE_RTCP_HEADER_SIZE = 4,
    RTP_ICE_CANDIDATE_LINE_SIZE = 256
};

struct turbo_rtp_ice {
    salts_ice_agent_t *agent;
    turbo_rtp_ice_receive_fn on_receive;
    void *receive_context;
    int remote_set;
    int closing;
    int in_callback;
};

static int ice_transport_status(const turbo_rtp_ice_t *transport)
{
    if (!transport) return SALTS_EINVAL;
    if (transport->in_callback) return SALTS_EBUSY;
    return transport->closing ? ICE_AGENT_ERROR_CLOSED : SALTS_OK;
}

static int ice_connected(turbo_rtp_ice_t *transport)
{
    ice_state_t state = ice_agent_get_state(transport->agent);
    return state == ICE_STATE_CONNECTED || state == ICE_STATE_COMPLETED;
}

static int packet_kind(const uint8_t *packet, size_t size)
{
    if (!packet || size < RTP_ICE_RTCP_HEADER_SIZE || size > TURBO_RTP_ICE_MAX_PACKET_BYTES ||
        (packet[0] >> 6) != RTP_VERSION) return -1;
    if (packet[1] >= RTP_ICE_RTCP_FIRST_TYPE && packet[1] <= RTP_ICE_RTCP_LAST_TYPE)
        return TURBO_RTP_ICE_RTCP;
    return size >= RTP_HEADER_SIZE ? TURBO_RTP_ICE_RTP : -1;
}

static void ice_receive(salts_ice_agent_t *agent, const void *data, size_t size, void *context)
{
    (void)agent;
    turbo_rtp_ice_t *transport = context;
    int kind = packet_kind(data, size);
    if (kind >= 0 && !transport->closing) {
        transport->in_callback = 1;
        transport->on_receive(transport->receive_context, (turbo_rtp_ice_packet_t)kind, data, size);
        transport->in_callback = 0;
    }
}

turbo_rtp_ice_t *turbo_rtp_ice_create(const ice_config_t *config,
                                      turbo_rtp_ice_receive_fn on_receive, void *context)
{
    if (!config || !on_receive || config->lite_mode ||
        config->stun_server_count < 0 || config->stun_server_count > ICE_MAX_STUN_SERVERS ||
        config->turn_server_count < 0 || config->turn_server_count > ICE_MAX_TURN_SERVERS)
        return NULL;

    turbo_rtp_ice_t *transport = calloc(1, sizeof(*transport));
    if (!transport) return NULL;
    transport->agent = ice_agent_create(config);
    if (!transport->agent) {
        free(transport);
        return NULL;
    }
    transport->on_receive = on_receive;
    transport->receive_context = context;
    const ice_callbacks_t callbacks = {.on_data = ice_receive, .user_data = transport};
    ice_agent_set_callbacks(transport->agent, &callbacks);
    return transport;
}

int turbo_rtp_ice_gather(turbo_rtp_ice_t *transport)
{
    int status = ice_transport_status(transport);
    if (status != 0) return status;
    if (ice_agent_get_gathering_state(transport->agent) != ICE_GATHERING_NEW)
        return SALTS_EBUSY;
    return ice_agent_gather_candidates(transport->agent);
}

static int media_shape(const sdp_media_t *media)
{
    if (!media || (media->type != SDP_MEDIA_AUDIO && media->type != SDP_MEDIA_VIDEO) ||
        media->codec_count <= 0 || media->codec_count > SDP_MAX_CODECS)
        return SALTS_EINVAL;
    for (int i = 0; i < media->codec_count; ++i) {
        int pt = media->codecs[i].payload_type;
        /* These payload types overlap the RTCP type range when the RTP M bit is set. */
        if (pt < 0 || pt > 127 || (pt >= 64 && pt <= 95)) return SALTS_EPROTO;
    }
    return 0;
}

static int pack_local_candidate(const ice_candidate_t *ice, sdp_candidate_t *sdp)
{
    const char *type = ice_candidate_type_name(ice->type);
    if (!type || !ice->ip[0] || !ice->port ||
        strlen(ice->foundation) >= sizeof(sdp->foundation) ||
        strlen(type) >= sizeof(sdp->type)) return SALTS_EPROTO;
    *sdp = (sdp_candidate_t){
        .component = RTP_ICE_COMPONENT, .priority = ice->priority,
        .port = ice->port, .rel_port = ice->related_port
    };
    strcpy(sdp->transport, "udp");
    strcpy(sdp->type, type);
    strcpy(sdp->foundation, ice->foundation);
    memcpy(sdp->address, ice->ip, sizeof(sdp->address));
    memcpy(sdp->rel_addr, ice->related_ip, sizeof(sdp->rel_addr));
    return 0;
}

int turbo_rtp_ice_write_local(turbo_rtp_ice_t *transport, sdp_media_t *media)
{
    int status = ice_transport_status(transport);
    if (status == 0) status = media_shape(media);
    if (status != 0) return status;
    if (ice_agent_get_gathering_state(transport->agent) != ICE_GATHERING_COMPLETE)
        return SALTS_EBUSY;

    sdp_candidate_t candidates[SDP_MAX_CANDIDATES];
    int used = 0;
    int default_index = 0;
    int count = ice_agent_get_local_candidate_count(transport->agent);
    if (count < 1 || count > SDP_MAX_CANDIDATES) return SALTS_EPROTO;

    for (int i = 0; i < count; ++i) {
        ice_candidate_t ice;
        status = ice_agent_get_local_candidate(transport->agent, i, &ice);
        if (status != 0) return status;
        if (ice.component_id != RTP_ICE_COMPONENT || ice.transport != ICE_TRANSPORT_UDP) continue;
        status = pack_local_candidate(&ice, &candidates[used]);
        if (status != 0) return status;
        /* Advertise a relay by default where present and prefer the lower-priority
         * reachable candidate across types. ICE still checks the full set. */
        if (used == 0 || candidates[used].priority < candidates[default_index].priority)
            default_index = used;
        ++used;
    }
    if (used == 0) return SALTS_EPROTO;
    ice_agent_get_local_credentials(transport->agent, media->ice_ufrag, sizeof(media->ice_ufrag),
                                     media->ice_pwd, sizeof(media->ice_pwd));
    memcpy(media->candidates, candidates, (size_t)used * sizeof(candidates[0]));
    media->candidate_count = used;
    media->rtcp_mux = 1;
    media->port = candidates[default_index].port;
    memcpy(media->connection_addr, candidates[default_index].address, sizeof(media->connection_addr));
    return SALTS_OK;
}

static int token_size(const char *data, size_t capacity)
{
    const char *end = memchr(data, '\0', capacity);
    if (!end) return -1;
    for (const char *p = data; p < end; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c >= 127) return -1;
    }
    return (int)(end - data);
}

static int remote_candidate(const sdp_candidate_t *sdp, char line[RTP_ICE_CANDIDATE_LINE_SIZE])
{
    if (token_size(sdp->foundation, sizeof(sdp->foundation)) <= 0 ||
        token_size(sdp->address, sizeof(sdp->address)) <= 0 ||
        token_size(sdp->type, sizeof(sdp->type)) <= 0 ||
        token_size(sdp->rel_addr, sizeof(sdp->rel_addr)) < 0 || !sdp->port)
        return SALTS_EPROTO;

    int size = fmt(line, RTP_ICE_CANDIDATE_LINE_SIZE, "candidate:{} 1 UDP {} {} {} typ {}",
                   sdp->foundation, sdp->priority, sdp->address, (unsigned)sdp->port, sdp->type);
    if (size < 0 || size >= RTP_ICE_CANDIDATE_LINE_SIZE) return SALTS_EPROTO;
    if (sdp->rel_addr[0] && sdp->rel_port) {
        int available = RTP_ICE_CANDIDATE_LINE_SIZE - size;
        int suffix = fmt(line + size, (size_t)available, " raddr {} rport {}",
                         sdp->rel_addr, (unsigned)sdp->rel_port);
        if (suffix < 0 || suffix >= available) return SALTS_EPROTO;
    }
    ice_candidate_t parsed;
    return ice_candidate_parse(line, &parsed) == 0 ? SALTS_OK : SALTS_EPROTO;
}

int turbo_rtp_ice_set_remote(turbo_rtp_ice_t *transport, const sdp_media_t *media)
{
    int status = ice_transport_status(transport);
    if (status == 0) status = media_shape(media);
    if (status != 0) return status;
    if (transport->remote_set) return SALTS_EBUSY;
    if (!media->rtcp_mux || !media->port) return SALTS_ENOTSUP;
    if (token_size(media->ice_ufrag, sizeof(media->ice_ufrag)) < ICE_UFRAG_LEN ||
        token_size(media->ice_pwd, sizeof(media->ice_pwd)) < ICE_PWD_LEN ||
        media->candidate_count <= 0 || media->candidate_count > SDP_MAX_CANDIDATES)
        return SALTS_EPROTO;

    char lines[SDP_MAX_CANDIDATES][RTP_ICE_CANDIDATE_LINE_SIZE];
    int used = 0;
    for (int i = 0; i < media->candidate_count; ++i) {
        const sdp_candidate_t *candidate = &media->candidates[i];
        int length = token_size(candidate->transport, sizeof(candidate->transport));
        if (length < 0) return SALTS_EPROTO;
        if (candidate->component != RTP_ICE_COMPONENT ||
            !vstr_ieq(vstr_from_buf(candidate->transport, (size_t)length), vstr_from_cstr("udp"))) continue;
        status = remote_candidate(candidate, lines[used]);
        if (status != 0) return status;
        ++used;
    }
    if (used == 0 || used > ICE_MAX_CANDIDATES) return SALTS_ENOTSUP;

    status = ice_agent_set_remote_credentials(transport->agent, media->ice_ufrag, media->ice_pwd);
    for (int i = 0; status == 0 && i < used; ++i)
        status = ice_agent_add_remote_candidate(transport->agent, lines[i]);
    if (status != 0) {
        transport->closing = 1;
        ice_agent_close(transport->agent);
        return status;
    }
    ice_agent_end_of_candidates(transport->agent);
    transport->remote_set = 1;
    return SALTS_OK;
}

int turbo_rtp_ice_connect(turbo_rtp_ice_t *transport)
{
    int status = ice_transport_status(transport);
    if (status != 0) return status;
    if (!transport->remote_set || ice_agent_get_gathering_state(transport->agent) != ICE_GATHERING_COMPLETE)
        return SALTS_EBUSY;
    if (ice_connected(transport)) return SALTS_OK;
    status = ice_agent_start_checks(transport->agent);
    if (status != 0) return status;
    return ice_connected(transport) ? SALTS_OK : SALTS_ENOTCONN;
}

int turbo_rtp_ice_poll(turbo_rtp_ice_t *transport, uint32_t timeout_ms)
{
    int status = ice_transport_status(transport);
    if (status != 0) return status;
    if (!ice_connected(transport)) return SALTS_ENOTCONN;
    ice_agent_poll_selected_pair(transport->agent, timeout_ms);
    return ice_connected(transport) ? SALTS_OK : SALTS_ENOTCONN;
}

int turbo_rtp_ice_send(turbo_rtp_ice_t *transport, turbo_rtp_ice_packet_t kind,
                       const void *data, size_t size)
{
    int status = ice_transport_status(transport);
    if (status != 0) return status;
    int actual = packet_kind(data, size);
    if (actual < 0 || actual != (int)kind) return SALTS_EINVAL;
    if (!ice_connected(transport)) return SALTS_ENOTCONN;
    return ice_agent_send(transport->agent, data, size);
}

int turbo_rtp_ice_get_selected_pair(turbo_rtp_ice_t *transport, ice_candidate_t *local,
                                    ice_candidate_t *remote)
{
    int status = ice_transport_status(transport);
    if (status != 0) return status;
    if (!local || !remote) return SALTS_EINVAL;
    if (!ice_connected(transport)) return SALTS_ENOTCONN;
    return ice_agent_get_selected_pair(transport->agent, local, remote);
}

int turbo_rtp_ice_destroy(turbo_rtp_ice_t **transport, uint32_t timeout_ms)
{
    if (!transport) return SALTS_EINVAL;
    turbo_rtp_ice_t *owner = *transport;
    if (!owner) return SALTS_OK;
    if (owner->in_callback) return SALTS_EBUSY;
    owner->closing = 1;
    int status = ice_agent_destroy_checked(&owner->agent, timeout_ms);
    if (status == 0) {
        free(owner);
        *transport = NULL;
    }
    return status;
}
