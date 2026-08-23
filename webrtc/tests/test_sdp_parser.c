/**
 * @file test_sdp_parser.c
 * @brief Unit tests for SDP parser
 *
 * Tests parsing and generation of WebRTC SDP
 */

#include "tinytest.h"
#include "turbo_sdp.h"
#include "tlog.h"
#include <string.h>

/* Test SDP samples */
static const char *SIMPLE_AUDIO_SDP =
    "v=0\r\n"
    "o=- 123456 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:abcd\r\n"
    "a=ice-pwd:1234567890abcdef\r\n"
    "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n";

static const char *DATACHANNEL_SDP =
    "v=0\r\n"
    "o=- 654321 1 IN IP4 192.168.1.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:efgh\r\n"
    "a=ice-pwd:fedcba0987654321\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:active\r\n"
    "a=mid:0\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n";

static const char *CANDIDATE_SDP =
    "v=0\r\n"
    "o=- 999 1 IN IP4 10.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=candidate:1 1 udp 2130706431 192.168.1.100 54321 typ host\r\n"
    "a=candidate:2 1 udp 1694498815 203.0.113.1 12345 typ srflx raddr 192.168.1.100 rport 54321\r\n";

static const char *TRANSPORT_CC_URI =
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";

void setUp(void) {
}

void tearDown(void) {
}

/* Test: Parse simple audio SDP */
void test_parse_audio_sdp(void) {
    TLOG_INFOF("Running {}", "test_parse_audio_sdp");
    sdp_session_t sdp;
    int result = sdp_parse(SIMPLE_AUDIO_SDP, strlen(SIMPLE_AUDIO_SDP), &sdp);

    check_equal((int)(result), (int)(0));
    check_equal((int)(sdp.media_count), (int)(1));
    check_equal((int)(sdp.media[0].type), (int)(SDP_MEDIA_AUDIO));
    check_equal(sdp.media[0].mid, "0");
    check_equal(sdp.media[0].ice_ufrag, "abcd");
    check_equal(sdp.media[0].ice_pwd, "1234567890abcdef");
    check_equal((int)(sdp.media[0].setup), (int)(SDP_ROLE_ACTPASS));
    check_equal((int)(sdp.media[0].direction), (int)(SDP_DIRECTION_SENDRECV));
    check_equal((int)(sdp.media[0].rtcp_mux), (int)(1));
}

/* Test: Parse DataChannel SDP */
void test_parse_datachannel_sdp(void) {
    sdp_session_t sdp;
    int result = sdp_parse(DATACHANNEL_SDP, strlen(DATACHANNEL_SDP), &sdp);

    check_equal((int)(result), (int)(0));
    check_equal((int)(sdp.media_count), (int)(1));
    check_equal((int)(sdp.media[0].type), (int)(SDP_MEDIA_APPLICATION));
    check_equal(sdp.media[0].mid, "0");
    check_equal((int)(sdp.media[0].sctp_port), (int)(5000));
    check_equal((int)(sdp.media[0].max_message_size), (int)(262144));
    check_equal((int)(sdp.media[0].setup), (int)(SDP_ROLE_ACTIVE));
}

void test_parse_origin_preserves_uint64_version(void) {
    static const char *sdp_text =
        "v=0\r\n"
        "o=- 123456 18446744073709551615 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n";
    sdp_session_t sdp;

    check_equal((int)(sdp_parse(sdp_text, strlen(sdp_text), &sdp)), (int)(0));
    check_equal((uint64_t)(sdp.session_version), (uint64_t)(UINT64_MAX));
    check_equal(sdp.session_id, "123456");
    check_equal(sdp.origin_addr, "127.0.0.1");
}

/* Test: Parse ICE candidates */
void test_parse_candidates(void) {
    sdp_session_t sdp;
    int result = sdp_parse(CANDIDATE_SDP, strlen(CANDIDATE_SDP), &sdp);

    check_equal((int)(result), (int)(0));
    check_equal((int)(sdp.media_count), (int)(1));
    check_equal((int)(sdp.media[0].candidate_count), (int)(2));

    /* Check first candidate (host) */
    const sdp_candidate_t *cand1 = &sdp.media[0].candidates[0];
    check_equal(cand1->foundation, "1");
    check_equal((int)(cand1->component), (int)(1));
    check_equal(cand1->transport, "udp");
    check_equal(cand1->address, "192.168.1.100");
    check_equal((uint16_t)(cand1->port), (uint16_t)(54321));
    check_equal(cand1->type, "host");

    /* Check second candidate (srflx) */
    const sdp_candidate_t *cand2 = &sdp.media[0].candidates[1];
    check_equal(cand2->foundation, "2");
    check_equal(cand2->type, "srflx");
    check_equal(cand2->rel_addr, "192.168.1.100");
    check_equal((uint16_t)(cand2->rel_port), (uint16_t)(54321));
}

/* Test: Parse codecs */
void test_parse_codecs(void) {
    sdp_session_t sdp;
    int result = sdp_parse(SIMPLE_AUDIO_SDP, strlen(SIMPLE_AUDIO_SDP), &sdp);

    check_equal((int)(result), (int)(0));
    check_greater(sdp.media[0].codec_count, 0);

    /* Find Opus codec */
    sdp_codec_t *opus = sdp_find_codec_by_pt(&sdp.media[0], 111);
    check_not_null(opus);
    check_equal(opus->name, "opus");
    check_equal((int)(opus->clock_rate), (int)(48000));
    check_equal((int)(opus->channels), (int)(2));
    check_equal(opus->fmtp, "minptime=10;useinbandfec=1");
}

void test_parse_extmap(void) {
    const char *sdp_text =
        "v=0\r\n"
        "o=- 123456 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=extmap:3/sendrecv http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        "a=rtpmap:96 VP8/90000\r\n";
    sdp_session_t sdp;

    check_equal((int)(sdp_parse(sdp_text, strlen(sdp_text), &sdp)), (int)(0));
    check_equal((int)(sdp.media[0].extension_count), (int)(1));
    check_equal((int)(sdp.media[0].extensions[0].id), (int)(3));
    check_equal(sdp.media[0].extensions[0].uri, TRANSPORT_CC_URI);
}

void test_parse_extmap_ignores_overflow(void) {
    const char *sdp_text =
        "v=0\r\n"
        "o=- 123456 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=extmap:1 urn:one\r\n"
        "a=extmap:2 urn:two\r\n"
        "a=extmap:3 urn:three\r\n"
        "a=extmap:4 urn:four\r\n"
        "a=extmap:5 urn:five\r\n"
        "a=extmap:6 urn:six\r\n"
        "a=extmap:7 urn:seven\r\n"
        "a=extmap:8 urn:eight\r\n"
        "a=extmap:9 urn:nine\r\n"
        "a=rtpmap:96 VP8/90000\r\n";
    sdp_session_t sdp;

    check_equal((int)(sdp_parse(sdp_text, strlen(sdp_text), &sdp)), (int)(0));
    check_equal((int)(sdp.media[0].extension_count), (int)(SDP_MAX_EXTENSIONS));
}

/* Test: Generate SDP */
void test_generate_sdp(void) {
    sdp_session_t sdp;
    sdp_session_init(&sdp);

    /* Add audio media */
    sdp_media_t *media = sdp_add_audio(&sdp, "0", SDP_DIRECTION_SENDRECV);
    check_not_null(media);

    sdp_media_set_ice(media, "test_ufrag", "test_password");
    sdp_media_set_fingerprint(media, "sha-256", "AA:BB:CC:DD");
    media->setup = SDP_ROLE_ACTPASS;

    /* Add Opus codec */
    sdp_codec_t codec = {
        .payload_type = 111,
        .clock_rate = 48000,
        .channels = 2
    };
    strncpy(codec.name, "opus", sizeof(codec.name) - 1);
    strncpy(codec.fmtp, "minptime=10", sizeof(codec.fmtp) - 1);
    sdp_media_add_codec(media, &codec);
    check_equal((int)(sdp_media_add_extension(media, 1, TRANSPORT_CC_URI)), (int)(0));

    /* Generate SDP string */
    char buffer[4096];
    int len = sdp_generate(&sdp, buffer, sizeof(buffer));

    check_greater(len, 0);
    check_not_null(strstr(buffer, "v=0"));
    check_not_null(strstr(buffer, "m=audio"));
    check_not_null(strstr(buffer, "a=rtpmap:111 opus/48000/2"));
    check_not_null(strstr(buffer, "a=ice-ufrag:test_ufrag"));
    check_not_null(strstr(buffer, "a=extmap:1 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"));
}

void test_generate_sdp_rejects_truncated_output(void) {
    sdp_session_t sdp;
    char buffer[8] = "stale";

    sdp_session_init(&sdp);
    check_equal((int)(sdp_generate(&sdp, buffer, sizeof(buffer))), (int)(-1));
    check_equal(buffer, "");
}

void test_generate_session_id_rejects_truncation(void) {
    char session_id[2] = "x";

    sdp_generate_session_id(session_id, sizeof(session_id));
    check_equal(session_id, "");
}

/* Test: Round-trip (generate -> parse) */
void test_roundtrip(void) {
    sdp_session_t original, parsed;
    sdp_session_init(&original);

    /* Create SDP */
    sdp_media_t *media = sdp_add_audio(&original, "0", SDP_DIRECTION_SENDRECV);
    sdp_media_set_ice(media, "ufrag123", "pwd456");

    sdp_codec_t codec = {.payload_type = 111, .clock_rate = 48000, .channels = 2};
    strncpy(codec.name, "opus", sizeof(codec.name) - 1);
    sdp_media_add_codec(media, &codec);

    /* Generate */
    char buffer[4096];
    int len = sdp_generate(&original, buffer, sizeof(buffer));
    check_greater(len, 0);

    /* Parse back */
    int result = sdp_parse(buffer, len, &parsed);
    check_equal((int)(result), (int)(0));
    check_equal((int)(parsed.media_count), (int)(original.media_count));
    check_equal(parsed.media[0].mid, original.media[0].mid);
    check_equal(parsed.media[0].ice_ufrag, original.media[0].ice_ufrag);
}

/* Test: Find functions */
void test_find_functions(void) {
    sdp_session_t sdp;
    sdp_session_init(&sdp);

    sdp_add_audio(&sdp, "0", SDP_DIRECTION_SENDRECV);
    sdp_add_video(&sdp, "1", SDP_DIRECTION_RECVONLY);

    /* Find by mid */
    sdp_media_t *m0 = sdp_find_media_by_mid(&sdp, "0");
    check_not_null(m0);
    check_equal((int)(m0->type), (int)(SDP_MEDIA_AUDIO));

    /* Find by type */
    sdp_media_t *video = sdp_find_media_by_type(&sdp, SDP_MEDIA_VIDEO);
    check_not_null(video);
    check_equal(video->mid, "1");
}

void test_generate_ice_credentials(void) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char first_ufrag[17];
    char first_pwd[33];
    char second_ufrag[17];
    char second_pwd[33];

    sdp_generate_ice_credentials(
        first_ufrag, sizeof(first_ufrag), first_pwd, sizeof(first_pwd));
    sdp_generate_ice_credentials(
        second_ufrag, sizeof(second_ufrag), second_pwd, sizeof(second_pwd));

    check_equal((size_t)(strlen(first_ufrag)), (size_t)(sizeof(first_ufrag) - 1));
    check_equal((size_t)(strlen(first_pwd)), (size_t)(sizeof(first_pwd) - 1));
    check_equal((size_t)(strspn(first_ufrag, alphabet)), (size_t)(strlen(first_ufrag)));
    check_equal((size_t)(strspn(first_pwd, alphabet)), (size_t)(strlen(first_pwd)));
    check_not_equal(strcmp(first_ufrag, second_ufrag), 0);
    check_not_equal(strcmp(first_pwd, second_pwd), 0);

    strcpy(first_pwd, "not-empty");
    sdp_generate_ice_credentials(NULL, 0, first_pwd, sizeof(first_pwd));
    check_equal(first_pwd, "");
}

spec("test_sdp_parser") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  it("test_parse_audio_sdp") { test_parse_audio_sdp(); };
  it("test_parse_datachannel_sdp") { test_parse_datachannel_sdp(); };
  it("test_parse_origin_preserves_uint64_version") { test_parse_origin_preserves_uint64_version(); };
  it("test_parse_candidates") { test_parse_candidates(); };
  it("test_parse_codecs") { test_parse_codecs(); };
  it("test_parse_extmap") { test_parse_extmap(); };
  it("test_parse_extmap_ignores_overflow") { test_parse_extmap_ignores_overflow(); };
  it("test_generate_sdp") { test_generate_sdp(); };
  it("test_generate_sdp_rejects_truncated_output") { test_generate_sdp_rejects_truncated_output(); };
  it("test_generate_session_id_rejects_truncation") { test_generate_session_id_rejects_truncation(); };
  it("test_roundtrip") { test_roundtrip(); };
  it("test_find_functions") { test_find_functions(); };
  it("test_generate_ice_credentials") { test_generate_ice_credentials(); };
}
