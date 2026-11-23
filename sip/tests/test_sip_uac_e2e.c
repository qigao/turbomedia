#include "cnet/cnet.h"
#include "core/platform.h"
#include "turbo_rtp_packet.h"
#include "rtp-payload.h"
#include "rtp.h"
#include "sip-agent.h"
#include "sip-registration.h"
#include "sip-uac-transaction.h"
#include "tinytest.h"
#include "turbo_audio_codec.h"
#include "turbo_rtp_ice.h"
#include "turbo_sdp.h"
#include <crypto/sha256.h>
#include <crypto/symmetric.h>

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { AUDIO_SAMPLES = 160, TEST_TIMEOUT_MS = 3000 };
static const char text_body[] = "SIP MESSAGE: \xe4\xbd\xa0\xe5\xa5\xbd";
static const char text_type[] = "text/plain; charset=utf-8";
static const char sdp_type[] = "application/sdp";

struct udp_socket {
    cnet_datagram socket;
    uint16_t port;
    unsigned sent;
    int (*receive)(const cnet_datagram_peer*, const cnet_receive_view*);
};

struct audio_endpoint {
    struct udp_socket udp;
    cnet_datagram_peer remote;
    void* encoder;
    void* decoder;
    turbo_audio_codec_t* pcmu_encoder;
    turbo_audio_codec_t* pcmu_decoder;
    unsigned received;
    uint16_t sequence;
    uint32_t ssrc;
    uint32_t timestamp;
    unsigned char pcmu[AUDIO_SAMPLES];
    int16_t pcm[AUDIO_SAMPLES];
};

struct ice_endpoint {
    sync_thread_t thread;
    sdp_session_t local;
    const sdp_media_t* remote;
    struct ice_endpoint* peer;
    atomic_int gathered, remote_ready, connected, got_rtp, got_rtcp, stopped, finished;
    int controlling, turn_port, result;
    uint32_t ssrc;
};

static struct {
    struct sip_agent_t* agent;
    sip_registration_t* registration;
    unsigned registration_replies;
    unsigned registration_requests;
    char registration_callid[80];
    struct sip_uac_transaction_t* request;
    struct sip_uac_transaction_t* bye;
    struct sip_dialog_t* dialog;
    struct udp_socket client;
    struct udp_socket peer;
    struct audio_endpoint client_audio;
    struct audio_endpoint peer_audio;
    struct ice_endpoint ice_client, ice_peer;
    int use_ice;
    /* SIP retransmission timers can call send on another thread. The polling
     * thread alone admits CNet operations and drains this copied packet slot. */
    sync_mutex_t outbound_lock;
    int lock_ready;
    unsigned char outbound[SIP_DATAGRAM_MAX_BYTES];
    size_t outbound_size;
    int io_status;
    char client_address[32];
    char from[64];
    char destination[64];
    char invite_call_id[80];
    char dialog_tag[32];
    uint32_t invite_cseq;
    unsigned messages;
    unsigned acks;
    unsigned byes;
    unsigned replies;
    int code;
    sdp_session_t offer;
    sdp_session_t answer;
} fixture;

static native_io_backend_kind backend_kind(void)
{
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    return NATIVE_IO_BACKEND_KQUEUE;
#else
    return NATIVE_IO_BACKEND_EPOLL;
#endif
}

static cnet_datagram_peer loopback(uint16_t port)
{
    return (cnet_datagram_peer){.family = CNET_DATAGRAM_ADDRESS_IPV4, .port = port,
                               .address = {127, 0, 0, 1}};
}

static void on_udp_send(void* user, cnet_datagram* socket, const cnet_datagram_peer* peer,
                        size_t size, int status, uint64_t tag)
{
    struct udp_socket* endpoint = user;
    (void)socket;
    (void)peer;
    (void)size;
    (void)tag;
    if (status == SALTS_OK)
        ++endpoint->sent;
    else if (fixture.io_status == SALTS_OK)
        fixture.io_status = status;
}

static void on_udp_receive(void* user, cnet_datagram* socket, const cnet_datagram_peer* peer,
                           const cnet_receive_view* view)
{
    struct udp_socket* endpoint = user;
    (void)socket;
    if (fixture.io_status == SALTS_OK)
        fixture.io_status = endpoint->receive(peer, view);
}

static void open_socket(struct udp_socket* endpoint,
                        int (*receive)(const cnet_datagram_peer*, const cnet_receive_view*))
{
    cnet_datagram_config config = CNET_DATAGRAM_CONFIG_INIT;
    endpoint->receive = receive;
    config.backend = backend_kind();
    config.host = "127.0.0.1";
    config.send_capacity = 4;
    config.request_capacity = 5;
    config.completion_batch_capacity = 5;
    config.max_datagram_bytes = SIP_DATAGRAM_MAX_BYTES;
    config.receive_buffer_bytes = SIP_DATAGRAM_MAX_BYTES;
    config.observer = (cnet_datagram_observer){on_udp_receive, on_udp_send, endpoint};
    check_equal(cnet_datagram_init(&endpoint->socket, &config), SALTS_OK);
    check_equal(cnet_datagram_port(&endpoint->socket, &endpoint->port), SALTS_OK);
    check(endpoint->port != 0);
    check_equal(cnet_datagram_receive(&endpoint->socket, 8), SALTS_OK);
}

static void close_socket(struct udp_socket* endpoint)
{
    if (!endpoint->socket.impl)
        return;
    int status = cnet_datagram_stop(&endpoint->socket, TEST_TIMEOUT_MS);
    int destroyed = cnet_datagram_destroy(&endpoint->socket);
    check_equal(status, SALTS_OK);
    check_equal(destroyed, SALTS_OK);
}

static void stop_transaction(struct sip_uac_transaction_t* transaction)
{
    if (!transaction)
        return;
    sync_mutex_lock(&transaction->locker);
    int linked = transaction->linked;
    if (linked) {
        if (transaction->status <= SIP_UAC_TRANSACTION_PROCEEDING) {
            sip_uac_transaction_timeout(transaction, 1);
        } else {
            sip_uac_stop_timer(fixture.agent, transaction, &transaction->timerd);
            sip_uac_transaction_timewait(transaction, 1);
        }
    }
    sync_mutex_unlock(&transaction->locker);
    uint64_t deadline = monotonic_ms() + TEST_TIMEOUT_MS;
    while (linked && monotonic_ms() < deadline) {
        sleep_ms(1);
        sync_mutex_lock(&transaction->locker);
        linked = transaction->linked;
        sync_mutex_unlock(&transaction->locker);
    }
    sip_uac_transaction_release(transaction);
    check_equal(linked, 0);
}

static void close_audio(struct audio_endpoint* audio)
{
    close_socket(&audio->udp);
    if (audio->encoder)
        rtp_payload_encode_destroy(audio->encoder);
    if (audio->decoder)
        rtp_payload_decode_destroy(audio->decoder);
    turbo_audio_codec_destroy(audio->pcmu_encoder);
    turbo_audio_codec_destroy(audio->pcmu_decoder);
}

static void tear_down(void)
{
    if (fixture.use_ice) {
        if (fixture.ice_client.thread) atomic_store(&fixture.ice_client.stopped, 1);
        if (fixture.ice_peer.thread) atomic_store(&fixture.ice_peer.stopped, 1);
        if (fixture.ice_client.thread) {
            sync_thread_join(&fixture.ice_client.thread);
            sync_thread_destroy(&fixture.ice_client.thread);
        }
        if (fixture.ice_peer.thread) {
            sync_thread_join(&fixture.ice_peer.thread);
            sync_thread_destroy(&fixture.ice_peer.thread);
        }
    }
    sip_registration_destroy(fixture.registration);
    stop_transaction(fixture.bye);
    stop_transaction(fixture.request);
    close_socket(&fixture.client);
    close_socket(&fixture.peer);
    close_audio(&fixture.client_audio);
    close_audio(&fixture.peer_audio);
    if (fixture.agent)
        sip_agent_destroy(fixture.agent);
    if (fixture.lock_ready)
        sync_mutex_destroy(&fixture.outbound_lock);
}

static void pump_until(const unsigned* counter, unsigned expected)
{
    const uint64_t deadline = monotonic_ms() + TEST_TIMEOUT_MS;
    struct udp_socket* sockets[] = {&fixture.client, &fixture.peer,
                                    &fixture.client_audio.udp, &fixture.peer_audio.udp};
    while (*counter < expected && fixture.io_status == SALTS_OK && monotonic_ms() < deadline) {
        sync_mutex_lock(&fixture.outbound_lock);
        if (fixture.outbound_size) {
            cnet_datagram_peer peer = loopback(fixture.peer.port);
            fixture.io_status = cnet_datagram_send(&fixture.client.socket, &peer, fixture.outbound,
                                                   fixture.outbound_size, 0);
            fixture.outbound_size = 0;
        }
        sync_mutex_unlock(&fixture.outbound_lock);
        for (size_t i = 0; i < sizeof(sockets) / sizeof(*sockets); ++i) {
            if (sockets[i]->socket.impl) {
                size_t events;
                check_equal(cnet_datagram_poll(&sockets[i]->socket, 0, &events), SALTS_OK);
            }
        }
        if (*counter < expected)
            sleep_ms(1);
    }
    check_equal(fixture.io_status, SALTS_OK);
    check_equal(*counter, expected);
}

static int udp_via(void* context, const char* destination, char protocol[16],
                   char local[128], char dns[128])
{
    (void)context;
    (void)destination;
    memcpy(protocol, "UDP", sizeof("UDP"));
    memcpy(local, fixture.client_address, strlen(fixture.client_address) + 1);
    memcpy(dns, fixture.client_address, strlen(fixture.client_address) + 1);
    return SALTS_OK;
}

static int queue_sip_send(void* context, const void* data, size_t bytes)
{
    (void)context;
    if (bytes > sizeof(fixture.outbound))
        return SALTS_EMSGSIZE;
    sync_mutex_lock(&fixture.outbound_lock);
    int status = fixture.outbound_size ? SALTS_ENOBUFS : SALTS_OK;
    if (status == SALTS_OK) {
        memcpy(fixture.outbound, data, bytes);
        fixture.outbound_size = bytes;
    }
    sync_mutex_unlock(&fixture.outbound_lock);
    return status;
}

static struct sip_transport_t transport = {udp_via, queue_sip_send};

static int client_receive(const cnet_datagram_peer* peer, const cnet_receive_view* view)
{
    if (peer->port != fixture.peer.port)
        return SALTS_EINVAL;
    int r = sip_agent_input_reply_datagram(fixture.agent, view->data, view->size);
    if (r == 0 && fixture.registration) ++fixture.registration_replies;
    return r;
}

static const sdp_codec_t* pcmu(const sdp_session_t* sdp)
{
    if (sdp->media_count != 1 || sdp->media[0].type != SDP_MEDIA_AUDIO || !sdp->media[0].port ||
        strcmp(sdp->media[0].protocol, "RTP/AVP") || sdp->media[0].direction != SDP_DIRECTION_SENDRECV)
        return NULL;
    for (int i = 0; i < sdp->media[0].codec_count; ++i) {
        const sdp_codec_t* codec = &sdp->media[0].codecs[i];
        if (codec->payload_type == 0 && !strcmp(codec->name, "PCMU") &&
            codec->clock_rate == 8000 && codec->channels == 1)
            return codec;
    }
    return NULL;
}

static int write_sdp(char* body, size_t capacity, uint16_t port)
{
    return snprintf(body, capacity,
                    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Audio\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                    "m=audio %u RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n", (unsigned)port);
}

static int ok_reply(const cnet_datagram_peer* peer, const struct sip_message_t* request, int invite)
{
    char answer[2048];
    char contact[80];
    unsigned char wire[SIP_DATAGRAM_MAX_BYTES];
    struct sip_message_t* reply = sip_message_create(SIP_MESSAGE_REPLY);
    if (!reply)
        return SALTS_ENOMEM;
    int status = sip_message_init3(reply, request, NULL);
    if (!status && invite) {
        snprintf(contact, sizeof(contact), "<%s>", fixture.destination);
        status = sip_message_add_header(reply, SIP_HEADER_CONTACT, contact);
        if (!status)
            status = sip_message_add_header(reply, "Content-Type", sdp_type);
        int bytes = fixture.use_ice ? sdp_generate(&fixture.ice_peer.local, answer, sizeof(answer)) :
            write_sdp(answer, sizeof(answer), fixture.peer_audio.udp.port);
        if (bytes <= 0 || (size_t)bytes >= sizeof(answer))
            status = SALTS_EINVAL;
        if (!status) {
            reply->payload = answer;
            reply->size = bytes;
            sip_sv_copy(&reply->to.tag, fixture.dialog_tag, sizeof(fixture.dialog_tag));
        }
    }
    if (!status) {
        reply->u.s.code = 200;
        reply->u.s.reason = vstr_from_cstr("OK");
        int bytes = sip_message_write(reply, wire, sizeof(wire));
        status = bytes > 0 && (size_t)bytes < sizeof(wire)
            ? cnet_datagram_send(&fixture.peer.socket, peer, wire, (size_t)bytes, 0) : SALTS_EINVAL;
    }
    sip_message_destroy(reply);
    return status;
}

static void test_digest_hex(const char* input, char hex[65], int sha256)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t bytes[32];
    size_t len = sha256 ? 32 : 16;
    check_equal(sha256 ? salts_crypto_sha256(input, strlen(input), bytes)
                       : salts_crypto_md5(input, strlen(input), bytes), SALTS_CRYPTO_OK);
    for (size_t i = 0; i < len; ++i) {
        hex[2 * i] = digits[bytes[i] >> 4];
        hex[2 * i + 1] = digits[bytes[i] & 15];
    }
    hex[2 * len] = '\0';
}

static int registrar_auth(const struct sip_message_t* request, const char* field,
                          const char* nonce, unsigned nc)
{
    char auth[512], target[64], a1[65], a2[65], answer[65], input[256], expected[96];
    const vstr* value = sip_message_get_header_by_name(request, field);
    if (!value || sip_sv_copy(value, auth, sizeof(auth)) >= sizeof(auth)) return 0;
    snprintf(target, sizeof(target), "sip:127.0.0.1:%u", (unsigned)fixture.peer.port);
    snprintf(expected, sizeof(expected), "uri=\"%s\"", target);
    if (!strstr(auth, expected) || !strstr(auth, nc ? "algorithm=SHA-256" : "algorithm=MD5")) return 0;
    test_digest_hex(nc ? "alice:voice:example-pass" : "alice:proxy:example-pass", a1, nc != 0);
    snprintf(input, sizeof(input), "REGISTER:%s", target);
    test_digest_hex(input, a2, nc != 0);
    if (nc) {
        const char* cnonce = strstr(auth, "cnonce=\"");
        if (!cnonce || strlen(cnonce) < 41 || cnonce[40] != '"') return 0;
        snprintf(expected, sizeof(expected), "nc=%08x", nc);
        if (!strstr(auth, expected)) return 0;
        snprintf(input, sizeof(input), "%s:%s:%08x:%.32s:auth:%s", a1, nonce, nc, cnonce + 8, a2);
    } else {
        snprintf(input, sizeof(input), "%s:%s:%s", a1, nonce, a2);
    }
    test_digest_hex(input, answer, nc != 0);
    snprintf(expected, sizeof(expected), "response=\"%s\"", answer);
    return strstr(auth, expected) != NULL;
}

static int registrar_reply(const cnet_datagram_peer* peer, struct sip_message_t* request)
{
    unsigned step = ++fixture.registration_requests;
    if (step == 1)
        sip_sv_copy(&request->callid, fixture.registration_callid, sizeof(fixture.registration_callid));
    if (step > 6 || request->cseq.id != step ||
        sip_sv_compare_cstr(&request->callid, fixture.registration_callid) ||
        (!!sip_message_get_header_by_name(request, "Authorization") != (step > 1)) ||
        (step > 1 && !registrar_auth(request, "Authorization",
                                     step == 6 ? "renewed" : "registrar", step == 6 ? 1 : step - 1)) ||
        (step > 2 && !registrar_auth(request, "Proxy-Authorization", "edge", 0)) ||
        sip_sv_compare_cstr(sip_message_get_header_by_name(request, "Expires"), step > 3 ? "15" : "10"))
        return SALTS_EINVAL;

    const int codes[] = {401, 407, 423, 200, 401, 200};
    const char* fields[] = {"WWW-Authenticate", "Proxy-Authenticate", "Min-Expires",
                            "Contact", "WWW-Authenticate", "Contact"};
    char contact[128];
    snprintf(contact, sizeof(contact), "<%s>;expires=20", fixture.from);
    const char* values[] = {"Digest realm=\"voice\", nonce=\"registrar\", algorithm=SHA-256, qop=\"auth,auth-int\"",
                            "Digest realm=\"proxy\", nonce=\"edge\"", "15", contact,
                            "Digest realm=\"voice\", nonce=\"renewed\", algorithm=SHA-256, qop=\"auth\", stale=true",
                            "<sip:alice@203.0.113.44:5078>"};
    struct sip_message_t* response = sip_message_create(SIP_MESSAGE_REPLY);
    if (!response) return SALTS_ENOMEM;
    int r = step == 6 ? sip_agent_set_rport(request, "203.0.113.44", 5078) : 0;
    if (r == 0) r = sip_message_init3(response, request, NULL);
    if (r == 0 && step == 1)
        r = sip_message_add_header(response, "WWW-Authenticate", "Digest realm=\"voice\", nonce=\"old\"");
    if (r == 0 && step == 6)
        r = sip_message_add_header(response, "Contact", "<sip:alice@203.0.113.44:5090>;expires=300");
    if (r == 0 && step == 6) r = sip_message_add_header(response, "Expires", "20");
    if (r == 0) r = sip_message_add_header(response, fields[step - 1], values[step - 1]);
    if (r == 0) {
        response->u.s.code = codes[step - 1];
        response->u.s.reason = vstr_from_cstr("Registration");
        uint8_t wire[SIP_DATAGRAM_MAX_BYTES];
        int bytes = sip_message_write(response, wire, sizeof(wire));
        r = bytes > 0 && (size_t)bytes < sizeof(wire)
            ? cnet_datagram_send(&fixture.peer.socket, peer, wire, (size_t)bytes, 0) : SALTS_EINVAL;
    }
    sip_message_destroy(response);
    return r;
}

static int peer_request(const cnet_datagram_peer* peer, struct sip_message_t* msg)
{
    if (msg->mode != SIP_MESSAGE_REQUEST)
        return SALTS_EINVAL;
    if (sip_message_isregister(msg))
        return registrar_reply(peer, msg);
    if (!sip_sv_compare_cstr(&msg->u.c.method, "MESSAGE")) {
        if (sip_sv_compare_cstr(sip_message_get_header_by_name(msg, "Content-Type"), text_type) ||
            msg->size != sizeof(text_body) - 1 || memcmp(msg->payload, text_body, sizeof(text_body) - 1))
            return SALTS_EINVAL;
        ++fixture.messages;
        return ok_reply(peer, msg, 0);
    }
    if (sip_message_isinvite(msg)) {
        if (sip_sv_compare_cstr(sip_message_get_header_by_name(msg, "Content-Type"), sdp_type) ||
            sdp_parse(msg->payload, (size_t)msg->size, &fixture.offer) || !pcmu(&fixture.offer) ||
            sip_sv_copy(&msg->callid, fixture.invite_call_id, sizeof(fixture.invite_call_id)) >= sizeof(fixture.invite_call_id))
            return SALTS_EINVAL;
        fixture.invite_cseq = msg->cseq.id;
        return ok_reply(peer, msg, 1);
    }
    if (sip_sv_compare_cstr(&msg->callid, fixture.invite_call_id) ||
        sip_sv_compare_cstr(&msg->to.tag, fixture.dialog_tag) || msg->size)
        return SALTS_EINVAL;
    if (sip_message_isack(msg) && msg->cseq.id == fixture.invite_cseq &&
        !sip_message_get_header_by_name(msg, "Content-Type")) {
        ++fixture.acks;
        return SALTS_OK;
    }
    if (sip_message_isbye(msg) && fixture.acks && msg->cseq.id == fixture.invite_cseq + 1) {
        ++fixture.byes;
        return ok_reply(peer, msg, 0);
    }
    return SALTS_EINVAL;
}

static int peer_receive(const cnet_datagram_peer* peer, const cnet_receive_view* view)
{
    if (peer->port != fixture.client.port)
        return SALTS_EINVAL;
    struct sip_message_t* request = sip_message_parse_datagram(view->data, view->size);
    if (!request)
        return SALTS_EINVAL;
    int status = peer_request(peer, request);
    sip_message_destroy(request);
    return status;
}

static void set_up(void)
{
    struct sip_uas_handler_t handlers = {0};
    memset(&fixture, 0, sizeof(fixture));
    fixture.agent = sip_agent_create(&handlers);
    check(fixture.agent != NULL);
    sync_mutex_init(&fixture.outbound_lock);
    fixture.lock_ready = 1;
    open_socket(&fixture.client, client_receive);
    open_socket(&fixture.peer, peer_receive);
    snprintf(fixture.client_address, sizeof(fixture.client_address), "127.0.0.1:%u", (unsigned)fixture.client.port);
    snprintf(fixture.from, sizeof(fixture.from), "sip:alice@%s", fixture.client_address);
    snprintf(fixture.destination, sizeof(fixture.destination), "sip:bob@127.0.0.1:%u", (unsigned)fixture.peer.port);
}

static int on_reply(void* context, const struct sip_message_t* reply,
                    struct sip_uac_transaction_t* transaction, int code)
{
    (void)context;
    (void)transaction;
    if (reply) {
        ++fixture.replies;
        fixture.code = code;
    }
    return SALTS_OK;
}

static int on_invite(void* context, const struct sip_message_t* reply, struct sip_uac_transaction_t* transaction,
                     struct sip_dialog_t* dialog, const vstr* id, int code)
{
    (void)id;
    if (!reply)
        return SALTS_OK;
    if (code != 200 || !dialog || sip_sv_compare_cstr(sip_message_get_header_by_name(reply, "Content-Type"), sdp_type) ||
        sdp_parse(reply->payload, (size_t)reply->size, &fixture.answer) || !pcmu(&fixture.answer))
        return SALTS_EINVAL;
    fixture.dialog = dialog; /* The INVITE transaction still owns the dialog. */
    return on_reply(context, reply, transaction, code);
}

static void* allocate_packet(void* context, int bytes)
{
    (void)context;
    return malloc((size_t)bytes);
}

static void release_packet(void* context, void* packet)
{
    (void)context;
    free(packet);
}

static int send_audio_packet(void* context, const void* packet, int bytes, uint32_t timestamp, int flags)
{
    struct audio_endpoint* audio = context;
    (void)timestamp;
    (void)flags;
    return cnet_datagram_send(&audio->udp.socket, &audio->remote, packet, (size_t)bytes, 0);
}

static int decode_audio_packet(void* context, const void* packet, int bytes, uint32_t timestamp, int flags)
{
    struct audio_endpoint* audio = context;
    if (bytes != sizeof(audio->pcmu) || flags)
        return SALTS_EINVAL;
    size_t pcm_size = sizeof(audio->pcm);
    if (turbo_audio_codec_decode(audio->pcmu_decoder, packet, (size_t)bytes,
                                 (uint8_t*)audio->pcm, &pcm_size) ||
        pcm_size != sizeof(audio->pcm))
        return SALTS_EINVAL;
    memcpy(audio->pcmu, packet, sizeof(audio->pcmu));
    audio->timestamp = timestamp;
    ++audio->received;
    return SALTS_OK;
}

static int audio_receive(struct audio_endpoint* audio, const cnet_datagram_peer* peer, const cnet_receive_view* view)
{
    rtp_packet_t parsed;
    if (peer->port != audio->remote.port || !audio->decoder ||
        rtp_packet_parse(&parsed, view->data, (int)view->size) || parsed.header.version != RTP_VERSION || parsed.header.payload_type != 0)
        return SALTS_EINVAL;
    audio->sequence = (uint16_t)parsed.header.sequence;
    audio->ssrc = parsed.header.ssrc;
    return rtp_payload_decode_input(audio->decoder, view->data, (int)view->size) == 1 ? SALTS_OK : SALTS_EINVAL;
}

static int client_audio_receive(const cnet_datagram_peer* peer, const cnet_receive_view* view)
{
    return audio_receive(&fixture.client_audio, peer, view);
}

static int peer_audio_receive(const cnet_datagram_peer* peer, const cnet_receive_view* view)
{
    return audio_receive(&fixture.peer_audio, peer, view);
}

static void start_audio(struct audio_endpoint* audio, const sdp_session_t* remote_sdp, uint16_t seq, uint32_t ssrc)
{
    static struct rtp_payload_t sender = {allocate_packet, release_packet, send_audio_packet};
    static struct rtp_payload_t receiver = {allocate_packet, release_packet, decode_audio_packet};
    const sdp_codec_t* codec = pcmu(remote_sdp);
    check(codec != NULL);
    const turbo_audio_codec_config_t config = {
        .sample_rate = 8000, .channels = 1, .frame_size_ms = 20
    };
    audio->pcmu_encoder = turbo_audio_codec_create_encoder("pcmu", &config);
    audio->pcmu_decoder = turbo_audio_codec_create_decoder("pcmu", &config);
    check(audio->pcmu_encoder != NULL);
    check(audio->pcmu_decoder != NULL);
    audio->remote = loopback(remote_sdp->media[0].port);
    audio->encoder = rtp_payload_encode_create(codec->payload_type, codec->name, seq, ssrc, &sender, audio);
    audio->decoder = rtp_payload_decode_create(codec->payload_type, codec->name, &receiver, audio);
    check(audio->encoder != NULL);
    check(audio->decoder != NULL);
}

static void send_pcm(struct audio_endpoint* audio, const int16_t pcm[AUDIO_SAMPLES],
                     uint8_t encoded[AUDIO_SAMPLES], uint32_t timestamp)
{
    size_t size = AUDIO_SAMPLES;
    turbo_encoded_frame_t info = {0};
    check_equal(turbo_audio_codec_encode(audio->pcmu_encoder, (const uint8_t*)pcm,
                AUDIO_SAMPLES * sizeof(*pcm), encoded, &size, &info), 0);
    check_equal(size, (size_t)AUDIO_SAMPLES);
    check_equal(rtp_payload_encode_input(audio->encoder, encoded, (int)size, timestamp), SALTS_OK);
}

static void ice_media_receive(void* context, turbo_rtp_ice_packet_t kind,
                              const void* bytes, size_t size)
{
    struct ice_endpoint* endpoint = context;
    rtp_packet_t parsed;
    if (kind == TURBO_RTP_ICE_RTP &&
        rtp_packet_parse(&parsed, bytes, size) == 0 && parsed.header.ssrc == endpoint->peer->ssrc &&
        parsed.header.payload_type == 0 && parsed.payload_len == AUDIO_SAMPLES) {
        int valid = 1;
        for (size_t i = 0; i < parsed.payload_len; ++i)
            valid &= parsed.payload[i] == 0xff;
        atomic_store(&endpoint->got_rtp, valid);
    }
    const uint8_t* packet = bytes;
    if (kind == TURBO_RTP_ICE_RTCP && size >= 8 && packet[1] == RTCP_SR) {
        uint32_t ssrc = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) |
            ((uint32_t)packet[6] << 8) | packet[7];
        atomic_store(&endpoint->got_rtcp, ssrc == endpoint->peer->ssrc);
    }
}

static int ice_wait_for(struct ice_endpoint* endpoint, const atomic_int* flag,
                        turbo_rtp_ice_t* transport)
{
    uint64_t deadline = monotonic_ms() + 6000;
    while (!atomic_load(flag) && !atomic_load(&endpoint->stopped) && monotonic_ms() < deadline) {
        if (transport) {
            int r = turbo_rtp_ice_poll(transport, 10);
            if (r != 0) return r;
        } else {
            sleep_ms(1);
        }
    }
    return atomic_load(flag) ? 0 : SALTS_ETIMEDOUT;
}

static int ice_send_media(struct ice_endpoint* endpoint, turbo_rtp_ice_t* transport)
{
    const turbo_audio_codec_config_t config = {.sample_rate = 8000, .channels = 1, .frame_size_ms = 20};
    turbo_audio_codec_t* encoder = turbo_audio_codec_create_encoder("pcmu", &config);
    if (!encoder) return SALTS_ENOMEM;
    int16_t pcm[AUDIO_SAMPLES] = {0};
    uint8_t payload[AUDIO_SAMPLES], wire[256];
    size_t payload_size = sizeof(payload);
    turbo_encoded_frame_t info = {0};
    int r = turbo_audio_codec_encode(encoder, (const uint8_t*)pcm, sizeof(pcm),
                                     payload, &payload_size, &info);
    turbo_audio_codec_destroy(encoder);
    if (r != 0) return r;
    rtp_packet_t packet;
    int bytes = rtp_packet_build(&packet, 0, 1, 8000, endpoint->ssrc, 0,
                                 payload, payload_size, wire, sizeof(wire));
    if (bytes < 0) return SALTS_EPROTO;
    r = turbo_rtp_ice_send(transport, TURBO_RTP_ICE_RTP, wire, (size_t)bytes);
    if (r == 0) r = ice_wait_for(endpoint, &endpoint->got_rtp, transport);
    if (r == 0) r = ice_wait_for(endpoint, &endpoint->peer->got_rtp, transport);
    if (r != 0) return r;

    struct rtp_event_t events = {0};
    void* rtp = rtp_create(&events, NULL, endpoint->ssrc, 8000, 8000, 64000, 1);
    if (!rtp) return SALTS_ENOMEM;
    bytes = rtp_rtcp_report(rtp, wire, sizeof(wire));
    rtp_destroy(rtp);
    if (bytes < 4) return SALTS_EPROTO;
    r = turbo_rtp_ice_send(transport, TURBO_RTP_ICE_RTCP, wire, (size_t)bytes);
    if (r == 0) r = ice_wait_for(endpoint, &endpoint->got_rtcp, transport);
    return r;
}

static void ice_worker(void* context)
{
    struct ice_endpoint* endpoint = context;
    ice_config_t config = ice_default_config();
    config.is_controlling = endpoint->controlling;
    config.allow_loopback = 1;
    config.gathering_timeout_ms = 2000;
    config.connectivity_timeout_ms = endpoint->turn_port ? 20000 : 5000;
    if (endpoint->turn_port) {
        config.turn_server_count = 1;
        snprintf(config.turn_servers[0].url, sizeof(config.turn_servers[0].url),
                 "turn:127.0.0.1:%d", endpoint->turn_port);
        strcpy(config.turn_servers[0].username, "voice-test");
        strcpy(config.turn_servers[0].credential, "loopback");
    }
    turbo_rtp_ice_t* transport = turbo_rtp_ice_create(&config, ice_media_receive, endpoint);
    int r = transport ? turbo_rtp_ice_gather(transport) : SALTS_ENOMEM;
    if (r == 0) {
        sdp_session_init(&endpoint->local);
        sdp_media_t* media = sdp_add_audio(&endpoint->local, NULL, SDP_DIRECTION_SENDRECV);
        const sdp_codec_t codec = {.name = "PCMU", .payload_type = 0, .clock_rate = 8000, .channels = 1};
        if (!media || sdp_media_add_codec(media, &codec) != 0) r = SALTS_EPROTO;
        else {
            strcpy(media->protocol, "RTP/AVP");
            r = turbo_rtp_ice_write_local(transport, media);
        }
        if (r == 0 && endpoint->turn_port) {
            /* Offer only relay candidates so host reachability cannot mask a TURN failure. */
            int relay_count = 0;
            for (int i = 0; i < media->candidate_count; ++i) {
                if (!strcmp(media->candidates[i].type, "relay"))
                    media->candidates[relay_count++] = media->candidates[i];
            }
            media->candidate_count = relay_count;
            if (relay_count == 0) r = SALTS_EPROTO;
        }
    }
    if (r == 0) {
        atomic_store(&endpoint->gathered, 1);
        r = ice_wait_for(endpoint, &endpoint->remote_ready, NULL);
    }
    if (r == 0) {
        sdp_media_t without_mux = *endpoint->remote;
        without_mux.rtcp_mux = 0;
        if (turbo_rtp_ice_set_remote(transport, &without_mux) != SALTS_ENOTSUP) r = SALTS_EPROTO;
    }
    if (r == 0) r = turbo_rtp_ice_set_remote(transport, endpoint->remote);
    if (r == 0) r = turbo_rtp_ice_connect(transport);
    if (r == 0 && endpoint->turn_port) {
        ice_candidate_t local, remote;
        r = turbo_rtp_ice_get_selected_pair(transport, &local, &remote);
        if (r == 0 && local.type != ICE_CANDIDATE_TYPE_RELAY &&
            remote.type != ICE_CANDIDATE_TYPE_RELAY) r = SALTS_EPROTO;
    }
    if (r == 0) {
        atomic_store(&endpoint->connected, 1);
        r = ice_wait_for(endpoint, &endpoint->peer->connected, transport);
    }
    if (r == 0) r = ice_send_media(endpoint, transport);
    int cleanup = turbo_rtp_ice_destroy(&transport, TEST_TIMEOUT_MS);
    endpoint->result = r ? r : cleanup;
    atomic_store(&endpoint->finished, 1);
}

static void start_ice_endpoint(struct ice_endpoint* endpoint, struct ice_endpoint* peer,
                               int controlling, int port, uint32_t ssrc)
{
    endpoint->peer = peer;
    endpoint->controlling = controlling;
    endpoint->turn_port = port;
    endpoint->ssrc = ssrc;
    atomic_init(&endpoint->gathered, 0);
    atomic_init(&endpoint->remote_ready, 0);
    atomic_init(&endpoint->connected, 0);
    atomic_init(&endpoint->got_rtp, 0);
    atomic_init(&endpoint->got_rtcp, 0);
    atomic_init(&endpoint->stopped, 0);
    atomic_init(&endpoint->finished, 0);
    check_equal(sync_thread_create(&endpoint->thread, ice_worker, endpoint), 0);
}

static void wait_ice_test(const struct ice_endpoint* endpoint, const atomic_int* ready)
{
    uint64_t deadline = monotonic_ms() + (endpoint->turn_port ? 28000 : 8000);
    while (!atomic_load(ready) && !atomic_load(&endpoint->finished) && monotonic_ms() < deadline)
        sleep_ms(1);
    if (atomic_load(&endpoint->finished)) check_equal(endpoint->result, 0);
    check_equal(atomic_load(ready), 1);
}

spec("SIP UAC loopback") {
    before_each() { set_up(); }
    after_each() { tear_down(); }

    it("carries an ICE offer and answer over SIP and exchanges RTP and RTCP on the selected path") {
        const char* port = getenv("TURBO_MEDIA_TEST_TURN_PORT");
        char* end = NULL;
        long parsed = port ? strtol(port, &end, 10) : 0;
        check(!port || (end != port && !*end && parsed > 0 && parsed <= UINT16_MAX));
        int turn_port = (int)parsed;
        fixture.use_ice = 1;
        start_ice_endpoint(&fixture.ice_client, &fixture.ice_peer, 1, turn_port, 1001);
        start_ice_endpoint(&fixture.ice_peer, &fixture.ice_client, 0, turn_port, 2002);
        wait_ice_test(&fixture.ice_client, &fixture.ice_client.gathered);
        wait_ice_test(&fixture.ice_peer, &fixture.ice_peer.gathered);
        check_not_equal(fixture.ice_client.local.media[0].port, fixture.client.port);
        check_not_equal(fixture.ice_peer.local.media[0].port, fixture.peer.port);

        char body[2048];
        int size = sdp_generate(&fixture.ice_client.local, body, sizeof(body));
        check(size > 0);
        fixture.request = sip_uac_invite(fixture.agent, fixture.from, fixture.destination, on_invite, &fixture);
        check(fixture.request != NULL);
        check_equal(sip_uac_add_header(fixture.request, "Content-Type", sdp_type), 0);
        check_equal(sip_uac_send(fixture.request, body, size, &transport, &fixture), 0);
        pump_until(&fixture.replies, 1);
        check_equal(fixture.code, 200);
        check_equal(sip_uac_ack(fixture.request, NULL, 0, NULL), 0);
        pump_until(&fixture.acks, 1);
        check_equal(fixture.offer.media[0].connection_addr, fixture.ice_client.local.media[0].connection_addr);
        check_equal(fixture.answer.media[0].connection_addr, fixture.ice_peer.local.media[0].connection_addr);
        fixture.ice_client.remote = &fixture.answer.media[0];
        fixture.ice_peer.remote = &fixture.offer.media[0];
        atomic_store(&fixture.ice_client.remote_ready, 1);
        atomic_store(&fixture.ice_peer.remote_ready, 1);
        wait_ice_test(&fixture.ice_client, &fixture.ice_client.finished);
        wait_ice_test(&fixture.ice_peer, &fixture.ice_peer.finished);
        fixture.bye = sip_uac_bye(fixture.agent, fixture.dialog, on_reply, &fixture);
        check(fixture.bye != NULL);
        check_equal(sip_uac_send(fixture.bye, NULL, 0, &transport, &fixture), 0);
        pump_until(&fixture.replies, 2);
        check_equal(fixture.byes, 1u);
    };

    it("registers through Digest challenges and refreshes the negotiated contact lease") {
        sip_registration_config_t config = {
            .agent = fixture.agent, .transport = &transport, .transport_context = &fixture,
            .address_of_record = fixture.from, .registrar = fixture.destination,
            .username = "alice", .password = "example-pass", .expires_seconds = 10
        };
        fixture.registration = sip_registration_create(&config);
        check(fixture.registration != NULL);
        sip_registration_status_t status;
        check_equal(sip_registration_poll(fixture.registration, 1000, &status), 0);
        check_equal(status.state, SIP_REGISTRATION_PENDING);
        for (unsigned i = 1; i <= 4; ++i) {
            pump_until(&fixture.registration_replies, i);
            check_equal(sip_registration_poll(fixture.registration, 1000, &status), 0);
            check_equal(status.error, 0);
        }
        check_equal(status.state, SIP_REGISTRATION_ACTIVE);
        check_equal(status.sip_code, 200);
        check_equal(status.expires_at_ms, UINT64_C(21000));
        check_equal(status.refresh_at_ms, UINT64_C(17000));
        check_equal(sip_registration_poll(fixture.registration, 16999, &status), 0);
        check_equal(status.refresh_at_ms, UINT64_C(17000));
        check_equal(sip_registration_poll(fixture.registration, 17000, &status), 0);
        for (unsigned i = 5; i <= 6; ++i) {
            pump_until(&fixture.registration_replies, i);
            check_equal(sip_registration_poll(fixture.registration, 17000, &status), 0);
            check_equal(status.error, 0);
        }
        check_equal(status.state, SIP_REGISTRATION_ACTIVE);
        check_equal(status.expires_at_ms, UINT64_C(37000));
    };

    it("delivers a UTF-8 MESSAGE to the UDP peer and receives its 200 OK") {
        fixture.request = sip_uac_message(fixture.agent, fixture.from, fixture.destination, on_reply, &fixture);
        check(fixture.request != NULL);
        check_equal(sip_uac_add_header(fixture.request, "Content-Type", text_type), SALTS_OK);
        check_equal(sip_uac_send(fixture.request, text_body, sizeof(text_body) - 1, &transport, &fixture), SALTS_OK);
        pump_until(&fixture.replies, 1);
        pump_until(&fixture.peer.sent, 1);
        check_equal(fixture.messages, 1u);
        check_equal(fixture.code, 200);
    };

    it("exchanges PCM over PCMU RTP on ports separate from SIP INVITE, ACK and BYE") {
        char offer[256];
        unsigned char client_frame[AUDIO_SAMPLES];
        unsigned char peer_frame[AUDIO_SAMPLES];
        int16_t client_pcm[AUDIO_SAMPLES], peer_pcm[AUDIO_SAMPLES];
        open_socket(&fixture.client_audio.udp, client_audio_receive);
        open_socket(&fixture.peer_audio.udp, peer_audio_receive);
        check(fixture.client_audio.udp.port != fixture.client.port);
        check(fixture.peer_audio.udp.port != fixture.peer.port);
        int bytes = write_sdp(offer, sizeof(offer), fixture.client_audio.udp.port);
        check(bytes > 0 && (size_t)bytes < sizeof(offer));
        fixture.request = sip_uac_invite(fixture.agent, fixture.from, fixture.destination, on_invite, &fixture);
        check(fixture.request != NULL);
        check_equal(sip_uac_add_header(fixture.request, "Content-Type", sdp_type), SALTS_OK);
        check_equal(sip_uac_send(fixture.request, offer, bytes, &transport, &fixture), SALTS_OK);
        pump_until(&fixture.replies, 1);
        check_equal(fixture.code, 200);
        check_equal(sip_uac_ack(fixture.request, NULL, 0, NULL), SALTS_OK);
        pump_until(&fixture.acks, 1);

        start_audio(&fixture.client_audio, &fixture.answer, 300, 1001);
        start_audio(&fixture.peer_audio, &fixture.offer, 400, 2002);
        for (unsigned i = 0; i < AUDIO_SAMPLES; ++i) {
            client_pcm[i] = (int16_t)(((int)i - 80) * 100);
            peer_pcm[i] = (int16_t)((40 - (int)i) * 50);
        }
        send_pcm(&fixture.client_audio, client_pcm, client_frame, 8000);
        send_pcm(&fixture.peer_audio, peer_pcm, peer_frame, 16000);
        pump_until(&fixture.client_audio.received, 1);
        pump_until(&fixture.peer_audio.received, 1);
        check_equal(fixture.client_audio.pcmu, peer_frame, sizeof(peer_frame));
        check_equal(fixture.peer_audio.pcmu, client_frame, sizeof(client_frame));
        for (unsigned i = 0; i < AUDIO_SAMPLES; ++i) {
            check(abs(fixture.client_audio.pcm[i] - peer_pcm[i]) <= 256);
            check(abs(fixture.peer_audio.pcm[i] - client_pcm[i]) <= 256);
        }
        check_equal((unsigned)fixture.client_audio.sequence, 400u);
        check_equal((unsigned)fixture.peer_audio.sequence, 300u);
        check_equal(fixture.client_audio.ssrc, 2002u);
        check_equal(fixture.peer_audio.ssrc, 1001u);
        check_equal(fixture.client_audio.timestamp, 16000u);
        check_equal(fixture.peer_audio.timestamp, 8000u);

        fixture.bye = sip_uac_bye(fixture.agent, fixture.dialog, on_reply, &fixture);
        check(fixture.bye != NULL);
        check_equal(sip_uac_send(fixture.bye, NULL, 0, &transport, &fixture), SALTS_OK);
        pump_until(&fixture.replies, 2);
        check_equal(fixture.byes, 1u);
        check_equal(fixture.code, 200);
    };
}
