/* Independently installed C++17 reader verifies that the new additive
 * RTSP/RFC2250 C functions keep unmangled C ABI and plain struct layout. */
#include <turbo_rtsp_rtp.h>
#include <type_traits>
#include <cstdint>

static_assert(std::is_standard_layout<turbo_rtsp_rtp_header_t>::value,
              "RTP header is a plain public C ABI");
static_assert(std::is_trivially_copyable<turbo_rtsp_mpeg_video_payload_t>::value,
              "Borrowed MPEG Video ES view is a trivial C record");
static_assert(TURBO_RTSP_MPEG_VIDEO_HEADER_SIZE==4,
              "RFC2250 elementary video header width");
static_assert(TURBO_RTSP_MPEG_VIDEO_RTP_PAYLOAD_TYPE==32,
              "RFC2250 MPEG Video ES static RTP PT");

int main() {
    const std::uint8_t es[]={
        0x00,0x03,0x3b,0x00, /* TR3, Sequence=1, B/E=1, P=3 */
        0x00,0x00,0x01,0xb3,0x11,0x22,0x33,0x44,
        0x00,0x00,0x01,0x00,0x00,0xd8,0x55,0x66,
        0x00,0x00,0x01,0x01,0xaa,0xbb
    };
    turbo_rtsp_mpeg_video_payload_t parsed{};
    const auto parse=&turbo_rtsp_mpeg_video_payload_parse;
    const auto validate=&turbo_rtsp_mpeg_video_one_picture_validate;
    if(parse(es,sizeof(es),&parsed)!=0)return 1;
    if(parsed.picture_type!=3 || parsed.temporal_reference!=3 ||
       parsed.es_len!=sizeof(es)-4)return 2;
    if(validate(&parsed,1u)!=0)return 3;
    if(validate(&parsed,0u)==0)return 4;
    return 0;
}
