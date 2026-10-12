/* An unrelated C11 program must link the INSTALLED RTSP shared
 * library and exported headers, never the source-tree build outputs. */
#include <turbo_rtsp_rtp.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    static const uint8_t picture[]={
        0x01,0x23,0x3a,0x00,
        0x00,0x00,0x01,0xb3,0x11,0x22,0x33,0x44,
        0x00,0x00,0x01,0x00,0x48,0xd0,0xaa,0xbb,
        0x00,0x00,0x01,0x01,0xde,0xad,0xbe,0xef
    };
    uint8_t bytes[64]={0};
    turbo_rtsp_rtp_header_t out={0},view={0};
    turbo_rtsp_mpeg_video_payload_t es={0};
    size_t offset=0u;
    int count;
    out.version=2;
    out.marker=1u;
    out.payload_type=TURBO_RTSP_MPEG_VIDEO_RTP_PAYLOAD_TYPE;
    out.sequence_number=195u;
    out.timestamp=3000u;
    out.ssrc=0x19761976u;
    count=turbo_rtsp_rtp_write_packet(
        bytes,sizeof(bytes),&out,picture,sizeof(picture));
    if(count<=0)return 1;
    if(turbo_rtsp_rtp_parse_header(
        bytes,(size_t)count,&view,&offset)!=0)return 2;
    if(view.payload_type!=TURBO_RTSP_MPEG_VIDEO_RTP_PAYLOAD_TYPE ||
       view.marker!=1u || view.ssrc!=out.ssrc || view.timestamp!=3000u ||
       view.sequence_number!=195u ||
       view.payload_len!=sizeof(picture) ||
       memcmp(view.payload,picture,sizeof(picture))!=0)return 3;
    if(turbo_rtsp_mpeg_video_payload_parse(
        view.payload,view.payload_len,&es)!=0)return 4;
    if(es.temporal_reference!=0x123u || es.picture_type!=2u ||
       es.sequence_header!=1u || es.es_len!=sizeof(picture)-4u)return 5;
    if(turbo_rtsp_mpeg_video_one_picture_validate(&es,view.marker)!=0)
        return 6;
    if(turbo_rtsp_mpeg_video_one_picture_validate(&es,0u)==0)return 7;
    es.picture_type=3u;
    if(turbo_rtsp_mpeg_video_one_picture_validate(&es,1u)==0)return 8;
    /* TS PT33 is explicitly a different format from video ES PT32. */
    out.payload_type=33u;
    count=turbo_rtsp_rtp_write_packet(
        bytes,sizeof(bytes),&out,picture,sizeof(picture));
    if(count<=0 || turbo_rtsp_rtp_parse_header(
        bytes,(size_t)count,&view,&offset)!=0 ||
       view.payload_type!=33u)return 9;
    return 0;
}
