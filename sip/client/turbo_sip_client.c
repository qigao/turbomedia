#include "turbo_sip_client.h"

#include "sip-internal.h"
#include "sip-uas.h"
#include "uac/sip-uac-transaction.h"
#include "uac/sip-register-digest.h"
#include "uas/sip-uas-transaction.h"
#include "rtp.h"
#include "turbo_rtp_packet.h"
#include "turbo_audio_codec.h"

#include <core/tstr.h>
#include <crypto/symmetric.h>
#include <errno.h>
#include <limits.h>

enum {
    SIP_CLIENT_REQUEST_LIMIT = 32,
    SIP_CLIENT_VALUE_LIMIT = 511,
    SIP_CLIENT_FRAME_MS = 20,
    SIP_CLIENT_AUDIO_MAX_SAMPLES = 5760 * 2,
    SIP_CLIENT_MEDIA_PACKET_BYTES = 2048
};

enum request_kind { REQUEST_INVITE, REQUEST_CANCEL, REQUEST_BYE, REQUEST_MESSAGE };

struct client_request {
    struct sip_uac_transaction_t* transaction;
    struct turbo_sip_client* client;
    struct turbo_sip_call* call;
    struct sip_dialog_t* bye_dialog;
    tstr body;
    struct sip_register_digest digest[2];
    unsigned stale_retried;
    enum request_kind kind;
    int obsolete;
    int code;
    const struct sip_message_t* reply; /* Valid through the current input call. */
    struct sip_dialog_t* accepted;
};

struct turbo_sip_call {
    turbo_sip_client_t* client;
    unsigned references;
    turbo_sip_call_state_t state;
    struct sip_uas_transaction_t* incoming;
    struct sip_dialog_t* dialog;
    sdp_session_t local;
    sdp_session_t remote;
    int audio_index;
    int payload;
    int prepared;
    int cancel_requested;
    int provisional_received;
    int can_send;
    int can_recv;
    turbo_audio_codec_t* encoder;
    turbo_audio_codec_t* decoder;
    void* rtp;
    uint32_t ssrc;
    uint32_t timestamp;
    uint16_t sequence;
    uint64_t next_rtcp;
    uint32_t peer_ssrc;
    int have_peer_ssrc;
};

struct turbo_sip_client {
    turbo_sip_client_config_t config;
    struct sip_agent_t* agent;
    sip_registration_t* registration;
    sip_registration_status_t registration_status;
    turbo_sip_call_t* call;
    struct client_request requests[SIP_CLIENT_REQUEST_LIMIT];
    sdp_codec_t codec;
    uint64_t now;
    int started;
    int in_callback;
};

static int client_ready(const turbo_sip_client_t* client)
{
    if (!client || !client->started) return -EINVAL;
    return client->in_callback ? -EBUSY : 0;
}

static int callback_status(int status)
{
    return status <= 0 ? status : -EIO;
}

static void emit(turbo_sip_client_t* client, const turbo_sip_event_t* event)
{
    if (client->config.on_event) {
        client->in_callback = 1;
        client->config.on_event(client->config.context, event);
        client->in_callback = 0;
    }
}

static int request_via(void* context, const char* next_hop, char protocol[16], char local[128], char dns[128])
{
    turbo_sip_client_t* client = context;
    const turbo_sip_signaling_t* signaling = &client->config.signaling;
    client->in_callback = 1;
    int result = signaling->request.via(signaling->context, next_hop, protocol, local, dns);
    client->in_callback = 0;
    return callback_status(result);
}

static int request_send(void* context, const void* packet, size_t bytes)
{
    turbo_sip_client_t* client = context;
    const turbo_sip_signaling_t* signaling = &client->config.signaling;
    client->in_callback = 1;
    int result = signaling->request.send(signaling->context, packet, bytes);
    client->in_callback = 0;
    return callback_status(result);
}

static int reply_send(void* context, const vstr* protocol, const vstr* sent_by, const vstr* received,
                      int rport, const void* packet, int bytes)
{
    turbo_sip_client_t* client = context;
    const turbo_sip_signaling_t* signaling = &client->config.signaling;
    client->in_callback = 1;
    int result = signaling->reply(signaling->context, protocol, sent_by, received, rport, packet, bytes);
    client->in_callback = 0;
    return callback_status(result);
}

static struct sip_transport_t client_transport = {request_via, request_send};

static int valid_text(const char* text)
{
    if (!text || !*text || strlen(text) > SIP_CLIENT_VALUE_LIMIT) return 0;
    for (; *text; ++text) {
        const unsigned char c = (unsigned char)*text;
        if (c < 32 || c == 127) return 0;
    }
    return 1;
}

static int has_media(const turbo_sip_client_t* client)
{
    return client->config.media.prepare != NULL;
}

static void media_close(turbo_sip_call_t* call)
{
    turbo_sip_client_t* client = call->client;
    if (call->prepared) {
        call->prepared = 0;
        client->in_callback = 1;
        client->config.media.close(client->config.media.context, call);
        client->in_callback = 0;
    }
    turbo_audio_codec_destroy(call->encoder);
    turbo_audio_codec_destroy(call->decoder);
    if (call->rtp) rtp_destroy(call->rtp);
    call->encoder = NULL;
    call->decoder = NULL;
    call->rtp = NULL;
}

static void call_unref(turbo_sip_call_t* call)
{
    if (!call || --call->references) return;
    media_close(call);
    if (call->incoming) sip_uas_transaction_release(call->incoming);
    if (call->dialog) sip_dialog_release(call->dialog);
    free(call);
}

static void call_changed(turbo_sip_call_t* call, turbo_sip_call_state_t state, int sip_code, int error)
{
    call->state = state;
    if (state == TURBO_SIP_CALL_ENDED)
        media_close(call);
    if (call->client->call == call) {
        const turbo_sip_event_t event = {
            .type = TURBO_SIP_CALL_CHANGED, .call = call, .call_state = state,
            .sip_code = sip_code, .error = error
        };
        emit(call->client, &event);
    }
}

static turbo_sip_call_t* new_call(turbo_sip_client_t* client)
{
    turbo_sip_call_t* call = calloc(1, sizeof(*call));
    if (!call) return NULL;
    call->client = client;
    call->references = 1;
    sdp_session_init(&call->local);
    return call;
}

static int pcm_media_prepare(turbo_sip_call_t* call, int offerer)
{
    turbo_sip_client_t* client = call->client;
    sdp_media_t* audio = sdp_add_audio(&call->local, NULL, SDP_DIRECTION_SENDRECV);
    if (!audio) return -ENOMEM;
    strcpy(audio->protocol, "RTP/AVP");
    if (sdp_media_add_codec(audio, &client->codec) != 0) return -EINVAL;
    client->in_callback = 1;
    int result = client->config.media.prepare(client->config.media.context, call, offerer, audio);
    client->in_callback = 0;
    if (result) return callback_status(result);
    call->prepared = 1;
    if (!audio->port || !*audio->connection_addr || strcmp(audio->protocol, "RTP/AVP") ||
        audio->codec_count != 1 || audio->codecs[0].payload_type != client->codec.payload_type ||
        strcmp(audio->codecs[0].name, client->codec.name) ||
        audio->codecs[0].clock_rate != client->codec.clock_rate ||
        audio->codecs[0].channels != client->codec.channels)
        return -EINVAL;
    return 0;
}

static int pcm_media_start(turbo_sip_call_t* call)
{
    turbo_sip_client_t* client = call->client;
    const sdp_media_t* local = &call->local.media[call->audio_index];
    const sdp_media_t* remote = &call->remote.media[call->audio_index];
    const turbo_audio_codec_config_t config = {
        .sample_rate = client->codec.clock_rate, .channels = client->codec.channels,
        .frame_size_ms = SIP_CLIENT_FRAME_MS
    };
    uint32_t sequence;
    if (sip_random_u32(&call->ssrc) != 0 || sip_random_u32(&call->timestamp) != 0 ||
        sip_random_u32(&sequence) != 0)
        return -EIO;
    call->sequence = (uint16_t)sequence;
    call->encoder = turbo_audio_codec_create_encoder(client->config.audio_codec, &config);
    call->decoder = turbo_audio_codec_create_decoder(client->config.audio_codec, &config);
    call->rtp = rtp_create(NULL, NULL, call->ssrc, call->timestamp, client->codec.clock_rate, 64000, 1);
    if (!call->encoder || !call->decoder || !call->rtp) return -ENOMEM;
    if (rtp_set_info(call->rtp, call->local.session_id, "TurboMedia") != 0) return -ENOMEM;

    call->can_send = (local->direction == SDP_DIRECTION_SENDRECV || local->direction == SDP_DIRECTION_SENDONLY) &&
                     (remote->direction == SDP_DIRECTION_SENDRECV || remote->direction == SDP_DIRECTION_RECVONLY);
    call->can_recv = (local->direction == SDP_DIRECTION_SENDRECV || local->direction == SDP_DIRECTION_RECVONLY) &&
                     (remote->direction == SDP_DIRECTION_SENDRECV || remote->direction == SDP_DIRECTION_SENDONLY);
    call->next_rtcp = client->now + (uint64_t)rtp_rtcp_interval(call->rtp);
    client->in_callback = 1;
    int result = client->config.media.start(client->config.media.context, call, local, remote);
    client->in_callback = 0;
    return callback_status(result);
}

static int pcm_media_send(turbo_sip_call_t* call, turbo_sip_packet_kind_t kind, const void* packet, size_t bytes)
{
    turbo_sip_client_t* client = call->client;
    client->in_callback = 1;
    int result = client->config.media.send(client->config.media.context, call, kind, packet, bytes);
    client->in_callback = 0;
    return callback_status(result);
}

static struct client_request* claim_request(turbo_sip_client_t* client, enum request_kind kind, turbo_sip_call_t* call)
{
    for (size_t i = 0; i < SIP_CLIENT_REQUEST_LIMIT; ++i) {
        struct client_request* request = &client->requests[i];
        if (!request->client) {
            request->client = client;
            request->kind = kind;
            request->call = call;
            if (call) ++call->references;
            return request;
        }
    }
    return NULL;
}

static void release_request(struct client_request* request)
{
    sip_uac_transaction_release(request->transaction);
    if (request->bye_dialog) sip_dialog_release(request->bye_dialog);
    if (request->body) tstr_free(request->body);
    call_unref(request->call);
    memset(request, 0, sizeof(*request));
}

static int receive_reply(void* context, const struct sip_message_t* reply,
                         struct sip_uac_transaction_t* transaction, int code)
{
    struct client_request* request = context;
    (void)transaction;
    request->code = code;
    request->reply = reply;
    return 0;
}

static int receive_invite_reply(void* context, const struct sip_message_t* reply,
                                struct sip_uac_transaction_t* transaction, struct sip_dialog_t* dialog,
                                const vstr* id, int code)
{
    struct client_request* request = context;
    (void)id;
    request->accepted = code >= 200 && code < 300 ? dialog : NULL;
    return receive_reply(context, reply, transaction, code);
}

static int send_request(struct client_request* request)
{
    return sip_uac_send(request->transaction, request->body, request->body ? (int)tstr_len(request->body) : 0,
                        &client_transport, request->client);
}

static int add_digest_headers(struct client_request* request)
{
    static const char* const names[] = {"Authorization", "Proxy-Authorization"};
    struct sip_message_t* msg = request->transaction->req;
    char uri[1024];
    int size = sip_message_write_target(msg, uri, uri + sizeof(uri));
    if (size < 0 || (size_t)size >= sizeof(uri)) return -E2BIG;
    uri[size] = '\0';
    for (size_t i = 0; i < 2; ++i) {
        if (!request->digest[i].algorithm) continue;
        char header[SIP_DATAGRAM_MAX_BYTES];
        const turbo_sip_client_config_t* config = &request->client->config;
        int result = sip_digest_write(&request->digest[i], config->username, msg->cseq.method,
                                      config->password, uri, header, sizeof(header));
        if (result == 0)
            result = sip_uac_add_header(request->transaction, names[i], header);
        salts_crypto_clear(header, sizeof(header));
        if (result != 0) return result;
        ++request->digest[i].nonce_count;
    }
    return 0;
}

static int make_authenticated_request(struct client_request* next, const struct client_request* previous)
{
    const struct sip_message_t* old = previous->transaction->req;
    uint32_t sequence = old->cseq.id;
    if (previous->bye_dialog && previous->bye_dialog->local.id > sequence)
        sequence = previous->bye_dialog->local.id;
    if (sequence >= INT_MAX) return -EOVERFLOW;
    struct sip_message_t* message = sip_message_create(SIP_MESSAGE_REQUEST);
    if (!message) return -ENOMEM;
    sip_params_free(&message->headers);
    sip_params_init(&message->headers);
    int result = sip_message_clone(message, old);
    if (result == 0 && message->ptr.ptr == message->ptr.end) result = -E2BIG;
    if (result == 0) {
        sip_vias_free(&message->vias);
        sip_vias_init(&message->vias);
        for (int i = sip_params_count(&message->headers) - 1; i >= 0; --i) {
            const struct sip_param_t* header = sip_params_get(&message->headers, i);
            if (!sip_sv_compare_cstr_ci(&header->name, "Authorization") ||
                !sip_sv_compare_cstr_ci(&header->name, "Proxy-Authorization"))
                vec_erase(&message->headers.items, (size_t)i, NULL);
        }
        message->cseq.id = sequence + 1;
        next->transaction = sip_uac_transaction_create(next->client->agent, message);
        if (!next->transaction) result = -ENOMEM;
    }
    if (result != 0) {
        sip_message_destroy(message);
        return result;
    }

    next->transaction->param = next;
    if (next->kind == REQUEST_INVITE) next->transaction->oninvite = receive_invite_reply;
    else next->transaction->onreply = receive_reply;
    if (previous->bye_dialog) {
        next->bye_dialog = previous->bye_dialog;
        sip_dialog_addref(next->bye_dialog);
        next->bye_dialog->local.id = message->cseq.id;
    }
    return 0;
}

/* Positive means the challenge was handled. Zero leaves it to the caller. */
static int retry_authentication(struct client_request* previous, int code, const struct sip_message_t* reply)
{
    if (!reply || !previous->client->config.username || previous->kind == REQUEST_CANCEL ||
        (code != 401 && code != 407) || (previous->call && previous->call->cancel_requested &&
         previous->kind == REQUEST_INVITE))
        return 0;

    unsigned slot = code == 401 ? 0 : 1;
    struct sip_register_digest challenge;
    sip_register_digest_challenge(reply, slot == 0 ? "WWW-Authenticate" : "Proxy-Authenticate", &challenge);
    if (!challenge.algorithm) return 0;
    const struct sip_register_digest* last = &previous->digest[slot];
    if (last->algorithm && (!challenge.stale || (previous->stale_retried & (1u << slot)) ||
        challenge.algorithm < last->algorithm || strcmp(challenge.realm, last->realm) ||
        !strcmp(challenge.nonce, last->nonce)))
        return 0;

    struct client_request* request = claim_request(previous->client, previous->kind, previous->call);
    if (!request) return -EBUSY;
    memcpy(request->digest, previous->digest, sizeof(request->digest));
    request->digest[slot] = challenge;
    request->stale_retried = previous->stale_retried | (last->algorithm ? 1u << slot : 0);
    request->obsolete = previous->obsolete;

    int result = make_authenticated_request(request, previous);
    if (result == 0) result = add_digest_headers(request);
    if (result == 0) {
        request->body = previous->body;
        previous->body = NULL;
        result = send_request(request);
    }
    if (result != 0) {
        release_request(request);
        return result;
    }
    previous->obsolete = 1;
    if (request->kind == REQUEST_INVITE) request->call->provisional_received = 0;
    return 1;
}

static int start_bye(turbo_sip_call_t* call, struct sip_dialog_t* dialog, int extra_branch)
{
    struct client_request* request = claim_request(call->client, REQUEST_BYE, call);
    if (!request) return -EBUSY;
    request->transaction = sip_uac_bye(call->client->agent, dialog, receive_reply, request);
    request->obsolete = extra_branch;
    request->bye_dialog = dialog;
    sip_dialog_addref(dialog);
    int result = request->transaction ? send_request(request) : -ENOMEM;
    if (result != 0) release_request(request);
    return result;
}

static int start_cancel(struct client_request* invite)
{
    struct client_request* request = claim_request(invite->client, REQUEST_CANCEL, invite->call);
    if (!request) return -EBUSY;
    request->transaction = sip_uac_cancel(invite->client->agent, invite->transaction, receive_reply, request);
    int result = request->transaction ? send_request(request) : -ENOMEM;
    if (result != 0) release_request(request);
    return result;
}

static int is_sdp(const struct sip_message_t* message)
{
    const vstr* type = sip_message_get_header_by_name(message, "Content-Type");
    if (!type) return 0;
    const char* end = memchr(type->data, ';', type->len);
    size_t size = end ? (size_t)(end - type->data) : type->len;
    return vstr_ieq(vstr_trim(vstr_from_buf(type->data, size), " \t"), vstr_from_cstr("application/sdp"));
}

static int use_answer(turbo_sip_call_t* call, const struct sip_message_t* reply)
{
    if (!reply || !is_sdp(reply) || sdp_parse(reply->payload, (size_t)reply->size, &call->remote) != 0 ||
        call->remote.media_count != call->local.media_count)
        return -EPROTO;
    const sdp_media_t* local = &call->local.media[0];
    const sdp_media_t* remote = &call->remote.media[0];
    sdp_codec_t negotiated;
    if (remote->type != SDP_MEDIA_AUDIO || !remote->port || strcmp(remote->protocol, local->protocol) ||
        (remote->rtcp_mux && !local->rtcp_mux) ||
        sdp_negotiate_codecs(local, remote, &negotiated, 1) != 1 ||
        negotiated.payload_type != local->codecs[0].payload_type)
        return -EPROTO;
    call->payload = negotiated.payload_type;
    return pcm_media_start(call);
}

static int process_accepted(struct client_request* request, struct sip_dialog_t* dialog,
                            const struct sip_message_t* response, int code)
{
    turbo_sip_call_t* call = request->call;
    int extra = request->obsolete || call->dialog || call->state == TURBO_SIP_CALL_ENDED;
    int result = sip_uac_ack_dialog(request->transaction, dialog, NULL, 0, NULL);
    if (extra)
        return result == 0 ? start_bye(call, dialog, 1) : result;

    sip_dialog_addref(dialog);
    call->dialog = dialog;
    if (result == 0 && !call->cancel_requested)
        result = use_answer(call, response);
    if (result == 0 && !call->cancel_requested) {
        call_changed(call, TURBO_SIP_CALL_ACTIVE, code, 0);
        return 0;
    }

    int bye = start_bye(call, dialog, 0);
    media_close(call);
    call_changed(call, bye == 0 ? TURBO_SIP_CALL_ENDING : TURBO_SIP_CALL_ENDED, code,
                 result != 0 ? result : bye);
    return 0;
}

static int process_replies(turbo_sip_client_t* client)
{
    int first_error = 0;
    for (size_t i = 0; i < SIP_CLIENT_REQUEST_LIMIT; ++i) {
        struct client_request* request = &client->requests[i];
        if (!request->client || !request->code) continue;
        int code = request->code;
        const struct sip_message_t* response = request->reply;
        struct sip_dialog_t* accepted = request->accepted;
        request->code = 0;
        request->reply = NULL;
        request->accepted = NULL;
        if (accepted) {
            int result = process_accepted(request, accepted, response, code);
            if (first_error == 0) first_error = result;
            continue;
        }
        if (request->obsolete && request->kind != REQUEST_BYE) continue;
        turbo_sip_call_t* call = request->call;
        if (code < 200) {
            if (request->kind == REQUEST_INVITE && !call->provisional_received) {
                call->provisional_received = 1;
                if (call->cancel_requested) {
                    int result = start_cancel(request);
                    if (first_error == 0) first_error = result;
                }
            }
            if (request->kind == REQUEST_INVITE && code >= 180 && call->state == TURBO_SIP_CALL_DIALING)
                call_changed(call, TURBO_SIP_CALL_RINGING, code, 0);
            continue;
        }
        int result = retry_authentication(request, code, response);
        if (result > 0) continue;
        if (request->kind == REQUEST_MESSAGE) {
            const turbo_sip_event_t event = {.type = TURBO_SIP_MESSAGE_RESULT, .sip_code = code, .error = result};
            emit(client, &event);
            request->obsolete = 1;
        } else if (request->kind != REQUEST_CANCEL && !request->obsolete && call->state != TURBO_SIP_CALL_ENDED) {
            call_changed(call, TURBO_SIP_CALL_ENDED, code, result);
        }
    }

    for (size_t i = 0; i < SIP_CLIENT_REQUEST_LIMIT; ++i) {
        struct client_request* request = &client->requests[i];
        if (request->client && !request->transaction->linked)
            release_request(request);
    }
    return first_error;
}

static int on_incoming(void* context, const struct sip_message_t* req,
                       struct sip_uas_transaction_t* tx, struct sip_dialog_t* existing,
                       const vstr* id, const void* data, int bytes)
{
    turbo_sip_client_t* client = context;
    (void)id;
    if (existing || !has_media(client) || !is_sdp(req) || bytes <= 0)
        return sip_uas_reply(tx, 488, NULL, 0, client);
    if (client->call)
        return sip_uas_reply(tx, 486, NULL, 0, client);

    turbo_sip_call_t* call = new_call(client);
    if (!call) return sip_uas_reply(tx, 500, NULL, 0, client);
    if (!tx->dialog || !sip_sv_valid(&tx->dialog->remote.target.host) ||
        sdp_parse(data, (size_t)bytes, &call->remote) != 0) {
        call_unref(call);
        return sip_uas_reply(tx, 488, NULL, 0, client);
    }

    call->incoming = tx;
    sip_uas_transaction_addref(tx);
    call->dialog = tx->dialog;
    sip_dialog_addref(call->dialog);
    client->call = call;
    int result = sip_uas_reply(tx, 180, NULL, 0, client);
    call_changed(call, TURBO_SIP_CALL_INCOMING, 180, result);
    return result;
}

static int on_incoming_ack(void* context, const struct sip_message_t* req,
                           struct sip_uas_transaction_t* tx, struct sip_dialog_t* dialog,
                           const vstr* id, int code, const void* data, int bytes)
{
    turbo_sip_client_t* client = context;
    (void)req; (void)dialog; (void)id; (void)data; (void)bytes;
    turbo_sip_call_t* call = client->call;
    if (!call || call->incoming != tx || call->state == TURBO_SIP_CALL_ENDED) return 0;
    if (code != 200) {
        call_changed(call, TURBO_SIP_CALL_ENDED, code, 0);
        return 0;
    }
    if (call->state != TURBO_SIP_CALL_ANSWERING) return 0;
    int result = call->cancel_requested ? 0 : pcm_media_start(call);
    if (result == 0 && !call->cancel_requested) {
        call_changed(call, TURBO_SIP_CALL_ACTIVE, code, 0);
    } else {
        int bye = start_bye(call, call->dialog, 0);
        media_close(call);
        call_changed(call, bye == 0 ? TURBO_SIP_CALL_ENDING : TURBO_SIP_CALL_ENDED, code,
                     result != 0 ? result : bye);
    }
    return 0;
}

static int on_incoming_cancel(void* context, const struct sip_message_t* req,
                              struct sip_uas_transaction_t* tx, const vstr* id)
{
    turbo_sip_client_t* client = context;
    (void)id;
    turbo_sip_call_t* call = client->call;
    if (!call || call->state != TURBO_SIP_CALL_INCOMING ||
        !sip_sv_equal(&req->callid, &call->dialog->callid) ||
        !sip_sv_equal(&req->from.tag, &call->dialog->remote.uri.tag))
        return sip_uas_reply(tx, 481, NULL, 0, client);

    int result = sip_uas_reply(tx, 200, NULL, 0, client);
    int final = sip_uas_reply(call->incoming, 487, NULL, 0, client);
    call_changed(call, TURBO_SIP_CALL_ENDED, 487, final);
    return result == 0 ? final : result;
}

static int on_incoming_bye(void* context, const struct sip_message_t* req,
                           struct sip_uas_transaction_t* tx, const vstr* id)
{
    turbo_sip_client_t* client = context;
    (void)id;
    turbo_sip_call_t* call = client->call;
    if (!call || !call->dialog || call->state == TURBO_SIP_CALL_ENDED ||
        !sip_sv_equal(&req->callid, &call->dialog->callid) ||
        !sip_sv_equal(&req->from.tag, &call->dialog->remote.uri.tag) ||
        !sip_sv_equal(&req->to.tag, &call->dialog->local.uri.tag))
        return sip_uas_reply(tx, 481, NULL, 0, client);

    int result = sip_uas_reply(tx, 200, NULL, 0, client);
    call_changed(call, TURBO_SIP_CALL_ENDED, 200, result);
    return result;
}

static int on_message(void* context, const struct sip_message_t* req,
                      struct sip_uas_transaction_t* tx, const void* body, int bytes)
{
    turbo_sip_client_t* client = context;
    const vstr* type = sip_message_get_header_by_name(req, "Content-Type");
    if (bytes > 0 && !type) return sip_uas_reply(tx, 400, NULL, 0, client);
    char from[SIP_DATAGRAM_MAX_BYTES];
    int size = sip_uri_write(&req->from.uri, from, from + sizeof(from));
    if (size <= 0 || (size_t)size >= sizeof(from))
        return sip_uas_reply(tx, 400, NULL, 0, client);
    int result = sip_uas_reply(tx, 200, NULL, 0, client);
    const turbo_sip_event_t event = {
        .type = TURBO_SIP_MESSAGE_RECEIVED,
        .from = vstr_from_buf(from, (size_t)size),
        .content_type = type ? *type : vstr_from_cstr(""),
        .body = vstr_from_buf(body, (size_t)bytes),
        .sip_code = 200, .error = result
    };
    emit(client, &event);
    return result;
}

static int copy_config(turbo_sip_client_t* client, const turbo_sip_client_config_t* config)
{
    client->config = *config;
    client->config.address_of_record = tstr_from_v(vstr_from_cstr(config->address_of_record));
    client->config.registrar = config->registrar ? tstr_from_v(vstr_from_cstr(config->registrar)) : NULL;
    client->config.contact = config->contact ? tstr_from_v(vstr_from_cstr(config->contact)) : NULL;
    client->config.username = config->username ? tstr_from_v(vstr_from_cstr(config->username)) : NULL;
    client->config.password = config->password ? tstr_from_v(vstr_from_cstr(config->password)) : NULL;
    if (!client->config.address_of_record || (config->registrar && !client->config.registrar) ||
        (config->contact && !client->config.contact) || (config->username && !client->config.username) ||
        (config->password && !client->config.password))
        return -ENOMEM;
    if (!client->config.expires_seconds) client->config.expires_seconds = 300;
    return 0;
}

turbo_sip_client_t* turbo_sip_client_create(const turbo_sip_client_config_t* config)
{
    if (!config || !valid_text(config->address_of_record) ||
        (config->registrar && !valid_text(config->registrar)) ||
        (config->contact && !valid_text(config->contact)) ||
        (!!config->username != !!config->password) ||
        (config->username && (!valid_text(config->username) || strchr(config->username, ':') ||
                             !*config->password || strlen(config->password) > SIP_CLIENT_VALUE_LIMIT)) ||
        config->expires_seconds > INT_MAX || !config->signaling.request.via ||
        !config->signaling.request.send || !config->signaling.reply)
        return NULL;
    const turbo_sip_media_t* media = &config->media;
    if ((media->prepare || media->start || media->send || media->close) &&
        (!media->prepare || !media->start || !media->send || !media->close))
        return NULL;

    const char* name = config->audio_codec ? config->audio_codec : "pcmu";
    static const struct {
        const char* factory;
        sdp_codec_t codec;
    } codecs[] = {
        {"pcmu", {.payload_type = 0, .name = "PCMU", .clock_rate = 8000, .channels = 1}},
        {"pcma", {.payload_type = 8, .name = "PCMA", .clock_rate = 8000, .channels = 1}},
        {"opus", {.payload_type = 111, .name = "opus", .clock_rate = 48000, .channels = 2}},
    };
    size_t i = 0;
    while (i < sizeof(codecs) / sizeof(codecs[0]) && sip_cstr_casecmp(name, codecs[i].factory)) ++i;
    if (i == sizeof(codecs) / sizeof(codecs[0])) return NULL;

    turbo_sip_client_t* client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    int result = copy_config(client, config);
    client->config.audio_codec = codecs[i].factory;
    client->codec = codecs[i].codec;
    if (result == 0) {
        struct sip_uas_handler_t handler = {
            .send = reply_send, .oninvite = on_incoming, .onack = on_incoming_ack,
            .oncancel = on_incoming_cancel, .onbye = on_incoming_bye, .onmessage = on_message
        };
        client->agent = sip_agent_create_owner(&handler);
    }
    if (!client->agent) {
        turbo_sip_client_destroy(client);
        return NULL;
    }
    return client;
}

void turbo_sip_client_destroy(turbo_sip_client_t* client)
{
    if (!client || client->in_callback) return;
    sip_registration_destroy(client->registration);
    if (client->agent) {
        sip_agent_abort_transactions(client->agent);
        for (size_t i = 0; i < SIP_CLIENT_REQUEST_LIMIT; ++i) {
            if (client->requests[i].client) release_request(&client->requests[i]);
        }
        call_unref(client->call);
        sip_agent_destroy(client->agent);
    }
    tstr_free((tstr)client->config.address_of_record);
    tstr_free((tstr)client->config.registrar);
    tstr_free((tstr)client->config.contact);
    tstr_free((tstr)client->config.username);
    if (client->config.password) {
        tstr password = (tstr)client->config.password;
        salts_crypto_clear(password, tstr_len(password));
        tstr_free(password);
    }
    free(client);
}

int turbo_sip_client_register(turbo_sip_client_t* client)
{
    int result = client_ready(client);
    if (result) return result;
    if (!client->config.registrar) return -ENOTSUP;
    if (client->registration && client->registration_status.state != SIP_REGISTRATION_FAILED &&
        client->registration_status.state != SIP_REGISTRATION_INACTIVE)
        return -EALREADY;
    sip_registration_config_t config = {
        .agent = client->agent, .transport = &client_transport, .transport_context = client,
        .address_of_record = client->config.address_of_record, .registrar = client->config.registrar,
        .contact = client->config.contact, .username = client->config.username, .password = client->config.password,
        .expires_seconds = client->config.expires_seconds
    };
    sip_registration_t* registration = sip_registration_create(&config);
    if (!registration) return -ENOMEM;
    sip_registration_destroy(client->registration);
    client->registration = registration;
    client->registration_status = (sip_registration_status_t){.state = SIP_REGISTRATION_PENDING};
    const turbo_sip_event_t event = {.type = TURBO_SIP_REGISTRATION, .registration = client->registration_status};
    emit(client, &event);
    return 0;
}

int turbo_sip_client_unregister(turbo_sip_client_t* client)
{
    int result = client_ready(client);
    if (result) return result;
    if (!client->registration) return -ENOENT;
    return sip_registration_unregister(client->registration);
}

static int poll_registration(turbo_sip_client_t* client, uint64_t now)
{
    if (!client->registration) return 0;
    sip_registration_status_t status;
    int result = sip_registration_poll(client->registration, now, &status);
    if (result) return result;
    sip_registration_status_t last = client->registration_status;
    client->registration_status = status;
    if (status.state != last.state || status.error != last.error || status.sip_code != last.sip_code ||
        status.expires_at_ms != last.expires_at_ms) {
        const turbo_sip_event_t event = {
            .type = TURBO_SIP_REGISTRATION, .registration = status,
            .sip_code = status.sip_code, .error = status.error
        };
        emit(client, &event);
    }
    return 0;
}

int turbo_sip_client_poll(turbo_sip_client_t* client, uint64_t now_ms)
{
    if (!client || now_ms < client->now || now_ms > UINT64_MAX - TIMER_C) return -EINVAL;
    if (client->in_callback) return -EBUSY;
    client->now = now_ms;
    client->started = 1;
    sip_agent_poll_timers(client->agent, now_ms);
    int result = process_replies(client);
    int registration = poll_registration(client, now_ms);
    if (result == 0) result = registration;
    turbo_sip_call_t* call = client->call;
    if (call && call->state == TURBO_SIP_CALL_ACTIVE && now_ms >= call->next_rtcp) {
        uint8_t packet[SIP_CLIENT_MEDIA_PACKET_BYTES];
        int bytes = rtp_rtcp_report(call->rtp, packet, sizeof(packet));
        int sent = bytes > 0 ? pcm_media_send(call, TURBO_SIP_RTCP, packet, (size_t)bytes) : -EIO;
        if (sent != -EAGAIN) call->next_rtcp = now_ms + (uint64_t)rtp_rtcp_interval(call->rtp);
        if (result == 0 && sent != -EAGAIN) result = sent;
    }
    return result;
}

int turbo_sip_client_input(turbo_sip_client_t* client, const void* packet, size_t bytes,
                           const char* peer_address, uint16_t peer_port)
{
    int result = client_ready(client);
    if (result) return result;
    struct sip_message_t* message = sip_message_parse_datagram(packet, bytes);
    if (!message) return -EINVAL;
    if (message->mode == SIP_MESSAGE_REQUEST) {
        if (!peer_address || !peer_port) result = -EINVAL;
        else result = sip_agent_set_rport(message, peer_address, peer_port);
    }
    if (result == 0)
        result = sip_agent_input(client->agent, message, client);
    int reply = process_replies(client);
    sip_message_destroy(message);
    return result == 0 ? reply : result;
}

int turbo_sip_client_message(turbo_sip_client_t* client, const char* target,
                             const char* content_type, const void* body, size_t bytes)
{
    int result = client_ready(client);
    if (result) return result;
    if (!valid_text(target) || !valid_text(content_type) || (!body && bytes) ||
        bytes > SIP_DATAGRAM_MAX_BYTES)
        return -EINVAL;
    for (size_t i = 0; i < SIP_CLIENT_REQUEST_LIMIT; ++i)
        if (client->requests[i].client && client->requests[i].kind == REQUEST_MESSAGE &&
            !client->requests[i].obsolete)
            return -EBUSY;

    struct client_request* request = claim_request(client, REQUEST_MESSAGE, NULL);
    if (!request) return -EBUSY;
    request->transaction = sip_uac_message(client->agent, client->config.address_of_record,
                                           target, receive_reply, request);
    if (bytes) request->body = tstr_from_v(vstr_from_buf(body, bytes));
    if (!request->transaction || (bytes && !request->body)) result = -ENOMEM;
    if (result == 0) result = sip_uac_add_header(request->transaction, "Content-Type", content_type);
    if (result == 0) result = send_request(request);
    if (result) release_request(request);
    return result;
}

int turbo_sip_client_dial(turbo_sip_client_t* client, const char* target, turbo_sip_call_t** out)
{
    int result = client_ready(client);
    if (result) return result;
    if (!out || !valid_text(target)) return -EINVAL;
    *out = NULL;
    if (client->call) return -EBUSY;
    if (!has_media(client)) return -ENOTSUP;
    turbo_sip_call_t* call = new_call(client);
    if (!call) return -ENOMEM;

    struct client_request* request = claim_request(client, REQUEST_INVITE, call);
    if (!request) {
        call_unref(call);
        return -EBUSY;
    }
    result = pcm_media_prepare(call, 1);
    char offer[SIP_DATAGRAM_MAX_BYTES];
    if (result == 0) {
        int bytes = sdp_generate(&call->local, offer, sizeof(offer));
        if (bytes <= 0) result = -E2BIG;
        else if (!(request->body = tstr_from_v(vstr_from_buf(offer, (size_t)bytes)))) result = -ENOMEM;
    }
    if (result == 0) {
        request->transaction = sip_uac_invite(client->agent, client->config.address_of_record,
                                              target, receive_invite_reply, request);
        if (!request->transaction) result = -ENOMEM;
    }
    if (result == 0 && client->config.contact)
        result = sip_uac_add_header(request->transaction, "Contact", client->config.contact);
    if (result == 0) result = sip_uac_add_header(request->transaction, "Content-Type", "application/sdp");
    if (result == 0) result = send_request(request);
    if (result) {
        release_request(request);
        call_unref(call);
        return result;
    }
    client->call = call;
    *out = call;
    call_changed(call, TURBO_SIP_CALL_DIALING, 0, 0);
    return 0;
}

int turbo_sip_call_answer(turbo_sip_call_t* call)
{
    if (!call) return -EINVAL;
    turbo_sip_client_t* client = call->client;
    int result = client_ready(client);
    if (result) return result;
    if (call->state != TURBO_SIP_CALL_INCOMING) return -EINVAL;

    sdp_session_t* answer = malloc(sizeof(*answer));
    if (!answer) return -ENOMEM;
    result = pcm_media_prepare(call, 0);
    if (result == 0 && sdp_build_answer(&call->local, &call->remote, answer) != 0)
        result = -EPROTO;
    if (result == 0) {
        int i = 0;
        while (i < answer->media_count && (answer->media[i].type != SDP_MEDIA_AUDIO || !answer->media[i].port)) ++i;
        if (i == answer->media_count) result = -ENOTSUP;
        else {
            call->local = *answer;
            call->audio_index = i;
            call->payload = answer->media[i].codecs[0].payload_type;
        }
    }
    free(answer);

    char description[SIP_DATAGRAM_MAX_BYTES];
    int bytes = 0;
    if (result == 0) {
        bytes = sdp_generate(&call->local, description, sizeof(description));
        if (bytes <= 0) result = -E2BIG;
    }
    if (result == 0 && client->config.contact)
        result = sip_uas_add_header(call->incoming, "Contact", client->config.contact);
    if (result == 0) result = sip_uas_add_header(call->incoming, "Content-Type", "application/sdp");
    if (result != 0) {
        int code = result == -ENOTSUP || result == -EPROTO ? 488 : 500;
        sip_uas_reply(call->incoming, code, NULL, 0, client);
        call_changed(call, TURBO_SIP_CALL_ENDED, code, result);
        return result;
    }
    result = sip_uas_reply(call->incoming, 200, description, bytes, client);
    if (call->incoming->status == SIP_UAS_TRANSACTION_ACCEPTED)
        call_changed(call, TURBO_SIP_CALL_ANSWERING, 200, result);
    else {
        sip_uas_reply(call->incoming, 500, NULL, 0, client);
        call_changed(call, TURBO_SIP_CALL_ENDED, 500, result);
    }
    return result;
}

int turbo_sip_call_hangup(turbo_sip_call_t* call)
{
    if (!call) return -EINVAL;
    turbo_sip_client_t* client = call->client;
    int result = client_ready(client);
    if (result) return result;
    switch (call->state) {
    case TURBO_SIP_CALL_INCOMING:
        result = sip_uas_reply(call->incoming, 486, NULL, 0, client);
        call_changed(call, TURBO_SIP_CALL_ENDED, 486, result);
        return result;
    case TURBO_SIP_CALL_DIALING:
    case TURBO_SIP_CALL_RINGING:
        if (call->provisional_received) {
            for (size_t i = 0; i < SIP_CLIENT_REQUEST_LIMIT; ++i) {
                struct client_request* request = &client->requests[i];
                if (request->call == call && request->kind == REQUEST_INVITE && !request->obsolete) {
                    result = start_cancel(request);
                    break;
                }
            }
        }
        if (result == 0) {
            call->cancel_requested = 1;
            call_changed(call, TURBO_SIP_CALL_ENDING, 0, 0);
        }
        return result;
    case TURBO_SIP_CALL_ANSWERING:
        call->cancel_requested = 1;
        return 0;
    case TURBO_SIP_CALL_ACTIVE:
        result = start_bye(call, call->dialog, 0);
        if (result == 0) {
            media_close(call);
            call_changed(call, TURBO_SIP_CALL_ENDING, 0, 0);
        }
        return result;
    default:
        return -EALREADY;
    }
}

int turbo_sip_call_release(turbo_sip_call_t* call)
{
    if (!call) return -EINVAL;
    turbo_sip_client_t* client = call->client;
    int result = client_ready(client);
    if (result) return result;
    if (client->call != call) return -EINVAL;
    if (call->state != TURBO_SIP_CALL_ENDED) return -EBUSY;
    client->call = NULL;
    call_unref(call);
    return 0;
}

int turbo_sip_call_send_pcm(turbo_sip_call_t* call, const int16_t* samples, size_t count)
{
    if (!call || !samples) return -EINVAL;
    const turbo_sip_client_t* client = call->client;
    int result = client_ready(client);
    if (result) return result;
    if (call->state != TURBO_SIP_CALL_ACTIVE || !call->can_send) return -ENOTCONN;
    const size_t frame = (size_t)client->codec.clock_rate * SIP_CLIENT_FRAME_MS / 1000;
    if (count != frame * (size_t)client->codec.channels) return -EINVAL;

    uint8_t payload[SIP_CLIENT_MEDIA_PACKET_BYTES - RTP_HEADER_SIZE];
    uint8_t wire[SIP_CLIENT_MEDIA_PACKET_BYTES];
    size_t bytes = sizeof(payload);
    turbo_encoded_frame_t info = {0};
    result = turbo_audio_codec_encode(call->encoder, (const uint8_t*)samples, count * sizeof(*samples),
                                      payload, &bytes, &info);
    if (result != 0) return -EIO;
    rtp_packet_t packet;
    int packet_bytes = rtp_packet_build(&packet, (uint8_t)call->payload, call->sequence, call->timestamp,
                                        call->ssrc, 0, payload, bytes, wire, sizeof(wire));
    if (packet_bytes <= 0) return -EIO;
    ++call->sequence;
    call->timestamp += (uint32_t)frame;
    result = pcm_media_send(call, TURBO_SIP_RTP, wire, (size_t)packet_bytes);
    if (result == 0) rtp_onsend(call->rtp, wire, packet_bytes);
    return result;
}

int turbo_sip_call_input_media(turbo_sip_call_t* call, turbo_sip_packet_kind_t kind,
                               const void* packet, size_t bytes)
{
    if (!call || !packet || !bytes || bytes > SIP_CLIENT_MEDIA_PACKET_BYTES) return -EINVAL;
    turbo_sip_client_t* client = call->client;
    int result = client_ready(client);
    if (result) return result;
    if (call->state != TURBO_SIP_CALL_ACTIVE) return -ENOTCONN;
    if (kind == TURBO_SIP_RTCP)
        return rtp_onreceived_rtcp(call->rtp, packet, (int)bytes) >= 0 ? 0 : -EPROTO;
    if (kind != TURBO_SIP_RTP || !call->can_recv) return -EINVAL;

    rtp_packet_t rtp;
    if (rtp_packet_parse(&rtp, packet, bytes) != 0) return -EPROTO;
    if (rtp.header.payload_type != call->payload || rtp.header.ssrc == call->ssrc ||
        (call->have_peer_ssrc && call->peer_ssrc != rtp.header.ssrc))
        return -ENOTSUP;
    if (rtp_onreceived(call->rtp, packet, (int)bytes) < 0) return -EPROTO;
    call->peer_ssrc = rtp.header.ssrc;
    call->have_peer_ssrc = 1;

    int16_t samples[SIP_CLIENT_AUDIO_MAX_SAMPLES];
    size_t size = sizeof(samples);
    if (turbo_audio_codec_decode(call->decoder, rtp.payload, rtp.payload_len, (uint8_t*)samples, &size) != 0)
        return -EPROTO;
    if (client->config.on_pcm) {
        client->in_callback = 1;
        client->config.on_pcm(client->config.context, call, samples, size / sizeof(samples[0]));
        client->in_callback = 0;
    }
    return 0;
}
