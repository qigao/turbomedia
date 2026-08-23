#ifndef TURBO_RTSP_RTP_H
#define TURBO_RTSP_RTP_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef TURBO_MEDIA_HAS_RTSP

#include <CoroNet.h>

#ifndef TURBO_RTSP_RTP_UDP_PAIR_TYPE_DEFINED
#define TURBO_RTSP_RTP_UDP_PAIR_TYPE_DEFINED
typedef struct turbo_rtsp_rtp_udp_pair_s turbo_rtsp_rtp_udp_pair_t;
#endif

#define TURBO_RTSP_RTP_HEADER_SIZE 12
#define TURBO_RTSP_INTERLEAVED_HEADER_SIZE 4
#define TURBO_RTSP_RTCP_HEADER_SIZE 4
#define TURBO_RTSP_RTCP_SENDER_INFO_SIZE 24
#define TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE 24
#define TURBO_RTSP_RTCP_SDES_CNAME 1
#define TURBO_RTSP_H264_NAL_TYPE_MASK 0x1f
#define TURBO_RTSP_H264_NAL_NRI_MASK 0x60
#define TURBO_RTSP_H264_NAL_TYPE_STAP_A 24
#define TURBO_RTSP_H264_NAL_TYPE_FU_A 28
#define TURBO_RTSP_H265_NAL_HEADER_SIZE 2
#define TURBO_RTSP_H265_NAL_TYPE_AP 48
#define TURBO_RTSP_H265_NAL_TYPE_FU 49
#define TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE 4
#define TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_AU_HEADER_BITS 16
#define TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_SIZE_LENGTH 13
#define TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_INDEX_LENGTH 3
#define TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_MAX_AU_SIZE 8191
#define TURBO_RTSP_MP4A_LATM_MAX_LENGTH_INFO_SIZE 40
#define TURBO_RTSP_MPEG2_TS_PACKET_SIZE 188
#define TURBO_RTSP_MPEG2_TS_SYNC_BYTE 0x47
#define TURBO_RTSP_RTP_H264_RECEIVE_REORDER_CAPACITY 4
#define TURBO_RTSP_RTP_H264_RECEIVE_REORDER_WINDOW 8
#define TURBO_RTSP_RTP_H264_RECEIVE_REORDER_PAYLOAD_SIZE 2048
#define TURBO_RTSP_RTP_H265_RECEIVE_REORDER_CAPACITY 4
#define TURBO_RTSP_RTP_H265_RECEIVE_REORDER_WINDOW 8
#define TURBO_RTSP_RTP_H265_RECEIVE_REORDER_PAYLOAD_SIZE 2048

typedef enum {
    TURBO_RTSP_FRAME_ERROR = -1,
    TURBO_RTSP_FRAME_OK = 0,
    TURBO_RTSP_FRAME_PARTIAL = 1
} turbo_rtsp_frame_result_t;

typedef enum {
    TURBO_RTSP_H264_PAYLOAD_SINGLE_NAL = 1,
    TURBO_RTSP_H264_PAYLOAD_STAP_A = TURBO_RTSP_H264_NAL_TYPE_STAP_A,
    TURBO_RTSP_H264_PAYLOAD_FU_A = TURBO_RTSP_H264_NAL_TYPE_FU_A
} turbo_rtsp_h264_payload_kind_t;

typedef enum {
    TURBO_RTSP_H265_PAYLOAD_SINGLE_NAL = 1,
    TURBO_RTSP_H265_PAYLOAD_AP = TURBO_RTSP_H265_NAL_TYPE_AP,
    TURBO_RTSP_H265_PAYLOAD_FU = TURBO_RTSP_H265_NAL_TYPE_FU
} turbo_rtsp_h265_payload_kind_t;

typedef enum {
    TURBO_RTSP_RTP_SOURCE_UPDATE_ERROR = -1,
    TURBO_RTSP_RTP_SOURCE_UPDATE_OK = 0,
    TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE = 1,
    TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER = 2,
    TURBO_RTSP_RTP_SOURCE_UPDATE_DROPOUT = 3,
    TURBO_RTSP_RTP_SOURCE_UPDATE_SSRC_MISMATCH = 4
} turbo_rtsp_rtp_source_update_result_t;

typedef enum {
    TURBO_RTSP_RTCP_SR = 200,
    TURBO_RTSP_RTCP_RR = 201,
    TURBO_RTSP_RTCP_SDES = 202,
    TURBO_RTSP_RTCP_BYE = 203,
    TURBO_RTSP_RTCP_APP = 204,
    TURBO_RTSP_RTCP_RTPFB = 205,
    TURBO_RTSP_RTCP_PSFB = 206
} turbo_rtsp_rtcp_packet_type_t;

typedef enum {
    TURBO_RTSP_RTCP_RTPFB_NACK = 1
} turbo_rtsp_rtcp_rtpfb_type_t;

typedef enum {
    TURBO_RTSP_RTCP_PSFB_PLI = 1
} turbo_rtsp_rtcp_psfb_type_t;

typedef struct {
    uint8_t version;
    uint8_t padding;
    uint8_t extension;
    uint8_t csrc_count;
    uint8_t marker;
    uint8_t payload_type;
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;
    uint32_t csrc[16];
    uint16_t extension_profile;
    size_t extension_len;
    const uint8_t *extension_data;
    uint8_t padding_len;
    const uint8_t *payload;
    size_t payload_len;
} turbo_rtsp_rtp_header_t;

typedef struct {
    turbo_rtsp_h264_payload_kind_t kind;
    uint8_t nal_header;
    uint8_t forbidden_zero_bit;
    uint8_t nal_ref_idc;
    uint8_t nal_unit_type;
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *nal;
    size_t nal_len;
    uint8_t fu_start;
    uint8_t fu_end;
    uint8_t fu_reserved;
    uint8_t fu_nal_unit_type;
    uint8_t reconstructed_nal_header;
    const uint8_t *fu_payload;
    size_t fu_payload_len;
} turbo_rtsp_h264_payload_t;

typedef struct {
    turbo_rtsp_h265_payload_kind_t kind;
    uint8_t nal_header[2];
    uint8_t forbidden_zero_bit;
    uint8_t nal_unit_type;
    uint8_t nuh_layer_id;
    uint8_t nuh_temporal_id_plus1;
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *nal;
    size_t nal_len;
    uint8_t fu_start;
    uint8_t fu_end;
    uint8_t fu_nal_unit_type;
    uint8_t reconstructed_nal_header[2];
    const uint8_t *fu_payload;
    size_t fu_payload_len;
} turbo_rtsp_h265_payload_t;

typedef struct {
    uint16_t au_header_bits;
    size_t au_size;
    uint8_t au_index;
    const uint8_t *au_fragment;
    size_t au_fragment_len;
    uint8_t complete;
} turbo_rtsp_mpeg4_generic_payload_t;

typedef struct {
    size_t payload_length;
    size_t length_info_len;
    const uint8_t *fragment;
    size_t fragment_len;
    uint8_t complete;
} turbo_rtsp_mp4a_latm_payload_t;

typedef struct {
    const uint8_t *packets;
    size_t packets_len;
    size_t packet_count;
} turbo_rtsp_mpeg2_ts_payload_t;

typedef struct {
    const uint8_t *payload;
    size_t payload_len;
    uint8_t marker;
    uint8_t end;
} turbo_rtsp_h264_packetized_payload_t;

typedef struct {
    const uint8_t *nal;
    size_t nal_len;
    size_t max_payload;
    size_t offset;
    uint8_t single_nal;
    uint8_t done;
} turbo_rtsp_h264_packetizer_t;

typedef turbo_rtsp_h264_packetized_payload_t turbo_rtsp_h265_packetized_payload_t;

typedef struct {
    const uint8_t *nal;
    size_t nal_len;
    size_t max_payload;
    size_t offset;
    uint8_t single_nal;
    uint8_t done;
} turbo_rtsp_h265_packetizer_t;

typedef struct {
    uint32_t ssrc;
    uint32_t timestamp;
    uint16_t next_sequence_number;
    uint8_t fu_nal_unit_type;
    uint8_t reconstructed_nal_header;
    size_t nal_len;
    uint8_t started;
} turbo_rtsp_h264_reassembler_t;

typedef struct {
    uint32_t ssrc;
    uint32_t timestamp;
    uint16_t next_sequence_number;
    uint8_t fu_nal_unit_type;
    uint8_t reconstructed_nal_header[2];
    size_t nal_len;
    uint8_t started;
} turbo_rtsp_h265_reassembler_t;

typedef struct {
    turbo_rtsp_rtp_header_t header;
    uint8_t payload[TURBO_RTSP_RTP_H264_RECEIVE_REORDER_PAYLOAD_SIZE];
    size_t payload_len;
    uint8_t occupied;
} turbo_rtsp_rtp_h264_receive_reorder_packet_t;

typedef struct {
    turbo_rtsp_rtp_header_t header;
    uint8_t payload[TURBO_RTSP_RTP_H265_RECEIVE_REORDER_PAYLOAD_SIZE];
    size_t payload_len;
    uint8_t occupied;
} turbo_rtsp_rtp_h265_receive_reorder_packet_t;

typedef struct {
    uint8_t channel;
    uint16_t payload_len;
    const uint8_t *payload;
} turbo_rtsp_interleaved_frame_t;

typedef struct {
    uint8_t header[TURBO_RTSP_INTERLEAVED_HEADER_SIZE];
    size_t header_len;
    uint8_t channel;
    uint16_t payload_len;
    size_t payload_remaining;
    size_t needed;
    uint8_t discarding_payload;
    uint8_t *payload_buffer;
    size_t payload_buffer_size;
    size_t payload_copied;
    uint8_t frame_pending;
} turbo_rtsp_interleaved_parser_t;

typedef struct {
    uint8_t version;
    uint8_t padding;
    uint8_t count;
    uint8_t packet_type;
    uint16_t length;
    size_t packet_len;
    uint8_t padding_len;
    size_t payload_len;
} turbo_rtsp_rtcp_header_t;

typedef struct {
    uint32_t ssrc;
    uint64_t ntp_timestamp;
    uint32_t rtp_timestamp;
    uint32_t packet_count;
    uint32_t octet_count;
} turbo_rtsp_rtcp_sender_info_t;

typedef struct {
    uint32_t ssrc;
    uint8_t fraction_lost;
    int32_t cumulative_lost;
    uint32_t extended_highest_sequence_number;
    uint32_t jitter;
    uint32_t last_sender_report;
    uint32_t delay_since_last_sender_report;
} turbo_rtsp_rtcp_report_block_t;

typedef struct {
    uint8_t subtype;
    uint32_t ssrc;
    char name[4];
    const uint8_t *data;
    size_t data_len;
} turbo_rtsp_rtcp_app_t;

typedef struct {
    uint16_t packet_id;
    uint16_t lost_packet_bitmask;
} turbo_rtsp_rtcp_nack_item_t;

typedef struct {
    uint32_t ssrc;
    uint16_t base_sequence_number;
    uint16_t max_sequence_number;
    uint32_t cycles;
    uint32_t received;
    uint32_t expected_prior;
    uint32_t received_prior;
    int32_t transit;
    uint32_t jitter_q4;
    uint64_t received_sequence_bitmap;
    uint8_t has_ssrc;
    uint8_t initialized;
} turbo_rtsp_rtp_source_t;

typedef struct {
    uint32_t ssrc;
    uint32_t packet_count;
    uint32_t octet_count;
    uint8_t has_ssrc;
} turbo_rtsp_rtp_sender_t;

typedef struct {
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t clock_rate;
    turbo_rtsp_rtp_sender_t sender;
} turbo_rtsp_rtp_stream_t;

typedef struct {
    uint8_t *packet;
    size_t packet_len;
} turbo_rtsp_rtp_stream_packet_t;

typedef struct {
    turbo_rtsp_rtp_source_t source;
    turbo_rtsp_h264_reassembler_t reassembler;
    turbo_rtsp_rtp_h264_receive_reorder_packet_t reorder[
        TURBO_RTSP_RTP_H264_RECEIVE_REORDER_CAPACITY];
    uint8_t reorder_count;
    uint8_t has_next_sequence_number;
    uint16_t next_sequence_number;
    uint8_t reorder_output_payload[TURBO_RTSP_RTP_H264_RECEIVE_REORDER_PAYLOAD_SIZE];
} turbo_rtsp_rtp_h264_receive_stream_t;

typedef struct {
    turbo_rtsp_rtp_source_t source;
    turbo_rtsp_h265_reassembler_t reassembler;
    turbo_rtsp_rtp_h265_receive_reorder_packet_t reorder[
        TURBO_RTSP_RTP_H265_RECEIVE_REORDER_CAPACITY];
    uint8_t reorder_count;
    uint8_t has_next_sequence_number;
    uint16_t next_sequence_number;
    uint8_t reorder_output_payload[TURBO_RTSP_RTP_H265_RECEIVE_REORDER_PAYLOAD_SIZE];
} turbo_rtsp_rtp_h265_receive_stream_t;

typedef struct {
    const char *local_host;      /* NULL defaults to 0.0.0.0 */
    int local_rtp_port;          /* 0 binds an ephemeral RTP port */
    int local_rtcp_port;         /* 0 uses local_rtp_port + 1, or ephemeral when RTP is ephemeral */
    uint64_t timeout_ms;         /* 0 leaves CoroNet default socket timeout */
} turbo_rtsp_rtp_udp_pair_config_t;

TURBO_MEDIA_C_API int turbo_rtsp_rtp_parse_header(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtp_header_t *header,
    size_t *header_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_write_header(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtp_header_t *header);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_write_packet(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtp_header_t *header,
    const uint8_t *payload,
    size_t payload_len);

TURBO_MEDIA_C_API int turbo_rtsp_mpeg4_generic_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_mpeg4_generic_payload_t *mpeg4);

TURBO_MEDIA_C_API int turbo_rtsp_mp4a_latm_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_mp4a_latm_payload_t *latm);

TURBO_MEDIA_C_API int turbo_rtsp_mpeg2_ts_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_mpeg2_ts_payload_t *ts);

TURBO_MEDIA_C_API int turbo_rtsp_h264_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_h264_payload_t *h264);

TURBO_MEDIA_C_API int turbo_rtsp_h264_stap_a_next(
    const turbo_rtsp_h264_payload_t *h264,
    size_t *offset,
    const uint8_t **nal,
    size_t *nal_len);

TURBO_MEDIA_C_API int turbo_rtsp_h264_payload_write_nal_fragment(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_h264_payload_t *h264,
    size_t *written);

TURBO_MEDIA_C_API int turbo_rtsp_h264_packetizer_init(
    turbo_rtsp_h264_packetizer_t *packetizer,
    const uint8_t *nal,
    size_t nal_len,
    size_t max_payload);

TURBO_MEDIA_C_API int turbo_rtsp_h264_packetizer_next(
    turbo_rtsp_h264_packetizer_t *packetizer,
    uint8_t *payload,
    size_t payload_size,
    turbo_rtsp_h264_packetized_payload_t *out);

TURBO_MEDIA_C_API void turbo_rtsp_h264_reassembler_init(
    turbo_rtsp_h264_reassembler_t *reassembler);

TURBO_MEDIA_C_API void turbo_rtsp_h264_reassembler_reset(
    turbo_rtsp_h264_reassembler_t *reassembler);

TURBO_MEDIA_C_API int turbo_rtsp_h264_reassembler_push(
    turbo_rtsp_h264_reassembler_t *reassembler,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *buffer,
    size_t buffer_size,
    const uint8_t **nal,
    size_t *nal_len);

TURBO_MEDIA_C_API int turbo_rtsp_h265_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_h265_payload_t *h265);

TURBO_MEDIA_C_API int turbo_rtsp_h265_ap_next(
    const turbo_rtsp_h265_payload_t *h265,
    size_t *offset,
    const uint8_t **nal,
    size_t *nal_len);

TURBO_MEDIA_C_API int turbo_rtsp_h265_payload_write_nal_fragment(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_h265_payload_t *h265,
    size_t *written);

TURBO_MEDIA_C_API int turbo_rtsp_h265_packetizer_init(
    turbo_rtsp_h265_packetizer_t *packetizer,
    const uint8_t *nal,
    size_t nal_len,
    size_t max_payload);

TURBO_MEDIA_C_API int turbo_rtsp_h265_packetizer_next(
    turbo_rtsp_h265_packetizer_t *packetizer,
    uint8_t *payload,
    size_t payload_size,
    turbo_rtsp_h265_packetized_payload_t *out);

TURBO_MEDIA_C_API void turbo_rtsp_h265_reassembler_init(
    turbo_rtsp_h265_reassembler_t *reassembler);

TURBO_MEDIA_C_API void turbo_rtsp_h265_reassembler_reset(
    turbo_rtsp_h265_reassembler_t *reassembler);

TURBO_MEDIA_C_API int turbo_rtsp_h265_reassembler_push(
    turbo_rtsp_h265_reassembler_t *reassembler,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *buffer,
    size_t buffer_size,
    const uint8_t **nal,
    size_t *nal_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_header(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_header_t *header);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_header(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_header_t *header);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_next_packet(
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_rtcp_header_t *header,
    size_t *consumed);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_validate_compound(
    const uint8_t *buffer,
    size_t buffer_len,
    size_t *packet_count);

TURBO_MEDIA_C_API uint32_t turbo_rtsp_rtcp_ntp_to_lsr(
    uint64_t ntp_timestamp);

TURBO_MEDIA_C_API uint32_t turbo_rtsp_rtcp_delay_us_to_dlsr(
    uint64_t delay_us);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_sender_info(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_sender_info_t *info);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_sender_info(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_sender_info_t *info);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_report_block(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_report_block_t *block);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_report_block(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_report_block_t *block);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_receiver_report(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *reporter_ssrc,
    turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_capacity,
    size_t *block_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_receiver_report(
    uint8_t *packet,
    size_t packet_size,
    uint32_t reporter_ssrc,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_sender_report(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_sender_info_t *sender_info,
    turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_capacity,
    size_t *block_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_sender_report(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_sender_info_t *sender_info,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_sdes_cname(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *ssrc,
    const char **cname,
    size_t *cname_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_sdes_cname(
    uint8_t *packet,
    size_t packet_size,
    uint32_t ssrc,
    const char *cname,
    size_t cname_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_bye(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *ssrcs,
    size_t ssrc_capacity,
    size_t *ssrc_count,
    const char **reason,
    size_t *reason_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_bye(
    uint8_t *packet,
    size_t packet_size,
    const uint32_t *ssrcs,
    size_t ssrc_count,
    const char *reason,
    size_t reason_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_app(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_app_t *app);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_app(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_app_t *app);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_generic_nack(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *sender_ssrc,
    uint32_t *media_ssrc,
    turbo_rtsp_rtcp_nack_item_t *items,
    size_t item_capacity,
    size_t *item_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_generic_nack(
    uint8_t *packet,
    size_t packet_size,
    uint32_t sender_ssrc,
    uint32_t media_ssrc,
    const turbo_rtsp_rtcp_nack_item_t *items,
    size_t item_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_parse_pli(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *sender_ssrc,
    uint32_t *media_ssrc);

TURBO_MEDIA_C_API int turbo_rtsp_rtcp_write_pli(
    uint8_t *packet,
    size_t packet_size,
    uint32_t sender_ssrc,
    uint32_t media_ssrc);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_source_init(
    turbo_rtsp_rtp_source_t *source,
    uint32_t ssrc);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_source_update(
    turbo_rtsp_rtp_source_t *source,
    const turbo_rtsp_rtp_header_t *header,
    uint32_t arrival_rtp_timestamp);

TURBO_MEDIA_C_API turbo_rtsp_rtp_source_update_result_t turbo_rtsp_rtp_source_update_ex(
    turbo_rtsp_rtp_source_t *source,
    const turbo_rtsp_rtp_header_t *header,
    uint32_t arrival_rtp_timestamp);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_source_make_report(
    turbo_rtsp_rtp_source_t *source,
    uint32_t last_sender_report,
    uint32_t delay_since_last_sender_report,
    turbo_rtsp_rtcp_report_block_t *block);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_source_write_receiver_report(
    turbo_rtsp_rtp_source_t *source,
    uint8_t *packet,
    size_t packet_size,
    uint32_t reporter_ssrc,
    uint32_t last_sender_report,
    uint32_t delay_since_last_sender_report);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_source_write_rtcp_compound(
    turbo_rtsp_rtp_source_t *source,
    uint8_t *packet,
    size_t packet_size,
    uint32_t reporter_ssrc,
    uint32_t last_sender_report,
    uint32_t delay_since_last_sender_report,
    const char *cname,
    size_t cname_len);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_sender_init(
    turbo_rtsp_rtp_sender_t *sender,
    uint32_t ssrc);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_sender_update(
    turbo_rtsp_rtp_sender_t *sender,
    const turbo_rtsp_rtp_header_t *header);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_sender_write_sender_report(
    const turbo_rtsp_rtp_sender_t *sender,
    uint8_t *packet,
    size_t packet_size,
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_sender_write_rtcp_compound(
    const turbo_rtsp_rtp_sender_t *sender,
    uint8_t *packet,
    size_t packet_size,
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count,
    const char *cname,
    size_t cname_len);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_stream_init(
    turbo_rtsp_rtp_stream_t *stream,
    uint8_t payload_type,
    uint32_t ssrc,
    uint16_t sequence_number,
    uint32_t timestamp,
    uint32_t clock_rate);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_payload(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *payload,
    size_t payload_len,
    int marker,
    uint32_t timestamp_increment,
    uint8_t *packet,
    size_t packet_size,
    size_t *packet_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_mpeg4_generic_au(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *au,
    size_t au_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_mp4a_latm(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *latm,
    size_t latm_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_mpeg2_ts(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *ts,
    size_t ts_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_h264_nal(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_h264_nal_ex(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    int marker,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_h265_nal(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_stream_write_h265_nal_ex(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    int marker,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_h264_receive_stream_init(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    uint32_t ssrc);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_h264_receive_stream_reset(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    uint32_t ssrc);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_h264_receive_stream_push(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    const uint8_t *packet,
    size_t packet_len,
    uint32_t arrival_rtp_timestamp,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len,
    turbo_rtsp_rtp_source_update_result_t *source_result);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_h264_receive_stream_drain_queued(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_h265_receive_stream_init(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    uint32_t ssrc);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_h265_receive_stream_reset(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    uint32_t ssrc);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_h265_receive_stream_push(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    const uint8_t *packet,
    size_t packet_len,
    uint32_t arrival_rtp_timestamp,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len,
    turbo_rtsp_rtp_source_update_result_t *source_result);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_h265_receive_stream_drain_queued(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len);

TURBO_MEDIA_C_API turbo_rtsp_rtp_udp_pair_t *turbo_rtsp_rtp_udp_pair_create(
    coro_context_t *ctx,
    const turbo_rtsp_rtp_udp_pair_config_t *config);

TURBO_MEDIA_C_API void turbo_rtsp_rtp_udp_pair_destroy(
    turbo_rtsp_rtp_udp_pair_t *pair);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_udp_pair_get_local_ports(
    turbo_rtsp_rtp_udp_pair_t *pair,
    int *rtp_port,
    int *rtcp_port);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_udp_pair_set_peer(
    turbo_rtsp_rtp_udp_pair_t *pair,
    const char *host,
    int rtp_port,
    int rtcp_port);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_udp_pair_send_rtp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    const uint8_t *packet,
    size_t packet_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_udp_pair_send_rtcp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    const uint8_t *packet,
    size_t packet_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_udp_pair_recv_rtp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len);

TURBO_MEDIA_C_API int turbo_rtsp_rtp_udp_pair_recv_rtcp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len);

TURBO_MEDIA_C_API int turbo_rtsp_interleaved_parse(
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_interleaved_frame_t *frame,
    size_t *consumed);

TURBO_MEDIA_C_API void turbo_rtsp_interleaved_parser_init(
    turbo_rtsp_interleaved_parser_t *parser);

TURBO_MEDIA_C_API void turbo_rtsp_interleaved_parser_reset(
    turbo_rtsp_interleaved_parser_t *parser);

TURBO_MEDIA_C_API void turbo_rtsp_interleaved_parser_destroy(
    turbo_rtsp_interleaved_parser_t *parser);

TURBO_MEDIA_C_API int turbo_rtsp_interleaved_parser_parse(
    turbo_rtsp_interleaved_parser_t *parser,
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_interleaved_frame_t *frame,
    size_t *consumed);

/* Returns TURBO_RTSP_FRAME_ERROR with frame->payload_len set to the required
 * payload size when payload_capacity is too small; the frame remains pending
 * so callers can retry with a larger payload buffer. */
TURBO_MEDIA_C_API int turbo_rtsp_interleaved_parser_parse_copy(
    turbo_rtsp_interleaved_parser_t *parser,
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_interleaved_frame_t *frame,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *consumed);

TURBO_MEDIA_C_API int turbo_rtsp_interleaved_write_header(
    uint8_t *buffer,
    size_t buffer_size,
    uint8_t channel,
    uint16_t payload_len);

#endif /* TURBO_MEDIA_HAS_RTSP */

#endif /* TURBO_RTSP_RTP_H */
