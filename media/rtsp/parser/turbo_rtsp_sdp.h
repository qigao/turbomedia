#ifndef TURBO_RTSP_SDP_H
#define TURBO_RTSP_SDP_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef TURBO_MEDIA_HAS_RTSP

#define TURBO_RTSP_SDP_MAX_MEDIA 8
#define TURBO_RTSP_SDP_MAX_TOKEN 128
#define TURBO_RTSP_SDP_MAX_CONTROL 512
#define TURBO_RTSP_SDP_MAX_FMTP 512
#define TURBO_RTSP_SDP_H264_MAX_PROFILE_LEVEL_ID 16
#define TURBO_RTSP_SDP_H264_MAX_SPROP_PARAMETER_SETS 512
#define TURBO_RTSP_SDP_H265_MAX_SPROP_VPS 64
#define TURBO_RTSP_SDP_H265_MAX_SPROP_SPS 128
#define TURBO_RTSP_SDP_H265_MAX_SPROP_PPS 256
#define TURBO_RTSP_SDP_H265_MAX_SPROP_SEI 128

#define TURBO_RTSP_SDP_H264_FMTP_PACKETIZATION_MODE 0x00000001u
#define TURBO_RTSP_SDP_H264_FMTP_PROFILE_LEVEL_ID 0x00000002u
#define TURBO_RTSP_SDP_H264_FMTP_SPROP_PARAMETER_SETS 0x00000004u

#define TURBO_RTSP_SDP_H265_FMTP_SPROP_VPS 0x00000100u
#define TURBO_RTSP_SDP_H265_FMTP_SPROP_SPS 0x00000200u
#define TURBO_RTSP_SDP_H265_FMTP_SPROP_PPS 0x00000400u
#define TURBO_RTSP_SDP_H265_FMTP_SPROP_SEI 0x00000800u

#define TURBO_RTSP_SDP_MPEG4_FMTP_STREAM_TYPE 0x00000001u
#define TURBO_RTSP_SDP_MPEG4_FMTP_PROFILE_LEVEL_ID 0x00000002u
#define TURBO_RTSP_SDP_MPEG4_FMTP_CONFIG 0x00000004u
#define TURBO_RTSP_SDP_MPEG4_FMTP_MODE 0x00000008u
#define TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT_TYPE 0x00000010u
#define TURBO_RTSP_SDP_MPEG4_FMTP_CONSTANT_SIZE 0x00000020u
#define TURBO_RTSP_SDP_MPEG4_FMTP_CONSTANT_DURATION 0x00000040u
#define TURBO_RTSP_SDP_MPEG4_FMTP_SIZE_LENGTH 0x00000080u
#define TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_LENGTH 0x00000100u
#define TURBO_RTSP_SDP_MPEG4_FMTP_INDEX_DELTA_LENGTH 0x00000200u
#define TURBO_RTSP_SDP_MPEG4_FMTP_OBJECT 0x00000400u
#define TURBO_RTSP_SDP_MPEG4_FMTP_CPRESENT 0x00000800u

typedef struct {
    const char *origin_user;
    uint32_t session_id;
    uint32_t session_version;
    const char *origin_address;
    const char *session_name;
    const char *connection_address;
    const char *range;
    const char *direction;
} turbo_rtsp_sdp_session_t;

typedef struct {
    const char *media;
    int port;
    const char *proto;
    int payload_type;
    const char *encoding_name;
    int clock_rate;
    const char *control;
    const char *fmtp;
    const char *range;
    const char *direction;
    const char *connection_address;
} turbo_rtsp_sdp_media_t;

typedef struct {
    uint32_t flags;
    int packetization_mode;
    char profile_level_id[TURBO_RTSP_SDP_H264_MAX_PROFILE_LEVEL_ID];
    char sprop_parameter_sets[TURBO_RTSP_SDP_H264_MAX_SPROP_PARAMETER_SETS];
} turbo_rtsp_sdp_h264_fmtp_t;

typedef struct {
    uint32_t flags;
    char sprop_vps[TURBO_RTSP_SDP_H265_MAX_SPROP_VPS];
    char sprop_sps[TURBO_RTSP_SDP_H265_MAX_SPROP_SPS];
    char sprop_pps[TURBO_RTSP_SDP_H265_MAX_SPROP_PPS];
    char sprop_sei[TURBO_RTSP_SDP_H265_MAX_SPROP_SEI];
} turbo_rtsp_sdp_h265_fmtp_t;

typedef struct {
    uint32_t flags;
    int stream_type;
    char profile_level_id[16];
    char mode[32];
    char config[512];
    int object_type;
    int object;
    int cpresent;
    int constant_size;
    int constant_duration;
    int size_length;
    int index_length;
    int index_delta_length;
} turbo_rtsp_sdp_mpeg4_fmtp_t;

typedef struct {
    char media[TURBO_RTSP_SDP_MAX_TOKEN];
    int port;
    char proto[TURBO_RTSP_SDP_MAX_TOKEN];
    int payload_type;
    char encoding_name[TURBO_RTSP_SDP_MAX_TOKEN];
    int clock_rate;
    int encoding_parameters;
    char control[TURBO_RTSP_SDP_MAX_CONTROL];
    char fmtp[TURBO_RTSP_SDP_MAX_FMTP];
    turbo_rtsp_sdp_h264_fmtp_t h264_fmtp;
    turbo_rtsp_sdp_h265_fmtp_t h265_fmtp;
    turbo_rtsp_sdp_mpeg4_fmtp_t mpeg4_fmtp;
    char range[TURBO_RTSP_SDP_MAX_TOKEN];
    char direction[TURBO_RTSP_SDP_MAX_TOKEN];
    char connection_address[TURBO_RTSP_SDP_MAX_TOKEN];
} turbo_rtsp_sdp_media_desc_t;

typedef struct {
    char session_name[TURBO_RTSP_SDP_MAX_TOKEN];
    char connection_address[TURBO_RTSP_SDP_MAX_TOKEN];
    char control[TURBO_RTSP_SDP_MAX_CONTROL];
    char range[TURBO_RTSP_SDP_MAX_TOKEN];
    char direction[TURBO_RTSP_SDP_MAX_TOKEN];
    turbo_rtsp_sdp_media_desc_t media[TURBO_RTSP_SDP_MAX_MEDIA];
    size_t media_count;
} turbo_rtsp_sdp_description_t;

TURBO_MEDIA_C_API int turbo_rtsp_sdp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_session_t *session,
    const turbo_rtsp_sdp_media_t *media,
    size_t media_count);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_parse(
    const char *sdp,
    size_t sdp_len,
    turbo_rtsp_sdp_description_t *description);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_h264_fmtp_parse(
    const char *fmtp,
    size_t fmtp_len,
    turbo_rtsp_sdp_h264_fmtp_t *h264);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_h264_fmtp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h264_fmtp_t *h264);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_h264_fmtp_write_annexb(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h264_fmtp_t *h264,
    size_t *written);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_h265_fmtp_parse(
    const char *fmtp,
    size_t fmtp_len,
    turbo_rtsp_sdp_h265_fmtp_t *h265);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_h265_fmtp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h265_fmtp_t *h265);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_h265_fmtp_write_annexb(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_h265_fmtp_t *h265,
    size_t *written);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_mpeg4_fmtp_parse(
    const char *fmtp,
    size_t fmtp_len,
    turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_mpeg4_fmtp_build(
    char *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4);

TURBO_MEDIA_C_API int turbo_rtsp_sdp_mpeg4_fmtp_write_config(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_sdp_mpeg4_fmtp_t *mpeg4,
    size_t *written);

#endif /* TURBO_MEDIA_HAS_RTSP */

#endif /* TURBO_RTSP_SDP_H */
