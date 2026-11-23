#include "turbo_rtp_packet.h"
#include "rtp-payload.h"
#include "sip-agent.h"
#include "sip-uac-transaction.h"
#include "tinytest.h"
#include "turbo_sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char caller[] = "Alice <sip:alice@127.0.0.1:5060>";
static const char callee[] = "sip:bob@127.0.0.1:5070";
static const char offer[] =
    "v=0\r\n"
    "o=alice 1 1 IN IP4 127.0.0.1\r\n"
    "s=Audio\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 40000 RTP/AVP 0 8\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n";
static const char answer[] =
    "v=0\r\n"
    "o=bob 2 2 IN IP4 127.0.0.1\r\n"
    "s=Audio\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 40002 RTP/AVP 0\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=sendrecv\r\n";

enum { PCMU_FRAME_BYTES = 160 };

struct captured_rtp {
    unsigned char data[RTP_HEADER_SIZE + PCMU_FRAME_BYTES];
    int size;
    uint32_t timestamp;
    int flags;
    int count;
};

static struct sip_fixture {
    struct sip_agent_t *agent;
    struct sip_uac_transaction_t *transaction;
    struct sip_message_t *reply;
    char wire[UDP_PACKET_SIZE];
    size_t wire_size;
    int sent;
    int replies;
    int status;
    int dialog_confirmed;
    int sdp_result;
    sdp_session_t remote_sdp;
    struct captured_rtp encoded;
    struct captured_rtp decoded;
    void *encoder;
    void *decoder;
} sip;

static void set_up(void)
{
    struct sip_uas_handler_t handlers = {0};
    memset(&sip, 0, sizeof(sip));
    sip.agent = sip_agent_create(&handlers);
}

static void tear_down(void)
{
    struct sip_uac_transaction_t *transaction = sip.transaction;
    if (sip.decoder)
        rtp_payload_decode_destroy(sip.decoder);
    if (sip.encoder)
        rtp_payload_encode_destroy(sip.encoder);
    if (sip.reply)
        sip_message_destroy(sip.reply);

    if (transaction) {
        int linked;
        /* Drain through the real transaction timers without waiting out Timer M. */
        sync_mutex_lock(&transaction->locker);
        linked = transaction->linked;
        if (linked) {
            if (transaction->status <= SIP_UAC_TRANSACTION_PROCEEDING) {
                sip_uac_transaction_timeout(transaction, 1);
            } else {
                sip_uac_stop_timer(sip.agent, transaction, &transaction->timerd);
                sip_uac_transaction_timewait(transaction, 1);
            }
        }
        sync_mutex_unlock(&transaction->locker);
        for (unsigned i = 0; linked && i < 1000; ++i) {
            sleep_ms(1);
            sync_mutex_lock(&transaction->locker);
            linked = transaction->linked;
            sync_mutex_unlock(&transaction->locker);
        }
        sip_uac_transaction_release(transaction);
        check_equal(linked, 0);
    }
    if (sip.agent)
        sip_agent_destroy(sip.agent);
}

static int local_via(void *context, const char *destination, char protocol[16],
                     char local[128], char dns[128])
{
    static const char address[] = "127.0.0.1:5060";
    (void)context;
    (void)destination;
    memcpy(protocol, "TCP", sizeof("TCP"));
    memcpy(local, address, sizeof(address));
    memcpy(dns, address, sizeof(address));
    return 0;
}

static int capture_request(void *context, const void *data, size_t bytes)
{
    struct sip_fixture *fixture = context;
    if (bytes >= sizeof(fixture->wire))
        return -1;
    memcpy(fixture->wire, data, bytes);
    fixture->wire[bytes] = '\0';
    fixture->wire_size = bytes;
    ++fixture->sent;
    return 0;
}

static struct sip_transport_t transport = {local_via, capture_request};

static int on_message_reply(void *context, const struct sip_message_t *reply,
                            struct sip_uac_transaction_t *transaction, int code)
{
    struct sip_fixture *fixture = context;
    if (transaction == fixture->transaction && reply) {
        ++fixture->replies;
        fixture->status = code;
    }
    return 0;
}

static int on_invite_reply(void *context, const struct sip_message_t *reply,
                           struct sip_uac_transaction_t *transaction,
                           struct sip_dialog_t *dialog, const vstr *id, int code)
{
    struct sip_fixture *fixture = context;
    on_message_reply(context, reply, transaction, code);
    if (code == 200 && reply) {
        fixture->dialog_confirmed = dialog && id && id->len > 0;
        fixture->sdp_result = reply->payload && reply->size > 0
            ? sdp_parse(reply->payload, (size_t)reply->size, &fixture->remote_sdp)
            : -1;
    }
    return 0;
}

static void create_ok_reply(void)
{
    sip.reply = sip_message_create(SIP_MESSAGE_REPLY);
    check(sip.reply != NULL);
    check_equal(sip_message_init3(sip.reply, sip.transaction->req, NULL), 0);
    sip.reply->u.s.code = 200;
}

static void check_request(const char *request_line, const char *content_type,
                          const void *body, size_t bytes)
{
    char content_length[64];
    const char *wire_body;
    check_equal(strncmp(sip.wire, request_line, strlen(request_line)), 0);
    if (content_type)
        check(strstr(sip.wire, content_type) != NULL);
    snprintf(content_length, sizeof(content_length), "\r\nContent-Length: %zu\r\n", bytes);
    check(strstr(sip.wire, content_length) != NULL);
    wire_body = strstr(sip.wire, "\r\n\r\n");
    check(wire_body != NULL);
    wire_body += 4;
    check_equal(sip.wire_size - (size_t)(wire_body - sip.wire), bytes);
    if (bytes)
        check_equal(wire_body, body, bytes);
}

static void *allocate_packet(void *context, int bytes)
{
    (void)context;
    return malloc((size_t)bytes);
}

static void release_packet(void *context, void *packet)
{
    (void)context;
    free(packet);
}

static int capture_packet(void *context, const void *data, int bytes,
                          uint32_t timestamp, int flags)
{
    struct captured_rtp *capture = context;
    if (bytes < 0 || (size_t)bytes > sizeof(capture->data))
        return -1;
    memcpy(capture->data, data, (size_t)bytes);
    capture->size = bytes;
    capture->timestamp = timestamp;
    capture->flags = flags;
    ++capture->count;
    return 0;
}

static void check_pcmu_rtp(const sdp_codec_t *codec)
{
    static struct rtp_payload_t callbacks = {allocate_packet, release_packet, capture_packet};
    const uint16_t first_sequence = 300;
    const uint32_t first_timestamp = 8000;
    const uint32_t ssrc = 0x12345678;
    unsigned char frame[PCMU_FRAME_BYTES];

    sip.encoder = rtp_payload_encode_create(codec->payload_type, codec->name,
                                            first_sequence, ssrc, &callbacks, &sip.encoded);
    sip.decoder = rtp_payload_decode_create(codec->payload_type, codec->name, &callbacks, &sip.decoded);
    check(sip.encoder != NULL);
    check(sip.decoder != NULL);

    for (unsigned packet = 0; packet < 2; ++packet) {
        rtp_packet_t header;
        uint32_t timestamp = first_timestamp + packet * (uint32_t)(codec->clock_rate / 50);
        for (unsigned sample = 0; sample < sizeof(frame); ++sample)
            frame[sample] = (unsigned char)(sample + packet);

        check_equal(rtp_payload_encode_input(sip.encoder, frame, sizeof(frame), timestamp), 0);
        check_equal(sip.encoded.count, (int)packet + 1);
        check_equal(rtp_packet_parse(&header, sip.encoded.data, sip.encoded.size), 0);
        check_equal((int)header.header.version, RTP_VERSION);
        check_equal((int)header.header.payload_type, codec->payload_type);
        check_equal((unsigned)header.header.sequence, first_sequence + packet);
        check_equal(header.header.ssrc, ssrc);
        check_equal(header.header.timestamp, timestamp);
        check_equal(header.payload_len, PCMU_FRAME_BYTES);

        check_equal(rtp_payload_decode_input(sip.decoder, sip.encoded.data, sip.encoded.size), 1);
        check_equal(sip.decoded.count, (int)packet + 1);
        check_equal(sip.decoded.timestamp, timestamp);
        check_equal(sip.decoded.flags, 0);
        check_equal(sip.decoded.size, PCMU_FRAME_BYTES);
        check_equal(sip.decoded.data, frame, sizeof(frame));
    }
}

spec("sip_uac") {
    before_each() { set_up(); }
    after_each() { tear_down(); }

    it("uses UDP Content-Length to bound an opaque payload and rejects truncation") {
        static const char wire[] =
            "SIP/2.0 200 OK\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK1234\r\n"
            "From: <sip:alice@127.0.0.1>;tag=alice\r\n"
            "To: <sip:bob@127.0.0.1>;tag=bob\r\n"
            "Call-ID: test@127.0.0.1\r\nCSeq: 17 MESSAGE\r\n"
            "Content-Type: application/octet-stream\r\nl:\r\n\t3\r\n\r\n"
            "X\0Yextra";
        static const char body[] = "X\0Y";
        char invalid[sizeof(wire)];
        sip.reply = sip_message_parse_datagram(wire, sizeof(wire) - 1);
        check(sip.reply != NULL);
        check_equal(sip.reply->u.s.code, 200);
        check_equal(sip.reply->size, 3);
        check_equal(sip.reply->payload, body, 3u);
        check(sip_message_parse_datagram(wire, sizeof(wire) - sizeof("Yextra")) == NULL);
        memcpy(invalid, wire, sizeof(wire));
        invalid[4] = '\0';
        check(sip_message_parse_datagram(invalid, sizeof(invalid) - 1) == NULL);
    };

    it("sends a UTF-8 MESSAGE and accepts only its matching response") {
        static const char body[] = "hello \xe4\xbd\xa0\xe5\xa5\xbd";
        static const char destination[] =
            "<sip:bob@127.0.0.1:5070;transport=tcp;method=MESSAGE?subject=hello>";
        uint32_t sequence;
        check(sip.agent != NULL);
        sip.transaction = sip_uac_message(sip.agent, caller, destination, on_message_reply, &sip);
        check(sip.transaction != NULL);
        check_equal(sip_uac_add_header(sip.transaction, "Content-Type", "text/plain; charset=utf-8"), 0);
        check_equal(sip_uac_send(sip.transaction, body, sizeof(body) - 1, &transport, &sip), 0);
        check_equal(sip.sent, 1);
        check_request("MESSAGE sip:bob@127.0.0.1:5070;transport=tcp SIP/2.0\r\n",
                      "\r\nContent-Type: text/plain; charset=utf-8\r\n", body, sizeof(body) - 1);

        create_ok_reply();
        sequence = sip.reply->cseq.id;
        sip.reply->cseq.id = sequence + 1;
        check_equal(sip_agent_input(sip.agent, sip.reply, NULL), 0);
        check_equal(sip.replies, 0);
        sip.reply->cseq.id = sequence;
        check_equal(sip_agent_input(sip.agent, sip.reply, NULL), 0);
        check_equal(sip.replies, 1);
        check_equal(sip.status, 200);
    };

    it("ACKs the INVITE and round-trips PCMU RTP selected by the SDP answer") {
        const sdp_media_t *audio;
        const sdp_codec_t *codec;
        char ack_cseq[64];
        check(sip.agent != NULL);
        sip.transaction = sip_uac_invite(sip.agent, caller, callee, on_invite_reply, &sip);
        check(sip.transaction != NULL);
        check_equal(sip_uac_add_header(sip.transaction, "Content-Type", "application/sdp"), 0);
        check_equal(sip_uac_send(sip.transaction, offer, sizeof(offer) - 1, &transport, &sip), 0);
        check_request("INVITE sip:bob@127.0.0.1:5070 SIP/2.0\r\n",
                      "\r\nContent-Type: application/sdp\r\n", offer, sizeof(offer) - 1);

        create_ok_reply();
        check_equal(sip_message_add_header(sip.reply, "Contact", "<sip:bob@127.0.0.1:5071>"), 0);
        check_equal(sip_message_add_header(sip.reply, "Content-Type", "application/sdp"), 0);
        sip.reply->payload = answer;
        sip.reply->size = sizeof(answer) - 1;
        check_equal(sip_agent_input(sip.agent, sip.reply, NULL), 0);
        check_equal(sip.replies, 1);
        check_equal(sip.status, 200);
        check_equal(sip.dialog_confirmed, 1);
        check_equal(sip.sdp_result, 0);
        check_equal(sip.remote_sdp.media_count, 1);
        audio = &sip.remote_sdp.media[0];
        check_equal(audio->type, SDP_MEDIA_AUDIO);
        check_equal((int)audio->port, 40002);
        check_equal(audio->protocol, "RTP/AVP");
        check_equal(audio->direction, SDP_DIRECTION_SENDRECV);
        check_equal(audio->codec_count, 1);
        codec = &audio->codecs[0];
        check_equal(codec->name, "PCMU");
        check_equal(codec->payload_type, 0);
        check_equal(codec->clock_rate, 8000);
        check_equal(codec->channels, 1);

        check_equal(sip_uac_ack(sip.transaction, NULL, 0, NULL), 0);
        check_equal(sip.sent, 2);
        check_request("ACK sip:bob@127.0.0.1:5071 SIP/2.0\r\n", NULL, NULL, 0);
        snprintf(ack_cseq, sizeof(ack_cseq), "\r\nCSeq: %u ACK\r\n", (unsigned)sip.reply->cseq.id);
        check(strstr(sip.wire, ack_cseq) != NULL);

        check_pcmu_rtp(codec);
    };
}
