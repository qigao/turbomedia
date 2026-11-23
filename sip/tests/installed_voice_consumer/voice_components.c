#include <sip-transport.h>
#include <sip-message.h>
#include <turbo_rtp_packet.h>
#include <rtp.h>
#include <rtp-demuxer.h>
#include <turbo_sdp.h>
#include <turbo_audio_codec.h>
#include <string.h>

int main(void) {
    const char request[] =
        "OPTIONS sip:bob@127.0.0.1 SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK-smoke\r\n"
        "From: <sip:alice@127.0.0.1>;tag=client\r\n"
        "To: <sip:bob@127.0.0.1>\r\n"
        "Call-ID: test@127.0.0.1\r\n"
        "CSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n";
    struct sip_message_t *sip = sip_message_parse_datagram(request, sizeof(request) - 1);
    if (!sip) return 1;
    sip_message_destroy(sip);

    const char offer[] =
        "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=voice\r\n"
        "c=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 4000 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";
    sdp_session_t sdp;
    if (sdp_parse(offer, sizeof(offer) - 1, &sdp) != 0 ||
        sdp.media_count != 1 || sdp.media[0].port != 4000) return 2;

    const uint8_t wire[] = {0x80, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0xff};
    rtp_packet_t packet;
    if (rtp_packet_parse(&packet, wire, sizeof(wire)) != 0 ||
        packet.header.payload_type != 0 || packet.payload_len != 1) return 3;
    uint8_t serialized[sizeof(wire)];
    if (rtp_packet_serialize(&packet, serialized, sizeof(serialized)) != (int)sizeof(wire) ||
        memcmp(wire, serialized, sizeof(wire)) != 0) return 3;

    struct rtp_event_t events = {0};
    void *rtp = rtp_create(&events, NULL, 1, 0, 8000, 8000, 1);
    if (!rtp) return 4;
    uint8_t report[256] = {0};
    const int report_size = rtp_rtcp_report(rtp, report, sizeof(report));
    rtp_destroy(rtp);
    if (report_size < 4 || report[1] != RTCP_SR) return 5;

    const turbo_audio_codec_config_t config = {.sample_rate = 8000, .channels = 1, .frame_size_ms = 20};
    turbo_audio_codec_t *decoder = turbo_audio_codec_create_decoder("pcmu", &config);
    if (!decoder) return 6;
    int16_t pcm;
    size_t bytes = sizeof(pcm);
    int status = turbo_audio_codec_decode(decoder, packet.payload, packet.payload_len,
                                         (uint8_t *)&pcm, &bytes);
    turbo_audio_codec_destroy(decoder);
    return status || bytes != sizeof(pcm) || pcm != 0;
}
