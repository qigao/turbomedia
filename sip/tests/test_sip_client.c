#include "tinytest.h"

#include <turbo_sip_client.h>
#include <sip-message.h>
#include <core/fmt.h>
#include <errno.h>
#include <string.h>

enum { TEST_PACKETS = 16, TEST_AUDIO_SAMPLES = 160 };

struct datagram {
    size_t size;
    unsigned char data[SIP_DATAGRAM_MAX_BYTES];
};

struct endpoint {
    turbo_sip_client_t* client;
    turbo_sip_call_t* call;
    turbo_sip_call_state_t state;
    sip_registration_state_t registration;
    int code;
    int send_error;
    unsigned sends;
    unsigned rtp_count;
    unsigned rtcp_count;
    unsigned active_count;
    unsigned incoming_messages;
    unsigned message_results;
    unsigned media_closes;
    unsigned media_starts;
    uint16_t sip_port;
    uint16_t media_port;
    struct datagram outbound[TEST_PACKETS];
    unsigned outbound_count;
    unsigned char media[2048];
    size_t media_size;
    int16_t pcm[TEST_AUDIO_SAMPLES];
    size_t pcm_count;
};

static struct endpoint caller, peer;
static struct sip_message_t* requests[4];
static struct datagram captured[4];

static int via(void* context, const char* destination, char protocol[16], char local[128], char dns[128])
{
    struct endpoint* endpoint = context;
    (void)destination;
    strcpy(protocol, "UDP");
    fmt(local, 128, "127.0.0.1:{}", (unsigned)endpoint->sip_port);
    strcpy(dns, local);
    return 0;
}

static int send_packet(void* context, const void* data, size_t bytes)
{
    struct endpoint* endpoint = context;
    ++endpoint->sends;
    if (endpoint->send_error) return endpoint->send_error;
    if (endpoint->outbound_count == TEST_PACKETS || bytes > SIP_DATAGRAM_MAX_BYTES) return -ENOSPC;
    struct datagram* packet = &endpoint->outbound[endpoint->outbound_count++];
    packet->size = bytes;
    memcpy(packet->data, data, bytes);
    return 0;
}

static int send_reply(void* context, const vstr* protocol, const vstr* sent_by,
                      const vstr* received, int rport, const void* data, int bytes)
{
    (void)protocol; (void)sent_by; (void)received; (void)rport;
    return send_packet(context, data, (size_t)bytes);
}

static int prepare_media(void* context, turbo_sip_call_t* call, int offerer, sdp_media_t* local)
{
    struct endpoint* endpoint = context;
    (void)call; (void)offerer;
    local->port = endpoint->media_port;
    local->rtcp_mux = 1;
    strcpy(local->connection_addr, "127.0.0.1");
    return 0;
}

static int start_media(void* context, turbo_sip_call_t* call, const sdp_media_t* local, const sdp_media_t* remote)
{
    struct endpoint* endpoint = context;
    (void)call;
    if (local->port != endpoint->media_port || !remote->port ||
        remote->port == local->port || remote->port == endpoint->sip_port)
        return -EINVAL;
    ++endpoint->media_starts;
    return 0;
}

static int send_media(void* context, turbo_sip_call_t* call, turbo_sip_packet_kind_t kind, const void* packet, size_t bytes)
{
    struct endpoint* endpoint = context;
    (void)call;
    if (bytes > sizeof(endpoint->media)) return -E2BIG;
    if (kind == TURBO_SIP_RTP) ++endpoint->rtp_count;
    else ++endpoint->rtcp_count;
    endpoint->media_size = bytes;
    memcpy(endpoint->media, packet, bytes);
    return 0;
}

static void close_media(void* context, turbo_sip_call_t* call)
{
    struct endpoint* endpoint = context;
    (void)call;
    ++endpoint->media_closes;
}

static void on_event(void* context, const turbo_sip_event_t* event)
{
    struct endpoint* endpoint = context;
    endpoint->code = event->sip_code;
    if (event->type == TURBO_SIP_CALL_CHANGED) {
        endpoint->call = event->call;
        endpoint->state = event->call_state;
        if (endpoint->state == TURBO_SIP_CALL_ACTIVE) ++endpoint->active_count;
    } else if (event->type == TURBO_SIP_REGISTRATION) {
        endpoint->registration = event->registration.state;
    } else if (event->type == TURBO_SIP_MESSAGE_RECEIVED) {
        const unsigned char body[] = {0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD};
        check_equal(event->body.len, sizeof(body));
        check_equal(event->body.data, body, sizeof(body));
        ++endpoint->incoming_messages;
    } else if (event->type == TURBO_SIP_MESSAGE_RESULT) {
        ++endpoint->message_results;
    }
}

static void on_pcm(void* context, turbo_sip_call_t* call, const int16_t* pcm, size_t count)
{
    struct endpoint* endpoint = context;
    (void)call;
    check_equal(count, (size_t)TEST_AUDIO_SAMPLES);
    endpoint->pcm_count = count;
    memcpy(endpoint->pcm, pcm, count * sizeof(*pcm));
}

static void start_endpoint(struct endpoint* endpoint, const char* aor, const char* contact, uint16_t sip, uint16_t media)
{
    endpoint->sip_port = sip;
    endpoint->media_port = media;
    const turbo_sip_client_config_t config = {
        .address_of_record = aor, .contact = contact,
        .registrar = "sip:127.0.0.1:5070", .username = "test-user", .password = "test-password",
        .signaling = {.request = {via, send_packet}, .reply = send_reply, .context = endpoint},
        .media = {prepare_media, start_media, send_media, close_media, endpoint},
        .on_event = on_event, .on_pcm = on_pcm, .context = endpoint
    };
    endpoint->client = turbo_sip_client_create(&config);
    check_not_null(endpoint->client);
    check_equal(turbo_sip_client_poll(endpoint->client, 1000), 0);
}

static struct datagram pop_packet(struct endpoint* endpoint)
{
    check(endpoint->outbound_count > 0);
    struct datagram packet = endpoint->outbound[0];
    --endpoint->outbound_count;
    memmove(endpoint->outbound, endpoint->outbound + 1, endpoint->outbound_count * sizeof(packet));
    return packet;
}

static void transfer(struct endpoint* source, struct endpoint* target)
{
    struct datagram packet = pop_packet(source);
    check_equal(turbo_sip_client_input(target->client, packet.data, packet.size, "127.0.0.1", source->sip_port), 0);
}

static struct sip_message_t* capture_sip_request(unsigned i)
{
    captured[i] = pop_packet(&caller);
    requests[i] = sip_message_parse_datagram(captured[i].data, captured[i].size);
    check_not_null(requests[i]);
    return requests[i];
}

static void mock_response(const struct sip_message_t* request, int code, const char* to,
                          const char* name, const char* value, const char* body)
{
    struct sip_message_t* response = sip_message_create(SIP_MESSAGE_REPLY);
    check_not_null(response);
    check_equal(sip_message_init3(response, request, NULL), 0);
    if (to) check_equal(sip_message_add_header(response, "To", to), 0);
    if (name) check_equal(sip_message_add_header(response, name, value), 0);
    if (body) {
        check_equal(sip_message_add_header(response, "Content-Type", "application/sdp"), 0);
        response->payload = body;
        response->size = (int)strlen(body);
    }
    response->u.s.code = code;
    response->u.s.reason = vstr_from_cstr("Test");
    unsigned char packet[SIP_DATAGRAM_MAX_BYTES];
    int bytes = sip_message_write(response, packet, sizeof(packet));
    sip_message_destroy(response);
    check(bytes > 0);
    check_equal(turbo_sip_client_input(caller.client, packet, (size_t)bytes, "127.0.0.1", peer.sip_port), 0);
}

suite("installed SIP client API") {
    before_each() {
        memset(&caller, 0, sizeof(caller));
        memset(&peer, 0, sizeof(peer));
        start_endpoint(&caller, "sip:alice@127.0.0.1:5071", "<sip:alice@127.0.0.1:5071>", 5071, 12000);
        start_endpoint(&peer, "sip:bob@127.0.0.1:5072", "<sip:bob@127.0.0.1:5072>", 5072, 12002);
    }

    after_each() {
        turbo_sip_client_destroy(caller.client);
        turbo_sip_client_destroy(peer.client);
        for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
            sip_message_destroy(requests[i]);
            requests[i] = NULL;
        }
    }

    it("registers and explicitly withdraws the same binding") {
        check_equal(turbo_sip_client_register(caller.client), 0);
        check_equal(turbo_sip_client_poll(caller.client, 1001), 0);
        struct sip_message_t* initial = capture_sip_request(0);
        mock_response(initial, 401, NULL, "WWW-Authenticate",
                      "Digest realm=\"local-test\", nonce=\"registration\", algorithm=SHA-256, qop=\"auth\"", NULL);
        check_equal(turbo_sip_client_poll(caller.client, 1002), 0);
        struct sip_message_t* binding = capture_sip_request(1);
        check_not_null(sip_message_get_header_by_name(binding, "Authorization"));
        mock_response(binding, 200, NULL, "Contact", "<sip:alice@127.0.0.1:5071>;expires=60", NULL);
        check_equal(turbo_sip_client_poll(caller.client, 1003), 0);
        check_equal(caller.registration, SIP_REGISTRATION_ACTIVE);

        check_equal(turbo_sip_client_unregister(caller.client), 0);
        check_equal(turbo_sip_client_poll(caller.client, 1004), 0);
        struct sip_message_t* leaving = capture_sip_request(2);
        check(sip_sv_equal(&binding->callid, &leaving->callid));
        check_equal(leaving->cseq.id, binding->cseq.id + 1);
        check_equal(sip_sv_compare_cstr(sip_message_get_header_by_name(leaving, "Expires"), "0"), 0);
        check_not_null(sip_message_get_header_by_name(leaving, "Authorization"));
        mock_response(leaving, 200, NULL, NULL, NULL, NULL);
        check_equal(turbo_sip_client_poll(caller.client, 1005), 0);
        check_equal(caller.registration, SIP_REGISTRATION_INACTIVE);
        check_equal(turbo_sip_client_poll(caller.client, 50000), 0);
        check_equal(caller.outbound_count, 0u);
    }

    it("exchanges MESSAGE independently of a call") {
        unsigned char body[] = {0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD};
        check_equal(turbo_sip_client_message(caller.client, "sip:bob@127.0.0.1:5072", "text/plain", body, sizeof(body)), 0);
        memset(body, 0, sizeof(body));
        transfer(&caller, &peer);
        transfer(&peer, &caller);
        check_equal(peer.incoming_messages, 1u);
        check_equal(caller.message_results, 1u);
        check_equal(caller.code, 200);
    }

    it("dials, answers, transfers PCM and RTCP, and hangs up") {
        turbo_sip_call_t* call = NULL;
        check_equal(turbo_sip_client_dial(caller.client, "sip:bob@127.0.0.1:5072", &call), 0);
        check(call == caller.call);
        transfer(&caller, &peer);
        check_equal(peer.state, TURBO_SIP_CALL_INCOMING);
        transfer(&peer, &caller);
        check_equal(caller.state, TURBO_SIP_CALL_RINGING);
        check_equal(turbo_sip_call_answer(peer.call), 0);
        transfer(&peer, &caller);
        transfer(&caller, &peer);
        check_equal(caller.state, TURBO_SIP_CALL_ACTIVE);
        check_equal(peer.state, TURBO_SIP_CALL_ACTIVE);
        check_equal(peer.media_starts, 1u);
        check_equal(caller.media_starts, 1u);

        int16_t outgoing[TEST_AUDIO_SAMPLES], incoming[TEST_AUDIO_SAMPLES];
        for (size_t i = 0; i < TEST_AUDIO_SAMPLES; ++i) {
            outgoing[i] = 7932;
            incoming[i] = -8316;
        }
        check_equal(turbo_sip_call_send_pcm(caller.call, outgoing, TEST_AUDIO_SAMPLES), 0);
        check_equal(turbo_sip_call_input_media(peer.call, TURBO_SIP_RTP, caller.media, caller.media_size), 0);
        check_equal(turbo_sip_call_send_pcm(peer.call, incoming, TEST_AUDIO_SAMPLES), 0);
        check_equal(turbo_sip_call_input_media(caller.call, TURBO_SIP_RTP, peer.media, peer.media_size), 0);
        check_equal(peer.pcm_count, (size_t)TEST_AUDIO_SAMPLES);
        check_equal(caller.pcm_count, (size_t)TEST_AUDIO_SAMPLES);
        check_equal(peer.pcm, outgoing, sizeof(outgoing));
        check_equal(caller.pcm, incoming, sizeof(incoming));

        check_equal(turbo_sip_client_poll(caller.client, 10000), 0);
        check_equal(caller.rtcp_count, 1u);
        check_equal(turbo_sip_call_input_media(peer.call, TURBO_SIP_RTCP, caller.media, caller.media_size), 0);

        check_equal(turbo_sip_call_hangup(caller.call), 0);
        transfer(&caller, &peer);
        transfer(&peer, &caller);
        check_equal(peer.state, TURBO_SIP_CALL_ENDED);
        check_equal(caller.state, TURBO_SIP_CALL_ENDED);
        check_equal(peer.media_closes, 1u);
        check_equal(caller.media_closes, 1u);
        check_equal(turbo_sip_call_release(peer.call), 0);
        check_equal(turbo_sip_call_release(caller.call), 0);
    }

    it("sends CANCEL only after provisional response and completes the original INVITE") {
        turbo_sip_call_t* call = NULL;
        check_equal(turbo_sip_client_dial(caller.client, "sip:bob@127.0.0.1:5072", &call), 0);
        check_equal(turbo_sip_call_hangup(call), 0);
        check_equal(caller.outbound_count, 1u);
        transfer(&caller, &peer);
        transfer(&peer, &caller);
        transfer(&caller, &peer);
        transfer(&peer, &caller);
        transfer(&peer, &caller);
        transfer(&caller, &peer);
        check_equal(peer.state, TURBO_SIP_CALL_ENDED);
        check_equal(caller.state, TURBO_SIP_CALL_ENDED);
        check_equal(caller.code, 487);
        check_equal(caller.media_closes, 1u);
        check_equal(peer.media_starts, 0u);
    }

    it("ACKs a challenged INVITE and retries with the same call identity") {
        turbo_sip_call_t* call = NULL;
        check_equal(turbo_sip_client_dial(caller.client, "sip:bob@127.0.0.1:5072", &call), 0);
        struct sip_message_t* invite = capture_sip_request(0);
        mock_response(invite, 407, "<sip:bob@127.0.0.1:5072>;tag=proxy",
                      "Proxy-Authenticate", "Digest realm=\"local-test\", nonce=\"nonce\", algorithm=SHA-256", NULL);
        struct sip_message_t* ack = capture_sip_request(1);
        check(sip_message_isack(ack));
        check(sip_sv_equal(sip_vias_top_branch(&invite->vias), sip_vias_top_branch(&ack->vias)));
        struct sip_message_t* authenticated = capture_sip_request(2);
        check(sip_message_isinvite(authenticated));
        check(sip_sv_equal(&invite->callid, &authenticated->callid));
        check(sip_sv_equal(&invite->from.tag, &authenticated->from.tag));
        check_equal(authenticated->cseq.id, invite->cseq.id + 1);
        check(!sip_sv_equal(sip_vias_top_branch(&invite->vias), sip_vias_top_branch(&authenticated->vias)));
        const vstr* auth = sip_message_get_header_by_name(authenticated, "Proxy-Authorization");
        check_not_null(auth);
        check(sip_sv_starts_with(auth, "Digest username=\"test-user\", realm=\"local-test\", nonce=\"nonce\", uri=\"sip:bob@127.0.0.1:5072\""));
        char header[SIP_DATAGRAM_MAX_BYTES];
        check(sip_sv_copy(auth, header, sizeof(header)) < sizeof(header));
        check_not_null(strstr(header, "response=\"094e6acc01ec3c744e28964bc3b7d067cb584dc660258d8378f4f487d9133119\""));
        check_equal(caller.active_count, 0u);
        mock_response(authenticated, 407, "<sip:bob@127.0.0.1:5072>;tag=proxy",
                      "Proxy-Authenticate", "Digest realm=\"local-test\", nonce=\"nonce\", algorithm=SHA-256", NULL);
        check_equal(caller.outbound_count, 1u);
        check_equal(caller.state, TURBO_SIP_CALL_ENDED);
        check_equal(caller.code, 407);
    }

    it("ACKs each fork and sends BYE only to the unselected dialog") {
        const char* sdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
                          "m=audio 12002 RTP/AVP 0\r\nc=IN IP4 127.0.0.1\r\na=rtcp-mux\r\n";
        turbo_sip_call_t* call = NULL;
        check_equal(turbo_sip_client_dial(caller.client, "sip:bob@127.0.0.1:5072", &call), 0);
        struct sip_message_t* invite = capture_sip_request(0);
        mock_response(invite, 200, "<sip:bob@127.0.0.1:5072>;tag=first",
                      "Contact", "<sip:bob@127.0.0.1:5072>", sdp);
        struct sip_message_t* first_ack = capture_sip_request(1);
        check(sip_message_isack(first_ack));
        check_equal(sip_sv_compare_cstr(&first_ack->to.tag, "first"), 0);
        mock_response(invite, 200, "<sip:bob@127.0.0.1:5072>;tag=second",
                      "Contact", "<sip:bob@127.0.0.1:5073>", sdp);
        struct sip_message_t* second_ack = capture_sip_request(2);
        struct sip_message_t* bye = capture_sip_request(3);
        check(sip_message_isack(second_ack));
        check(sip_message_isbye(bye));
        check_equal(sip_sv_compare_cstr(&second_ack->to.tag, "second"), 0);
        check_equal(sip_sv_compare_cstr(&bye->to.tag, "second"), 0);
        mock_response(invite, 200, "<sip:bob@127.0.0.1:5072>;tag=first",
                      "Contact", "<sip:bob@127.0.0.1:5072>", sdp);
        struct datagram duplicate = pop_packet(&caller);
        struct sip_message_t* repeated = sip_message_parse_datagram(duplicate.data, duplicate.size);
        check_not_null(repeated);
        int same = sip_sv_equal(sip_vias_top_branch(&first_ack->vias), sip_vias_top_branch(&repeated->vias));
        sip_message_destroy(repeated);
        check(same);
        check_equal(caller.outbound_count, 0u);
        check_equal(caller.active_count, 1u);
    }

    it("rolls back rejected first sends before closing their contexts") {
        caller.send_error = -EIO;
        int result = turbo_sip_client_message(caller.client, "sip:bob@127.0.0.1:5072", "text/plain", "x", 1);
        check_equal(result, -EIO);
        check_equal(caller.sends, 1u);
        check_equal(turbo_sip_client_poll(caller.client, 50000), 0);
        check_equal(caller.sends, 1u);
        check_equal(caller.message_results, 0u);
    }
}
