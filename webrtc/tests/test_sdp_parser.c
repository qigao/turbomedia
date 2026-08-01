/**
 * @file test_sdp_parser.c
 * @brief Unit tests for SDP parser
 *
 * Tests parsing and generation of WebRTC SDP
 */

#include "tinytest_compat.h"
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
    TLOG_INFO("Running %s", "test_parse_audio_sdp");
    sdp_session_t sdp;
    int result = sdp_parse(SIMPLE_AUDIO_SDP, strlen(SIMPLE_AUDIO_SDP), &sdp);

    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(1, sdp.media_count);
    TEST_ASSERT_EQUAL_INT(SDP_MEDIA_AUDIO, sdp.media[0].type);
    TEST_ASSERT_EQUAL_STRING("0", sdp.media[0].mid);
    TEST_ASSERT_EQUAL_STRING("abcd", sdp.media[0].ice_ufrag);
    TEST_ASSERT_EQUAL_STRING("1234567890abcdef", sdp.media[0].ice_pwd);
    TEST_ASSERT_EQUAL_INT(SDP_ROLE_ACTPASS, sdp.media[0].setup);
    TEST_ASSERT_EQUAL_INT(SDP_DIRECTION_SENDRECV, sdp.media[0].direction);
    TEST_ASSERT_EQUAL_INT(1, sdp.media[0].rtcp_mux);
}

/* Test: Parse DataChannel SDP */
void test_parse_datachannel_sdp(void) {
    sdp_session_t sdp;
    int result = sdp_parse(DATACHANNEL_SDP, strlen(DATACHANNEL_SDP), &sdp);

    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(1, sdp.media_count);
    TEST_ASSERT_EQUAL_INT(SDP_MEDIA_APPLICATION, sdp.media[0].type);
    TEST_ASSERT_EQUAL_STRING("0", sdp.media[0].mid);
    TEST_ASSERT_EQUAL_INT(5000, sdp.media[0].sctp_port);
    TEST_ASSERT_EQUAL_INT(262144, sdp.media[0].max_message_size);
    TEST_ASSERT_EQUAL_INT(SDP_ROLE_ACTIVE, sdp.media[0].setup);
}

void test_parse_origin_preserves_uint64_version(void) {
    static const char *sdp_text =
        "v=0\r\n"
        "o=- 123456 18446744073709551615 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n";
    sdp_session_t sdp;

    TEST_ASSERT_EQUAL_INT(0, sdp_parse(sdp_text, strlen(sdp_text), &sdp));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, sdp.session_version);
    TEST_ASSERT_EQUAL_STRING("123456", sdp.session_id);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", sdp.origin_addr);
}

/* Test: Parse ICE candidates */
void test_parse_candidates(void) {
    sdp_session_t sdp;
    int result = sdp_parse(CANDIDATE_SDP, strlen(CANDIDATE_SDP), &sdp);

    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(1, sdp.media_count);
    TEST_ASSERT_EQUAL_INT(2, sdp.media[0].candidate_count);

    /* Check first candidate (host) */
    const sdp_candidate_t *cand1 = &sdp.media[0].candidates[0];
    TEST_ASSERT_EQUAL_STRING("1", cand1->foundation);
    TEST_ASSERT_EQUAL_INT(1, cand1->component);
    TEST_ASSERT_EQUAL_STRING("udp", cand1->transport);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", cand1->address);
    TEST_ASSERT_EQUAL_UINT16(54321, cand1->port);
    TEST_ASSERT_EQUAL_STRING("host", cand1->type);

    /* Check second candidate (srflx) */
    const sdp_candidate_t *cand2 = &sdp.media[0].candidates[1];
    TEST_ASSERT_EQUAL_STRING("2", cand2->foundation);
    TEST_ASSERT_EQUAL_STRING("srflx", cand2->type);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", cand2->rel_addr);
    TEST_ASSERT_EQUAL_UINT16(54321, cand2->rel_port);
}

/* Test: Parse codecs */
void test_parse_codecs(void) {
    sdp_session_t sdp;
    int result = sdp_parse(SIMPLE_AUDIO_SDP, strlen(SIMPLE_AUDIO_SDP), &sdp);

    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_GREATER_THAN(0, sdp.media[0].codec_count);

    /* Find Opus codec */
    sdp_codec_t *opus = sdp_find_codec_by_pt(&sdp.media[0], 111);
    TEST_ASSERT_NOT_NULL(opus);
    TEST_ASSERT_EQUAL_STRING("opus", opus->name);
    TEST_ASSERT_EQUAL_INT(48000, opus->clock_rate);
    TEST_ASSERT_EQUAL_INT(2, opus->channels);
    TEST_ASSERT_EQUAL_STRING("minptime=10;useinbandfec=1", opus->fmtp);
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

    TEST_ASSERT_EQUAL_INT(0, sdp_parse(sdp_text, strlen(sdp_text), &sdp));
    TEST_ASSERT_EQUAL_INT(1, sdp.media[0].extension_count);
    TEST_ASSERT_EQUAL_INT(3, sdp.media[0].extensions[0].id);
    TEST_ASSERT_EQUAL_STRING(TRANSPORT_CC_URI, sdp.media[0].extensions[0].uri);
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

    TEST_ASSERT_EQUAL_INT(0, sdp_parse(sdp_text, strlen(sdp_text), &sdp));
    TEST_ASSERT_EQUAL_INT(SDP_MAX_EXTENSIONS, sdp.media[0].extension_count);
}

/* Test: Generate SDP */
void test_generate_sdp(void) {
    sdp_session_t sdp;
    sdp_session_init(&sdp);

    /* Add audio media */
    sdp_media_t *media = sdp_add_audio(&sdp, "0", SDP_DIRECTION_SENDRECV);
    TEST_ASSERT_NOT_NULL(media);

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
    TEST_ASSERT_EQUAL_INT(0, sdp_media_add_extension(media, 1, TRANSPORT_CC_URI));

    /* Generate SDP string */
    char buffer[4096];
    int len = sdp_generate(&sdp, buffer, sizeof(buffer));

    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NOT_NULL(strstr(buffer, "v=0"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "m=audio"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "a=rtpmap:111 opus/48000/2"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "a=ice-ufrag:test_ufrag"));
    TEST_ASSERT_NOT_NULL(strstr(buffer, "a=extmap:1 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"));
}

void test_generate_sdp_rejects_truncated_output(void) {
    sdp_session_t sdp;
    char buffer[8] = "stale";

    sdp_session_init(&sdp);
    TEST_ASSERT_EQUAL_INT(-1, sdp_generate(&sdp, buffer, sizeof(buffer)));
    TEST_ASSERT_EQUAL_STRING("", buffer);
}

void test_generate_session_id_rejects_truncation(void) {
    char session_id[2] = "x";

    sdp_generate_session_id(session_id, sizeof(session_id));
    TEST_ASSERT_EQUAL_STRING("", session_id);
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
    TEST_ASSERT_GREATER_THAN(0, len);

    /* Parse back */
    int result = sdp_parse(buffer, len, &parsed);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(original.media_count, parsed.media_count);
    TEST_ASSERT_EQUAL_STRING(original.media[0].mid, parsed.media[0].mid);
    TEST_ASSERT_EQUAL_STRING(original.media[0].ice_ufrag, parsed.media[0].ice_ufrag);
}

/* Test: Find functions */
void test_find_functions(void) {
    sdp_session_t sdp;
    sdp_session_init(&sdp);

    sdp_add_audio(&sdp, "0", SDP_DIRECTION_SENDRECV);
    sdp_add_video(&sdp, "1", SDP_DIRECTION_RECVONLY);

    /* Find by mid */
    sdp_media_t *m0 = sdp_find_media_by_mid(&sdp, "0");
    TEST_ASSERT_NOT_NULL(m0);
    TEST_ASSERT_EQUAL_INT(SDP_MEDIA_AUDIO, m0->type);

    /* Find by type */
    sdp_media_t *video = sdp_find_media_by_type(&sdp, SDP_MEDIA_VIDEO);
    TEST_ASSERT_NOT_NULL(video);
    TEST_ASSERT_EQUAL_STRING("1", video->mid);
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

    TEST_ASSERT_EQUAL_size_t(sizeof(first_ufrag) - 1, strlen(first_ufrag));
    TEST_ASSERT_EQUAL_size_t(sizeof(first_pwd) - 1, strlen(first_pwd));
    TEST_ASSERT_EQUAL_size_t(strlen(first_ufrag), strspn(first_ufrag, alphabet));
    TEST_ASSERT_EQUAL_size_t(strlen(first_pwd), strspn(first_pwd, alphabet));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(first_ufrag, second_ufrag));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(first_pwd, second_pwd));

    strcpy(first_pwd, "not-empty");
    sdp_generate_ice_credentials(NULL, 0, first_pwd, sizeof(first_pwd));
    TEST_ASSERT_EQUAL_STRING("", first_pwd);
}

spec("test_sdp_parser") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  TT_TEST(test_parse_audio_sdp);
  TT_TEST(test_parse_datachannel_sdp);
  TT_TEST(test_parse_origin_preserves_uint64_version);
  TT_TEST(test_parse_candidates);
  TT_TEST(test_parse_codecs);
  TT_TEST(test_parse_extmap);
  TT_TEST(test_parse_extmap_ignores_overflow);
  TT_TEST(test_generate_sdp);
  TT_TEST(test_generate_sdp_rejects_truncated_output);
  TT_TEST(test_generate_session_id_rejects_truncation);
  TT_TEST(test_roundtrip);
  TT_TEST(test_find_functions);
  TT_TEST(test_generate_ice_credentials);
}
