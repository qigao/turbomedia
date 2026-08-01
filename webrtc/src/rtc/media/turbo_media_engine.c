/**
 * TurboNet Media Implementation
 *
 * Audio/video media track management
 */
#include "turbo_media_engine.h"
#include "jitter_buffer.h"
#include "tlog.h"
#include "turbo_capture.h"
#include "turbo_codec.h"
#include "turbo_datachannel.h"
#include "turbo_nack.h"
#include "turbo_rtp.h"
#include "turbo_srtp.h"
#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Internal Structures
 * ============================================================================= */

struct turbo_media_track_s {
  turbo_media_context_t *ctx;
  turbo_rtc_media_track_type_t type;
  turbo_media_direction_t direction;
  turbo_media_state_t state;
  turbo_codec_type_t codec_type;
  uint8_t payload_type;

  /* RTP/SRTP */
  rtp_session_t *rtp_session;
  srtp_session_t *srtp_session;

  /* TWCC (Transport-Wide Congestion Control) */
  twcc_tracker_t *twcc_tracker;   /* Sender side */
  twcc_receiver_t *twcc_receiver; /* Receiver side */
  uint64_t last_twcc_feedback_time;
  uint32_t current_target_bitrate;
  int transport_cc_ext_id;

  /* Jitter buffer (receive) */
  jitter_buffer_t *jitter;

  /* Codec context */
  void *encoder;
  void *decoder;

  /* Capture context */
  void *capture;
  rtp_history_t *history;

  /* Configuration */
  union {
    turbo_audio_config_t audio;
    turbo_video_config_t video;
  } config;

  /* Callbacks */
  turbo_rtc_media_frame_cb frame_cb;
  turbo_media_state_cb state_cb;
  turbo_media_keyframe_cb keyframe_cb;
  turbo_media_rtp_packet_cb rtp_packet_cb;
  void *user_data;

  /* Statistics */
  turbo_media_stats_t stats;
  uint64_t last_nack_time;
};

struct turbo_media_context_s {
  turbo_dc_peer_t *peer;
  void *user_data;
  srtp_session_t *rtcp_session;

  /* Tracks */
  turbo_media_track_t *tracks[TURBO_MEDIA_MAX_TRACKS];
  int track_count;

  /* SRTP keying material */
  srtp_keying_material_t keys;
  int keys_valid;

  /* Timing */
  uint64_t last_rtcp_time;
  uint32_t rtcp_interval_ms;
};

/* =============================================================================
 * Time Utilities
 * ============================================================================= */

#define MEDIA_RTP_PAYLOAD_MTU 1100

static uint64_t get_time_ms(void) { return turbo_monotonic_ms(); }

static uint32_t read_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void parse_rtcp_rr_block(const uint8_t *data, rtcp_rr_block_t *block) {
  uint32_t loss_word;

  if (!data || !block) {
    return;
  }

  memset(block, 0, sizeof(*block));
  block->ssrc = read_u32(data);
  loss_word = read_u32(data + 4);
  block->fraction_lost = (uint8_t)(loss_word >> 24);
  block->cumulative_lost = loss_word & 0x00FFFFFFu;
  block->extended_seq = read_u32(data + 8);
  block->jitter = read_u32(data + 12);
  block->lsr = read_u32(data + 16);
  block->dlsr = read_u32(data + 20);
}

static uint32_t media_default_track_bitrate(turbo_rtc_media_track_type_t type,
                                            const turbo_media_track_config_t *config) {
  uint32_t bitrate_bps = 0;

  if (!config) {
    return (type == TURBO_RTC_MEDIA_TRACK_AUDIO) ? 64000u : 1000000u;
  }

  if (type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
    bitrate_bps = (uint32_t)config->audio.bitrate;
    if (bitrate_bps == 0) {
      bitrate_bps = 64000u;
    }
  } else {
    bitrate_bps = (uint32_t)config->video.bitrate;
    if (bitrate_bps == 0) {
      bitrate_bps = 1000000u;
    }
  }

  return bitrate_bps;
}

static void apply_track_target_bitrate(turbo_media_track_t *track, uint32_t bitrate_bps) {
  if (!track || !(track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) || bitrate_bps == 0) {
    return;
  }

  track->current_target_bitrate = bitrate_bps;
  if (track->type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
    track->config.audio.bitrate = (int)bitrate_bps;
  } else {
    track->config.video.bitrate = (int)bitrate_bps;
  }
  track->stats.bitrate_bps = bitrate_bps;

  if (track->twcc_tracker) {
    twcc_tracker_set_bitrate(track->twcc_tracker, bitrate_bps);
  }

  if (track->encoder) {
    turbo_codec_set_bitrate((turbo_codec_t *)track->encoder, (int)bitrate_bps);
  }
}

static size_t media_decoded_frame_capacity(const turbo_media_track_t *track) {
  size_t capacity = TURBO_CODEC_MAX_FRAME_SIZE;

  if (track && track->type == TURBO_RTC_MEDIA_TRACK_VIDEO && track->config.video.width > 0 &&
      track->config.video.height > 0) {
    size_t pixels = (size_t)track->config.video.width * (size_t)track->config.video.height;
    size_t i420_size = pixels + (pixels / 2);
    if (i420_size > capacity) {
      capacity = i420_size;
    }
  }

  return capacity;
}

static const char *media_audio_codec_name(turbo_codec_type_t codec_type) {
  switch (codec_type) {
  case TURBO_CODEC_PCMU:
    return "pcmu";
  case TURBO_CODEC_PCMA:
    return "pcma";
  case TURBO_CODEC_OPUS:
    return "opus";
  default:
    return NULL;
  }
}

static const char *media_video_codec_name(turbo_codec_type_t codec_type) {
  switch (codec_type) {
  case TURBO_CODEC_VP8:
    return "vp8";
  case TURBO_CODEC_VP9:
    return "vp9";
  case TURBO_CODEC_H264:
    return "h264";
  case TURBO_CODEC_H265:
    return "h265";
  default:
    return NULL;
  }
}

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static void on_rtcp_packet(int type, const void *data, size_t len, void *user_data);
static void handle_rtcp_pli(turbo_media_context_t *ctx, const rtcp_pli_t *pli);
static void handle_rtcp_fir(turbo_media_context_t *ctx, const rtcp_fir_t *fir);
static void handle_rtcp_nack(turbo_media_context_t *ctx, const void *data, size_t len);
static void handle_rtcp_twcc(turbo_media_context_t *ctx, const rtcp_twcc_t *twcc);
static void send_rtcp_reports(turbo_media_context_t *ctx, uint64_t now);
static void send_twcc_feedback(turbo_media_context_t *ctx, uint64_t now);
static void process_jitter_buffers(turbo_media_context_t *ctx, uint64_t now);
static void send_nack_for_track(turbo_media_track_t *track, turbo_media_context_t *ctx,
                                uint64_t now);
static void handle_rtcp_sr(turbo_media_context_t *ctx, const rtcp_sr_t *sr);
static void handle_rtcp_rr(turbo_media_context_t *ctx, const rtcp_rr_block_t *block);
static void handle_rtcp_psfb(turbo_media_context_t *ctx, const void *data, size_t len);
static void handle_rtcp_remb(turbo_media_context_t *ctx, const void *data, size_t len);
static int init_encoder(turbo_media_track_t *track);
static int init_decoder(turbo_media_track_t *track);
static int start_capture_if_present(turbo_media_track_t *track);

static void on_transport_data(void *user_data, const uint8_t *data, size_t len) {
  turbo_media_context_t *ctx = (turbo_media_context_t *)user_data;

  if (!ctx || !data || len == 0) {
    return;
  }

  turbo_media_feed_data(ctx, data, len);
}

/* =============================================================================
 * Context Functions
 * ============================================================================= */

turbo_media_context_t *turbo_media_create(turbo_dc_peer_t *peer, void *user_data) {
  if (!peer) return NULL;

  turbo_codec_registry_init();

  turbo_media_context_t *ctx = (turbo_media_context_t *)calloc(1, sizeof(turbo_media_context_t));
  if (!ctx) return NULL;

  ctx->peer = peer;
  ctx->user_data = user_data;
  ctx->rtcp_interval_ms = 5000; /* Default 5 second RTCP interval */

  /* Initialize SRTP library */
  if (srtp_lib_init() != 0) {
    free(ctx);
    return NULL;
  }

  turbo_dc_peer_set_transport_data_handler(peer, on_transport_data, ctx);
  return ctx;
}

void turbo_media_destroy(turbo_media_context_t *ctx) {
  if (!ctx) return;

  /* Stop new transport callbacks before tearing down track state.
   * PeerConnection destroy can race with DTLS/SCTP/RTCP traffic still in flight. */
  if (ctx->peer) {
    turbo_dc_peer_set_transport_data_handler(ctx->peer, NULL, NULL);
  }

  /* Destroy all tracks */
  while (ctx->track_count > 0) {
    turbo_media_track_t *track = ctx->tracks[ctx->track_count - 1];
    if (!track) {
      ctx->track_count--;
      continue;
    }
    turbo_media_remove_track(track);
  }

  if (ctx->rtcp_session) {
    srtp_session_destroy(ctx->rtcp_session);
    ctx->rtcp_session = NULL;
  }

  free(ctx);
}

void *turbo_media_get_user_data(turbo_media_context_t *ctx) { return ctx ? ctx->user_data : NULL; }

int turbo_media_setup_srtp(turbo_media_context_t *ctx) {
  if (!ctx || !ctx->peer) return -1;

  srtp_keying_material_t material;
  uint16_t profile;
  int is_dtls_client;

  profile = turbo_dc_peer_get_srtp_keys(ctx->peer, &material);
  is_dtls_client = turbo_dc_peer_is_dtls_server(ctx->peer) ? 0 : 1;
  if (profile == 0) return -1;

  if (ctx->rtcp_session) {
    srtp_session_destroy(ctx->rtcp_session);
    ctx->rtcp_session = NULL;
  }
  {
    srtp_session_config_t rtcp_cfg = {
        .is_sender = 1, .is_dtls_client = is_dtls_client, .profile = profile, .keys = &material};
    ctx->rtcp_session = srtp_session_create(&rtcp_cfg);
    if (!ctx->rtcp_session) {
      return -1;
    }
  }

  /* Initialize SRTP session for each track */
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (track->srtp_session) {
      srtp_session_destroy(track->srtp_session);
    }

    srtp_session_config_t srtp_cfg = {
        .is_sender = (track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) ? 1 : 0,
        .is_dtls_client = is_dtls_client,
        .profile = profile,
        .keys = &material};
    track->srtp_session = srtp_session_create(&srtp_cfg);
    if (!track->srtp_session) {
      return -1;
    }
  }

  return 0;
}

int turbo_media_handle_timers(turbo_media_context_t *ctx) {
  if (!ctx) return 0;

  uint64_t now = get_time_ms();
  int next_timer = 1000; /* Default 1 second */

  /* Check if RTCP should be sent */
  if (now - ctx->last_rtcp_time >= ctx->rtcp_interval_ms) {
    send_rtcp_reports(ctx, now);
  }

  /* Send TWCC feedback for receive tracks */
  send_twcc_feedback(ctx, now);

  /* Process jitter buffers for receive tracks */
  process_jitter_buffers(ctx, now);

  return next_timer;
}

int turbo_media_get_track_count(turbo_media_context_t *ctx) { return ctx ? ctx->track_count : 0; }

turbo_media_track_t *turbo_media_get_track(turbo_media_context_t *ctx, int index) {
  if (!ctx || index < 0 || index >= ctx->track_count) {
    return NULL;
  }
  return ctx->tracks[index];
}

/* Send RTCP reports for all tracks */
static void send_rtcp_reports(turbo_media_context_t *ctx, uint64_t now) {
  uint8_t buf[RTP_MAX_PACKET];

  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (!track || track->state != TURBO_MEDIA_STATE_ACTIVE) continue;

    rtcp_compound_t rtcp;
    rtcp_compound_init(&rtcp, buf, sizeof(buf));

    if (track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
      /* Build Sender Report */
      rtcp_sr_t sr;
      rtp_session_build_sr(track->rtp_session, &sr);

      /* Collect report blocks for what we are receiving */
      rtcp_rr_block_t blocks[31];
      int block_count = 0;
      for (int j = 0; j < ctx->track_count && block_count < 31; j++) {
        turbo_media_track_t *recv_track = ctx->tracks[j];
        if (recv_track->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
          if (rtp_session_get_remote_ssrc(recv_track->rtp_session) != 0) {
            rtp_session_build_rr_block(recv_track->rtp_session, 0, &blocks[block_count++]);
          }
        }
      }
      rtcp_compound_add_sr(&rtcp, &sr, blocks, block_count);
    } else if (track->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
      /* If we only receive, we still send Receiver Reports */
      /* But avoid duplicate RRs if we already sent them with an SR above */
      int already_reported = 0;
      for (int j = 0; j < ctx->track_count; j++) {
        if (ctx->tracks[j]->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
          already_reported = 1;
          break;
        }
      }
      if (already_reported) continue;

      uint32_t reporter_ssrc = rtp_session_get_ssrc(track->rtp_session);
      rtcp_rr_block_t blocks[31];
      int block_count = 0;
      for (int j = 0; j < ctx->track_count && block_count < 31; j++) {
        turbo_media_track_t *recv_track = ctx->tracks[j];
        if (recv_track->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
          if (rtp_session_get_remote_ssrc(recv_track->rtp_session) != 0) {
            rtp_session_build_rr_block(recv_track->rtp_session, 0, &blocks[block_count++]);
          }
        }
      }
      if (block_count > 0) {
        rtcp_compound_add_rr(&rtcp, reporter_ssrc, blocks, block_count);

        /* Also send REMB for video */
        if (track->type == TURBO_RTC_MEDIA_TRACK_VIDEO) {
          uint32_t remote_ssrc = rtp_session_get_remote_ssrc(track->rtp_session);
          if (remote_ssrc != 0) {
            rtcp_compound_add_remb(&rtcp, reporter_ssrc, remote_ssrc, 2000000); // 2 Mbps
          }
        }
      }
    }

    size_t len = rtcp_compound_finish(&rtcp);
    if (len > 0) {
      /* Protect and send */
      if (ctx->rtcp_session) {
        turbo_srtcp_protect(ctx->rtcp_session, buf, &len, sizeof(buf));
      }
      turbo_dc_peer_send_transport_data(ctx->peer, buf, len);
    }
  }
  ctx->last_rtcp_time = now;
}

/* Process all jitter buffers */
static void process_jitter_buffers(turbo_media_context_t *ctx, uint64_t now) {
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (!track || track->state != TURBO_MEDIA_STATE_ACTIVE) continue;
    if (!(track->direction & TURBO_MEDIA_DIRECTION_RECVONLY)) continue;

    if (track->jitter) {
      uint8_t payload_data[RTP_MAX_PACKET];
      uint8_t frame_data[TURBO_CODEC_MAX_FRAME_SIZE];
      size_t payload_len;
      uint32_t timestamp;
      int marker = 0;

      while (jitter_buffer_get_ex(track->jitter, payload_data, sizeof(payload_data), &payload_len,
                                  &timestamp, &marker, now) == 1) {
        size_t frame_len = payload_len;
        int frame_complete = 1;

        if (track->decoder) {
          turbo_codec_t *decoder = (turbo_codec_t *)track->decoder;
          size_t decoded_cap = media_decoded_frame_capacity(track);
          uint8_t *decoded = (uint8_t *)malloc(decoded_cap);
          size_t decoded_len = decoded_cap;

          if (!decoded) {
            break;
          }

          if (track->type == TURBO_RTC_MEDIA_TRACK_VIDEO && decoder->ops &&
              decoder->ops->depacketize) {
            frame_len = sizeof(frame_data);
            frame_complete = 0;
            if (decoder->ops->depacketize(decoder->decoder_ctx, payload_data, payload_len,
                                          frame_data, &frame_len,
                                          &frame_complete) != TURBO_CODEC_OK) {
              free(decoded);
              turbo_media_track_request_keyframe(track);
              continue;
            }

            if (!frame_complete && !marker) {
              free(decoded);
              continue;
            }
            if (marker) {
              frame_complete = 1;
            }
          } else {
            if (payload_len > sizeof(frame_data)) {
              free(decoded);
              continue;
            }
            memcpy(frame_data, payload_data, payload_len);
          }

          if (!frame_complete) {
            free(decoded);
            continue;
          }

          if (turbo_codec_decode((turbo_codec_t *)track->decoder, frame_data, frame_len, decoded,
                                 &decoded_len) == TURBO_CODEC_OK) {
            if (track->frame_cb) {
              track->frame_cb(track, decoded, decoded_len, timestamp, track->user_data);
            }
            track->stats.frames_recv++;
          } else if (track->type == TURBO_RTC_MEDIA_TRACK_VIDEO) {
            /* Decoding failed, likely due to packet loss, request keyframe */
            turbo_media_track_request_keyframe(track);
          }
          free(decoded);
        } else {
          /* No decoder, just deliver as is (might be pass-through) */
          if (track->frame_cb) {
            track->frame_cb(track, payload_data, payload_len, timestamp, track->user_data);
          }
          track->stats.frames_recv++;
        }
      }

      /* Check for missing packets (NACK) */
      if (now - track->last_nack_time >= 100) {
        send_nack_for_track(track, ctx, now);
      }
    }
  }
}

/* Send NACK for a specific track */
static void send_nack_for_track(turbo_media_track_t *track, turbo_media_context_t *ctx,
                                uint64_t now) {
  uint16_t missing[16];
  int missing_count = jitter_buffer_get_missing(track->jitter, missing, 16);
  if (missing_count > 0) {
    uint8_t nack_buf[RTP_MAX_PACKET];
    rtcp_compound_t rtcp;
    rtcp_compound_init(&rtcp, nack_buf, sizeof(nack_buf));

    uint32_t our_ssrc = 0;
    for (int j = 0; j < ctx->track_count; j++) {
      if (ctx->tracks[j]->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
        our_ssrc = rtp_session_get_ssrc(ctx->tracks[j]->rtp_session);
        break;
      }
    }

    uint32_t remote_ssrc = rtp_session_get_remote_ssrc(track->rtp_session);

    /* Group missing into PID/BLP */
    uint16_t pid = missing[0];
    uint16_t blp = 0;
    for (int j = 1; j < missing_count; j++) {
      int diff = missing[j] - pid - 1;
      if (diff >= 0 && diff < 16) {
        blp |= (1 << diff);
      }
    }

    rtcp_compound_add_nack(&rtcp, our_ssrc, remote_ssrc, pid, blp);
    size_t nack_len = rtcp_compound_finish(&rtcp);

    if (nack_len > 0) {
      if (ctx->rtcp_session) {
        turbo_srtcp_protect(ctx->rtcp_session, nack_buf, &nack_len, sizeof(nack_buf));
      }
      turbo_dc_peer_send_transport_data(ctx->peer, nack_buf, nack_len);
    }
    track->last_nack_time = now;
  }
}

int turbo_media_feed_data(turbo_media_context_t *ctx, const uint8_t *data, size_t len) {
  uint8_t packet_buf[RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN];
  const uint8_t *packet = data;
  size_t packet_len = len;
  uint8_t packet_type;
  int looks_rtcp;

  if (!ctx || !data || len < 12) return -1;
  if (len > sizeof(packet_buf)) return -1;
  packet_type = data[1];
  looks_rtcp = packet_type >= RTCP_SR && packet_type <= RTCP_PSFB;

  if (looks_rtcp) {
    if (ctx->rtcp_session) {
      size_t srtcp_len = len;
      memcpy(packet_buf, data, len);
      if (turbo_srtcp_unprotect(ctx->rtcp_session, packet_buf, &srtcp_len) == 0) {
#ifdef _WIN32
        /* Windows can raise SEH here when a bad or racing control packet
         * reaches the RTCP dispatcher. Treat it as a dropped feedback
         * packet instead of taking down the whole media pipeline. */
        __try {
#endif
          int parsed = rtcp_compound_parse(packet_buf, srtcp_len, on_rtcp_packet, ctx);
          if (parsed > 0) {
            return 0;
          }
#ifdef _WIN32
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          return 0;
        }
#endif
      } else {
        return 0;
      }
      return 0;
    }
#ifdef _WIN32
    __try {
#endif
      rtcp_compound_parse(data, len, on_rtcp_packet, ctx);
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0;
    }
#endif
    return 0;
  }

  rtp_packet_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.header.version = (uint8_t)((data[0] >> 6) & 0x03);
  pkt.header.payload_type = (uint8_t)(data[1] & 0x7F);
  pkt.header.sequence = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
  pkt.header.ssrc = read_u32(data + 8);
  if (pkt.header.version != RTP_VERSION) {
    return -1;
  }

  /* Find track for this SSRC */
  turbo_media_track_t *track = NULL;
  turbo_media_track_t *single_recv_track = NULL;
  int recv_track_count = 0;
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *t = ctx->tracks[i];
    if (t && (t->direction & TURBO_MEDIA_DIRECTION_RECVONLY)) {
      single_recv_track = t;
      recv_track_count++;
      uint32_t remote_ssrc = rtp_session_get_remote_ssrc(t->rtp_session);
      if (remote_ssrc != 0) {
        if (pkt.header.ssrc == remote_ssrc) {
          track = t;
          break;
        }
      } else {
        /* First packet, match by payload type as fallback */
        if (pkt.header.payload_type == t->payload_type) {
          track = t;
          /* Don't break yet, specific SSRC match is better */
        }
      }
    }
  }

  if (!track && recv_track_count == 1) {
    track = single_recv_track;
  }

  if (!track) return -1;

  /* Decrypt with SRTP if session exists */
  if (track->srtp_session) {
    size_t srtp_len = len;
    memcpy(packet_buf, data, len);
    if (turbo_srtp_unprotect(track->srtp_session, packet_buf, &srtp_len) != 0) {
      track->stats.packets_lost++;
      return -1;
    }
    if (rtp_packet_parse(&pkt, packet_buf, srtp_len) != 0) {
      track->stats.packets_lost++;
      return -1;
    }
    packet = packet_buf;
    packet_len = srtp_len;
  } else if (rtp_packet_parse(&pkt, packet, packet_len) != 0) {
    track->stats.packets_lost++;
    return -1;
  }

  if (rtp_session_recv(track->rtp_session, &pkt) != 0) {
    track->stats.packets_lost++;
    return -1;
  }
  track->stats.packets_recv++;
  track->stats.bytes_recv += pkt.payload_len;

  if (pkt.payload_len == 0) {
    return 0;
  }

  /* Extract TWCC sequence number if present */
  if (track->twcc_receiver && track->transport_cc_ext_id > 0 && pkt.header.extension) {
    uint8_t twcc_data[2];
    uint8_t twcc_len;
    if (rtp_packet_get_extension(&pkt, (uint8_t)track->transport_cc_ext_id, twcc_data, &twcc_len) ==
            0 &&
        twcc_len == 2) {
      uint16_t twcc_seq = ((uint16_t)twcc_data[0] << 8) | twcc_data[1];
      uint64_t arrival_time_us = get_time_ms() * 1000;
      twcc_receiver_register_packet(track->twcc_receiver, twcc_seq, arrival_time_us);
    }
  }

  if (track->rtp_packet_cb) {
    track->rtp_packet_cb(track, packet, packet_len, track->user_data);
  }

  if (!track->jitter) return -1;

  /* Insert into jitter buffer */
  return jitter_buffer_put(track->jitter, &pkt, get_time_ms());
}

static void handle_rtcp_pli(turbo_media_context_t *ctx, const rtcp_pli_t *pli) {
  /* Find SENDONLY track by media SSRC */
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
      if (rtp_session_get_ssrc(track->rtp_session) == pli->media_ssrc) {
        if (track->encoder) {
          turbo_codec_request_keyframe((turbo_codec_t *)track->encoder);
        }
        break;
      }
    }
  }
}

static void handle_rtcp_fir(turbo_media_context_t *ctx, const rtcp_fir_t *fir) {
  /* Similar to PLI */
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
      if (rtp_session_get_ssrc(track->rtp_session) == fir->media_ssrc) {
        if (track->encoder) {
          turbo_codec_request_keyframe((turbo_codec_t *)track->encoder);
        }
        break;
      }
    }
  }
}

static void on_rtcp_packet(int type, const void *data, size_t len, void *user_data) {
  turbo_media_context_t *ctx = (turbo_media_context_t *)user_data;
  const uint8_t *packet = (const uint8_t *)data;

  if (!packet || len < 4) {
    return;
  }

  switch (type) {
  case RTCP_SR: {
    rtcp_sr_t sr;

    if (len < 28) {
      break;
    }

    memset(&sr, 0, sizeof(sr));
    sr.ssrc = read_u32(packet + 4);
    sr.ntp_sec = read_u32(packet + 8);
    sr.ntp_frac = read_u32(packet + 12);
    sr.rtp_ts = read_u32(packet + 16);
    sr.packet_count = read_u32(packet + 20);
    sr.octet_count = read_u32(packet + 24);
    handle_rtcp_sr(ctx, &sr);
    break;
  }

  case RTCP_RR: {
    uint8_t block_count = packet[0] & 0x1F;

    if (len < 8 + ((size_t)block_count * 24u)) {
      break;
    }

    for (uint8_t i = 0; i < block_count; ++i) {
      rtcp_rr_block_t block;
      parse_rtcp_rr_block(packet + 8 + ((size_t)i * 24u), &block);
      handle_rtcp_rr(ctx, &block);
    }
    break;
  }
  case RTCP_RTPFB: {
    uint8_t fmt = packet[0] & 0x1F;
    if (fmt == RTCP_NACK) {
      handle_rtcp_nack(ctx, data, len);
    } else if (fmt == 15) { /* TWCC feedback */
      rtcp_twcc_t *twcc = (rtcp_twcc_t *)calloc(1, sizeof(*twcc));
      if (twcc) {
        if (rtcp_parse_twcc(packet, len, twcc) == 0) {
          handle_rtcp_twcc(ctx, twcc);
        }
        free(twcc);
      }
    }
    break;
  }
  case RTCP_PSFB: {
    handle_rtcp_psfb(ctx, data, len);
    break;
  }
  }
}

/* Handle RTCP Sender Report */
static void handle_rtcp_sr(turbo_media_context_t *ctx, const rtcp_sr_t *sr) {
  /* Find RECVONLY track by sender SSRC */
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (track->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
      if (rtp_session_get_remote_ssrc(track->rtp_session) == sr->ssrc) {
        rtp_session_process_sr(track->rtp_session, sr);
        break;
      }
    }
  }
}

/* Handle RTCP Receiver Report */
static void handle_rtcp_rr(turbo_media_context_t *ctx, const rtcp_rr_block_t *block) {
  if (!ctx || !block) {
    return;
  }

  /* Find SENDONLY track by source SSRC */
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
      if (rtp_session_get_ssrc(track->rtp_session) == block->ssrc) {
        rtp_session_process_rr(track->rtp_session, block);
        break;
      }
    }
  }
}

/* Handle RTCP PSFB (Payload-Specific Feedback) */
static void handle_rtcp_psfb(turbo_media_context_t *ctx, const void *data, size_t len) {
  uint8_t fmt = ((const uint8_t *)data)[0] & 0x1F;
  if (fmt == RTCP_PLI) {
    rtcp_pli_t pli;
    pli.sender_ssrc = read_u32((const uint8_t *)data + 4);
    pli.media_ssrc = read_u32((const uint8_t *)data + 8);
    handle_rtcp_pli(ctx, &pli);
  } else if (fmt == RTCP_FIR) {
    rtcp_fir_t fir;
    fir.sender_ssrc = read_u32((const uint8_t *)data + 4);
    fir.media_ssrc = read_u32((const uint8_t *)data + 8);
    handle_rtcp_fir(ctx, &fir);
  } else if (fmt == RTCP_REMB) {
    handle_rtcp_remb(ctx, data, len);
  }
}

/* Handle RTCP REMB (Receiver Estimated Maximum Bitrate) */
static void handle_rtcp_remb(turbo_media_context_t *ctx, const void *data, size_t len) {
  const uint8_t *packet = (const uint8_t *)data;
  uint8_t num_ssrcs;
  uint8_t exp;
  uint32_t mantissa;
  uint32_t bitrate;
  uint64_t bitrate64;
  size_t ssrcs_offset = 20;
  int matched = 0;

  if (!ctx || !packet || len < 24) {
    return;
  }

  if (packet[12] != 'R' || packet[13] != 'E' || packet[14] != 'M' || packet[15] != 'B') {
    return;
  }

  num_ssrcs = packet[16];
  if (num_ssrcs == 0 || len < ssrcs_offset + ((size_t)num_ssrcs * 4U)) {
    return;
  }

  exp = (uint8_t)((packet[17] >> 2) & 0x3FU);
  mantissa =
      (((uint32_t)packet[17] & 0x03U) << 16) | ((uint32_t)packet[18] << 8) | (uint32_t)packet[19];
  bitrate64 = ((uint64_t)mantissa) << exp;
  bitrate = bitrate64 > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)bitrate64;
  if (bitrate == 0) {
    return;
  }

  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    uint32_t track_ssrc;

    if (!track || !(track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) ||
        track->type != TURBO_RTC_MEDIA_TRACK_VIDEO || !track->rtp_session) {
      continue;
    }

    track_ssrc = rtp_session_get_ssrc(track->rtp_session);
    for (uint8_t j = 0; j < num_ssrcs; ++j) {
      uint32_t remb_ssrc = read_u32(packet + ssrcs_offset + ((size_t)j * 4U));
      if (track_ssrc == remb_ssrc) {
        apply_track_target_bitrate(track, bitrate);
        matched = 1;
        break;
      }
    }
  }

  if (!matched && num_ssrcs == 1 && read_u32(packet + ssrcs_offset) == 0) {
    for (int i = 0; i < ctx->track_count; i++) {
      turbo_media_track_t *track = ctx->tracks[i];
      if (track->direction & TURBO_MEDIA_DIRECTION_SENDONLY &&
          track->type == TURBO_RTC_MEDIA_TRACK_VIDEO) {
        apply_track_target_bitrate(track, bitrate);
      }
    }
  }
}

static int capture_device_id(turbo_capture_type_t type, int device_index, char *device_id,
                             size_t device_id_len) {
  device_id[0] = '\0';
  if (device_index < 0) {
    return 0;
  }

#ifdef _WIN32
  snprintf(device_id, device_id_len, "%d", device_index);
  return 0;
#else
  turbo_capture_device_t devices[TURBO_CAPTURE_MAX_DEVICES];
  int count;

  if (type == TURBO_CAPTURE_TYPE_AUDIO) {
    count = turbo_capture_list_audio_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
  } else {
    count = turbo_capture_list_video_devices(devices, TURBO_CAPTURE_MAX_DEVICES);
  }
  if (count <= device_index) {
    return -1;
  }

  strncpy(device_id, devices[device_index].id, device_id_len - 1);
  device_id[device_id_len - 1] = '\0';
  return 0;
#endif
}

/* =============================================================================
 * Audio Capture Callbacks
 * ============================================================================= */

static void on_audio_captured(turbo_capture_t *capture, const uint8_t *samples, size_t len,
                              uint64_t timestamp, void *user_data) {
  turbo_media_track_t *track = (turbo_media_track_t *)user_data;
  if (!track || track->state != TURBO_MEDIA_STATE_ACTIVE) return;

  (void)timestamp;

  /* Capture clocks are microseconds; RTP pacing is owned by the track. */
  turbo_media_track_send_frame(track, samples, len, 0);
}

static void on_video_captured(turbo_capture_t *capture, const uint8_t *frame, size_t len, int width,
                              int height, uint64_t timestamp, void *user_data) {
  turbo_media_track_t *track = (turbo_media_track_t *)user_data;
  if (!track || track->state != TURBO_MEDIA_STATE_ACTIVE) return;

  (void)timestamp;
  (void)width;
  (void)height;

  /* Capture clocks are microseconds; RTP pacing is owned by the track. */
  turbo_media_track_send_frame(track, frame, len, 0);
}

/* =============================================================================
 * Track Functions
 * ============================================================================= */

turbo_media_track_t *turbo_media_add_track(turbo_media_context_t *ctx,
                                           const turbo_media_track_config_t *config) {
  if (!ctx || !config) return NULL;
  if (ctx->track_count >= TURBO_MEDIA_MAX_TRACKS) return NULL;
  if (config->type != TURBO_RTC_MEDIA_TRACK_AUDIO && config->type != TURBO_RTC_MEDIA_TRACK_VIDEO)
    return NULL;
  if (config->direction < TURBO_MEDIA_DIRECTION_SENDONLY ||
      config->direction > TURBO_MEDIA_DIRECTION_SENDRECV)
    return NULL;

  turbo_media_track_t *track = (turbo_media_track_t *)calloc(1, sizeof(turbo_media_track_t));
  if (!track) return NULL;

  track->ctx = ctx;
  track->type = config->type;
  track->direction = config->direction;
  track->codec_type = config->codec;
  track->payload_type = (uint8_t)config->codec;
  track->state = TURBO_MEDIA_STATE_IDLE;

  /* Copy config */
  if (config->type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
    memcpy(&track->config.audio, &config->audio, sizeof(turbo_audio_config_t));
  } else {
    memcpy(&track->config.video, &config->video, sizeof(turbo_video_config_t));
  }

  /* Create RTP session */
  rtp_session_config_t rtp_cfg = {.ssrc = 0, /* Will be generated */
                                  .payload_type = track->payload_type,
                                  .clock_rate = (config->type == TURBO_RTC_MEDIA_TRACK_AUDIO)
                                                    ? RTP_CLOCK_AUDIO
                                                    : RTP_CLOCK_VIDEO,
                                  .is_audio = (config->type == TURBO_RTC_MEDIA_TRACK_AUDIO)};
  track->rtp_session = rtp_session_create(&rtp_cfg);
  if (!track->rtp_session) goto fail;

  /* Create TWCC tracker for send tracks */
  if (config->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
    track->twcc_tracker = twcc_tracker_create();
    if (!track->twcc_tracker) goto fail;
    apply_track_target_bitrate(track, media_default_track_bitrate(config->type, config));
  }

  /* Create TWCC receiver for receive tracks */
  if (config->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
    uint32_t ssrc = rtp_session_get_ssrc(track->rtp_session);
    track->twcc_receiver = twcc_receiver_create(ssrc);
    if (!track->twcc_receiver) goto fail;
    track->last_twcc_feedback_time = get_time_ms();
  }

  /* Create history for send tracks */
  if (config->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
    track->history = rtp_history_create(128, RTP_MAX_PACKET);
    if (!track->history) goto fail;
  }

  /* Create jitter buffer for receive tracks */
  if (config->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
    uint32_t delay =
        config->jitter_buffer_ms ? config->jitter_buffer_ms : TURBO_MEDIA_JITTER_DEFAULT;
    track->jitter = jitter_buffer_create(rtp_cfg.clock_rate, delay, 2048);
    if (!track->jitter) goto fail;
  }

  /* Add to context */
  ctx->tracks[ctx->track_count++] = track;

  return track;

fail:
  turbo_media_remove_track(track);
  return NULL;
}

turbo_media_track_t *turbo_media_add_screen_track(turbo_media_context_t *ctx, int screen_index,
                                                  int fps) {
  turbo_media_track_config_t config = {.type = TURBO_RTC_MEDIA_TRACK_VIDEO,
                                       .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
                                       .codec = TURBO_CODEC_VP8,
                                       .video = {.width = 1920,
                                                 .height = 1080,
                                                 .framerate = fps > 0 ? fps : 30,
                                                 .bitrate = 2000000,
                                                 .keyframe_interval = 60},
                                       .jitter_buffer_ms = 0};

  turbo_media_track_t *track = turbo_media_add_track(ctx, &config);
  if (track) {
    turbo_capture_config_t cap_cfg = {
        .type = TURBO_MEDIA_CAPTURE_SCREEN, .device_index = screen_index, .video = config.video};
    turbo_media_track_set_capture(track, &cap_cfg);
  }
  return track;
}

turbo_media_track_t *turbo_media_add_audio_track(turbo_media_context_t *ctx, int device_index) {
  turbo_media_track_config_t config = {
      .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
      .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
      .codec = TURBO_CODEC_OPUS,
      .audio = {.sample_rate = 48000, .channels = 2, .bitrate = 64000, .frame_size_ms = 20}};

  turbo_media_track_t *track = turbo_media_add_track(ctx, &config);
  if (track) {
    turbo_capture_config_t cap_cfg = {.type = TURBO_MEDIA_CAPTURE_MICROPHONE,
                                      .device_index = device_index,
                                      .audio = config.audio};
    turbo_media_track_set_capture(track, &cap_cfg);
  }
  return track;
}

turbo_media_track_t *turbo_media_add_video_track(turbo_media_context_t *ctx, int device_index) {
  turbo_media_track_config_t config = {.type = TURBO_RTC_MEDIA_TRACK_VIDEO,
                                       .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
                                       .codec = TURBO_CODEC_VP8,
                                       .video = {.width = 1280,
                                                 .height = 720,
                                                 .framerate = 30,
                                                 .bitrate = 2000000,
                                                 .keyframe_interval = 60}};

  turbo_media_track_t *track = turbo_media_add_track(ctx, &config);
  if (track) {
    turbo_capture_config_t cap_cfg = {
        .type = TURBO_MEDIA_CAPTURE_CAMERA, .device_index = device_index, .video = config.video};
    turbo_media_track_set_capture(track, &cap_cfg);
  }
  return track;
}

void turbo_media_remove_track(turbo_media_track_t *track) {
  if (!track) return;

  turbo_media_track_stop(track);

  /* Remove from context */
  turbo_media_context_t *ctx = track->ctx;
  for (int i = 0; i < ctx->track_count; i++) {
    if (ctx->tracks[i] == track) {
      /* Shift remaining tracks */
      for (int j = i; j < ctx->track_count - 1; j++) {
        ctx->tracks[j] = ctx->tracks[j + 1];
      }
      ctx->track_count--;
      break;
    }
  }

  /* Cleanup */
  if (track->rtp_session) {
    rtp_session_destroy(track->rtp_session);
  }
  if (track->jitter) {
    jitter_buffer_destroy(track->jitter);
  }
  if (track->srtp_session) {
    srtp_session_destroy(track->srtp_session);
  }
  if (track->twcc_tracker) {
    twcc_tracker_destroy(track->twcc_tracker);
  }
  if (track->twcc_receiver) {
    twcc_receiver_destroy(track->twcc_receiver);
  }
  if (track->capture) {
    turbo_capture_destroy((turbo_capture_t *)track->capture);
  }

  if (track->history) {
    rtp_history_destroy(track->history);
  }
  free(track);
}

int turbo_media_track_set_capture(turbo_media_track_t *track,
                                  const turbo_capture_config_t *config) {
  if (!track || !config) return -1;

  /* Destroy existing capture if any */
  if (track->capture) {
    turbo_capture_destroy((turbo_capture_t *)track->capture);
    track->capture = NULL;
  }

  if (config->type == TURBO_MEDIA_CAPTURE_MICROPHONE) {
    char device_id[128];
    turbo_audio_capture_config_t audio_cfg = {.sample_rate = config->audio.sample_rate,
                                              .channels = config->audio.channels,
                                              .bits_per_sample = 16, /* Standard for Opus input */
                                              .frame_size_ms = config->audio.frame_size_ms};

    if (capture_device_id(TURBO_CAPTURE_TYPE_AUDIO, config->device_index, device_id,
                          sizeof(device_id)) != 0) {
      return -1;
    }

    track->capture = turbo_audio_capture_create(device_id[0] ? device_id : NULL, &audio_cfg);
    if (!track->capture) return -1;

    turbo_audio_capture_set_callback((turbo_capture_t *)track->capture, on_audio_captured, track);
    return 0;
  }

  if (config->type == TURBO_MEDIA_CAPTURE_CAMERA) {
    char device_id[128];
    turbo_video_capture_config_t video_cfg = {.width = config->video.width,
                                              .height = config->video.height,
                                              .framerate = config->video.framerate,
                                              .format = 0};

    if (capture_device_id(TURBO_CAPTURE_TYPE_VIDEO, config->device_index, device_id,
                          sizeof(device_id)) != 0) {
      return -1;
    }

    track->capture = turbo_video_capture_create(device_id[0] ? device_id : NULL, &video_cfg);
    if (!track->capture) return -1;

    turbo_video_capture_set_callback((turbo_capture_t *)track->capture, on_video_captured, track);
    return 0;
  }

  if (config->type == TURBO_MEDIA_CAPTURE_SCREEN) {
    turbo_screen_capture_config_t screen_cfg = {.monitor_index = config->device_index,
                                                .framerate = config->video.framerate,
                                                .capture_cursor = 1,
                                                .capture_audio = 0};

    track->capture = turbo_screen_capture_create(&screen_cfg);
    if (!track->capture) return -1;

    turbo_screen_capture_set_callback((turbo_capture_t *)track->capture, on_video_captured, track);
    return 0;
  }

  return -1;
}

void turbo_media_track_on_frame(turbo_media_track_t *track, turbo_rtc_media_frame_cb cb) {
  if (track) track->frame_cb = cb;
}

void turbo_media_track_set_user_data(turbo_media_track_t *track, void *user_data) {
  if (track) track->user_data = user_data;
}

void *turbo_media_track_get_user_data(turbo_media_track_t *track) {
  return track ? track->user_data : NULL;
}

void turbo_media_track_on_state(turbo_media_track_t *track, turbo_media_state_cb cb) {
  if (track) track->state_cb = cb;
}

void turbo_media_track_on_keyframe_request(turbo_media_track_t *track, turbo_media_keyframe_cb cb) {
  if (track) track->keyframe_cb = cb;
}

void turbo_media_track_on_rtp_packet(turbo_media_track_t *track, turbo_media_rtp_packet_cb cb) {
  if (track) track->rtp_packet_cb = cb;
}

int turbo_media_track_start(turbo_media_track_t *track) {
  if (!track) return -1;
  if (track->state == TURBO_MEDIA_STATE_ACTIVE) return 0;

  track->state = TURBO_MEDIA_STATE_STARTING;

  /* Initialize encoder if not already done */
  if (track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
    if (init_encoder(track) != 0) {
      track->state = TURBO_MEDIA_STATE_ERROR;
      return -1;
    }

    /* Start capture if present */
    if (start_capture_if_present(track) != 0) {
      track->state = TURBO_MEDIA_STATE_ERROR;
      return -1;
    }
  }

  /* Initialize decoder if not already done */
  if (track->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
    if (init_decoder(track) != 0) {
      track->state = TURBO_MEDIA_STATE_ERROR;
      return -1;
    }
  }

  track->state = TURBO_MEDIA_STATE_ACTIVE;

  if ((track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) &&
      track->type == TURBO_RTC_MEDIA_TRACK_VIDEO && track->encoder) {
    turbo_codec_request_keyframe((turbo_codec_t *)track->encoder);
  }

  if (track->state_cb) {
    track->state_cb(track, track->state, track->user_data);
  }

  return 0;
}

/* Initialize encoder for send-only track */
static int init_encoder(turbo_media_track_t *track) {
  const char *codec_name;

  if (track->encoder) return 0;

  if (track->type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
    turbo_audio_codec_config_t cfg = {.sample_rate = track->config.audio.sample_rate,
                                      .channels = track->config.audio.channels,
                                      .bitrate = track->config.audio.bitrate,
                                      .frame_size_ms = track->config.audio.frame_size_ms,
                                      .enable_fec = track->config.audio.enable_fec,
                                      .enable_dtx = track->config.audio.enable_dtx,
                                      .complexity = 5};
    codec_name = media_audio_codec_name(track->codec_type);
    if (!codec_name) {
      return -1;
    }
    track->encoder = turbo_codec_create_encoder(codec_name, &cfg);
  } else if (track->type == TURBO_RTC_MEDIA_TRACK_VIDEO) {
    turbo_video_codec_config_t cfg = {.width = track->config.video.width,
                                      .height = track->config.video.height,
                                      .framerate = track->config.video.framerate,
                                      .bitrate = track->config.video.bitrate,
                                      .keyframe_interval = track->config.video.keyframe_interval,
                                      .threads = 0,
                                      .quality = 30};
    codec_name = media_video_codec_name(track->codec_type);
    if (!codec_name) {
      return -1;
    }
    track->encoder = turbo_codec_create_encoder(codec_name, &cfg);
  }

  return track->encoder ? 0 : -1;
}

/* Initialize decoder for recv-only track */
static int init_decoder(turbo_media_track_t *track) {
  const char *codec_name = NULL;

  if (track->decoder) return 0;

  if (track->type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
    turbo_audio_codec_config_t cfg = {
        .sample_rate = track->config.audio.sample_rate,
        .channels = track->config.audio.channels,
        .bitrate = track->config.audio.bitrate,
        .frame_size_ms = track->config.audio.frame_size_ms,
        .enable_fec = track->config.audio.enable_fec,
        .enable_dtx = track->config.audio.enable_dtx,
        .complexity = 5};
    codec_name = media_audio_codec_name(track->codec_type);
    if (!codec_name) {
      return -1;
    }
    track->decoder = turbo_codec_create_decoder(codec_name, &cfg);
  } else if (track->type == TURBO_RTC_MEDIA_TRACK_VIDEO) {
    turbo_video_codec_config_t cfg = {.width = track->config.video.width,
                                      .height = track->config.video.height};
    codec_name = media_video_codec_name(track->codec_type);
    if (!codec_name) {
      return -1;
    }
    track->decoder = turbo_codec_create_decoder(codec_name, &cfg);
  }

  return track->decoder ? 0 : -1;
}

/* Start capture if present */
static int start_capture_if_present(turbo_media_track_t *track) {
  if (!track->capture) return 0;

  return turbo_capture_start((turbo_capture_t *)track->capture);
}

void turbo_media_track_stop(turbo_media_track_t *track) {
  if (!track) return;
  if (track->state == TURBO_MEDIA_STATE_STOPPED) return;

  track->state = TURBO_MEDIA_STATE_STOPPING;

  /* Stop capture source */
  if (track->capture) {
    turbo_capture_stop((turbo_capture_t *)track->capture);
  }

  /* Destroy encoder/decoder */
  if (track->encoder) {
    turbo_codec_destroy((turbo_codec_t *)track->encoder);
    track->encoder = NULL;
  }
  if (track->decoder) {
    turbo_codec_destroy((turbo_codec_t *)track->decoder);
    track->decoder = NULL;
  }

  track->state = TURBO_MEDIA_STATE_STOPPED;

  if (track->state_cb) {
    track->state_cb(track, track->state, track->user_data);
  }
}

int turbo_media_track_send_frame(turbo_media_track_t *track, const uint8_t *data, size_t len,
                                 uint64_t timestamp) {
  if (!track || !data) return -1;
  if (track->state != TURBO_MEDIA_STATE_ACTIVE) return -1;
  if (!(track->direction & TURBO_MEDIA_DIRECTION_SENDONLY)) return -1;

  if (!track->encoder || !track->rtp_session) return -1;

  turbo_codec_t *codec = (turbo_codec_t *)track->encoder;
  uint8_t encoded[TURBO_CODEC_MAX_FRAME_SIZE];
  size_t encoded_len = sizeof(encoded);
  turbo_encoded_frame_t info;
  turbo_rtp_fragment_t fragments[TURBO_CODEC_MAX_PACKETS];
  int fragment_count = 0;
  uint64_t send_time_us = get_time_ms() * 1000;

  if (turbo_codec_encode(codec, data, len, encoded, &encoded_len, &info) != TURBO_CODEC_OK) {
    return -1;
  }

  memset(fragments, 0, sizeof(fragments));
  if (codec->ops && codec->ops->packetize) {
    fragment_count = codec->ops->packetize(codec->encoder_ctx, &info, fragments,
                                           TURBO_CODEC_MAX_PACKETS, MEDIA_RTP_PAYLOAD_MTU);
    if (fragment_count < 0) {
      return -1;
    }
  }

  if (fragment_count == 0) {
    fragments[0].data = encoded;
    fragments[0].len = encoded_len;
    fragments[0].marker = 1;
    fragments[0].fragment_start = 1;
    fragments[0].fragment_end = 1;
    fragment_count = 1;
  }

  for (int i = 0; i < fragment_count; i++) {
    uint8_t rtp_buf[RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN];
    rtp_packet_t pkt;
    int rtp_len;
    if (timestamp != 0) {
      rtp_len =
          rtp_session_send_with_timestamp(track->rtp_session, fragments[i].data, fragments[i].len,
                                          fragments[i].marker || fragments[i].fragment_end,
                                          (uint32_t)timestamp, &pkt, rtp_buf, sizeof(rtp_buf));
    } else {
      rtp_len = rtp_session_send(track->rtp_session, fragments[i].data, fragments[i].len,
                                 fragments[i].marker || fragments[i].fragment_end, &pkt, rtp_buf,
                                 sizeof(rtp_buf));
    }
    if (rtp_len <= 0) {
      continue;
    }

    if (track->twcc_tracker && track->transport_cc_ext_id > 0) {
      uint16_t twcc_seq =
          twcc_tracker_register_packet(track->twcc_tracker, (size_t)rtp_len + 8, send_time_us);
      uint8_t twcc_data[2];
      twcc_data[0] = (uint8_t)(twcc_seq >> 8);
      twcc_data[1] = (uint8_t)(twcc_seq & 0xFF);
      if (rtp_packet_add_extension(&pkt, (uint8_t)track->transport_cc_ext_id, twcc_data,
                                   sizeof(twcc_data)) == 0) {
        rtp_len = rtp_packet_serialize(&pkt, rtp_buf, sizeof(rtp_buf));
        if (rtp_len <= 0) {
          continue;
        }
      }
    }

    if (track->srtp_session) {
      size_t srtp_len = (size_t)rtp_len;
      if (turbo_srtp_protect(track->srtp_session, rtp_buf, &srtp_len, sizeof(rtp_buf)) != 0) {
        continue;
      }
      rtp_len = (int)srtp_len;
    }

    if (track->history) {
      rtp_history_put(track->history, pkt.header.sequence, rtp_buf, (size_t)rtp_len);
    }

    turbo_dc_peer_send_transport_data(track->ctx->peer, rtp_buf, (size_t)rtp_len);

    track->stats.packets_sent++;
    track->stats.bytes_sent += rtp_len;
  }

  if (track->type == TURBO_RTC_MEDIA_TRACK_AUDIO) {
    uint32_t samples =
        (uint32_t)(track->config.audio.sample_rate * track->config.audio.frame_size_ms / 1000);
    rtp_session_advance_timestamp(track->rtp_session, samples);
  } else {
    uint32_t ticks =
        (uint32_t)(RTP_CLOCK_VIDEO /
                   (track->config.video.framerate > 0 ? track->config.video.framerate : 30));
    if (ticks == 0) {
      ticks = RTP_CLOCK_VIDEO / 30;
    }
    rtp_session_advance_timestamp(track->rtp_session, ticks);
  }

  track->stats.frames_sent++;

  return 0;
}

int turbo_media_track_send_rtp_packet(turbo_media_track_t *track, const uint8_t *packet,
                                      size_t len) {
  rtp_packet_t incoming;
  rtp_packet_t outgoing;
  uint8_t rtp_buf[RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN];
  int rtp_len;

  if (!track || !packet) return -1;
  if (track->state != TURBO_MEDIA_STATE_ACTIVE) return -1;
  if (!(track->direction & TURBO_MEDIA_DIRECTION_SENDONLY)) return -1;
  if (!track->ctx || !track->ctx->peer || !track->rtp_session) return -1;

  memset(&incoming, 0, sizeof(incoming));
  if (rtp_packet_parse(&incoming, packet, len) != 0) {
    return -1;
  }

  rtp_len = rtp_session_send_with_timestamp(
      track->rtp_session, incoming.payload, incoming.payload_len, incoming.header.marker,
      incoming.header.timestamp, &outgoing, rtp_buf, sizeof(rtp_buf));
  if (rtp_len <= 0) {
    return -1;
  }

  if (track->srtp_session) {
    size_t srtp_len = (size_t)rtp_len;
    if (turbo_srtp_protect(track->srtp_session, rtp_buf, &srtp_len, sizeof(rtp_buf)) != 0) {
      return -1;
    }
    rtp_len = (int)srtp_len;
  }

  if (track->history) {
    rtp_history_put(track->history, outgoing.header.sequence, rtp_buf, (size_t)rtp_len);
  }

  turbo_dc_peer_send_transport_data(track->ctx->peer, rtp_buf, (size_t)rtp_len);
  track->stats.packets_sent++;
  track->stats.bytes_sent += (uint64_t)rtp_len;

  return 0;
}

void turbo_media_track_request_keyframe(turbo_media_track_t *track) {
  if (!track || !track->ctx) return;
  if (track->direction & TURBO_MEDIA_DIRECTION_RECVONLY) {
    /* We are receiving, send PLI to the remote sender */
    uint8_t buf[RTP_MAX_PACKET];
    rtcp_compound_t rtcp;
    rtcp_compound_init(&rtcp, buf, sizeof(buf));

    uint32_t our_ssrc = 0;
    /* Find any SENDONLY track to use its SSRC as reporter */
    for (int i = 0; i < track->ctx->track_count; i++) {
      if (track->ctx->tracks[i]->direction & TURBO_MEDIA_DIRECTION_SENDONLY) {
        our_ssrc = rtp_session_get_ssrc(track->ctx->tracks[i]->rtp_session);
        break;
      }
    }

    uint32_t remote_ssrc = rtp_session_get_remote_ssrc(track->rtp_session);
    if (remote_ssrc != 0) {
      rtcp_compound_add_pli(&rtcp, our_ssrc, remote_ssrc);
      size_t len = rtcp_compound_finish(&rtcp);

      /* Protect and send */
      if (track->ctx->rtcp_session) {
        turbo_srtcp_protect(track->ctx->rtcp_session, buf, &len, sizeof(buf));
      }
      turbo_dc_peer_send_transport_data(track->ctx->peer, buf, len);
    }
  } else {
    /* We are sending, if someone requested a keyframe from us, handled in RTCP parsing */
  }
}

void turbo_media_track_get_stats(turbo_media_track_t *track, turbo_media_stats_t *stats) {
  if (!track || !stats) return;
  memcpy(stats, &track->stats, sizeof(turbo_media_stats_t));
}

turbo_media_state_t turbo_media_track_get_state(turbo_media_track_t *track) {
  return track ? track->state : TURBO_MEDIA_STATE_IDLE;
}

turbo_rtc_media_track_type_t turbo_media_track_get_type(turbo_media_track_t *track) {
  return track ? track->type : TURBO_RTC_MEDIA_TRACK_AUDIO;
}

turbo_media_direction_t turbo_media_track_get_direction(turbo_media_track_t *track) {
  return track ? track->direction : TURBO_MEDIA_DIRECTION_SENDRECV;
}

turbo_codec_type_t turbo_media_track_get_codec(turbo_media_track_t *track) {
  return track ? track->codec_type : TURBO_CODEC_OPUS;
}

uint8_t turbo_media_track_get_payload_type(turbo_media_track_t *track) {
  return track ? track->payload_type : 0;
}

void turbo_media_track_set_payload_type(turbo_media_track_t *track, uint8_t payload_type) {
  if (!track) {
    return;
  }
  track->payload_type = payload_type;
  if (track->rtp_session) {
    rtp_session_set_payload_type(track->rtp_session, payload_type);
  }
}

uint32_t turbo_media_track_get_ssrc(turbo_media_track_t *track) {
  return track && track->rtp_session ? rtp_session_get_ssrc(track->rtp_session) : 0;
}

uint32_t turbo_media_track_get_remote_ssrc(turbo_media_track_t *track) {
  return track && track->rtp_session ? rtp_session_get_remote_ssrc(track->rtp_session) : 0;
}

void turbo_media_track_set_remote_ssrc(turbo_media_track_t *track, uint32_t ssrc) {
  if (track && track->rtp_session) {
    rtp_session_set_remote_ssrc(track->rtp_session, ssrc);
  }
}

void turbo_media_track_set_transport_cc_ext_id(turbo_media_track_t *track, int ext_id) {
  if (!track) {
    return;
  }
  track->transport_cc_ext_id = (ext_id >= 1 && ext_id <= 14) ? ext_id : 0;
}

int turbo_media_track_get_transport_cc_ext_id(turbo_media_track_t *track) {
  return track ? track->transport_cc_ext_id : 0;
}

/* =============================================================================
 * Device Enumeration (Stubs)
 * ============================================================================= */

int turbo_media_list_audio_inputs(turbo_media_device_t *devices, int max_count) {
  turbo_capture_device_t cap_devices[TURBO_CAPTURE_MAX_DEVICES];
  int count = turbo_capture_list_audio_devices(
      cap_devices, max_count < TURBO_CAPTURE_MAX_DEVICES ? max_count : TURBO_CAPTURE_MAX_DEVICES);
  if (count < 0) return 0;

  for (int i = 0; i < count; i++) {
    devices[i].index = cap_devices[i].index;
    strncpy(devices[i].name, cap_devices[i].name, sizeof(devices[i].name) - 1);
    strncpy(devices[i].id, cap_devices[i].id, sizeof(devices[i].id) - 1);
    devices[i].is_default = cap_devices[i].is_default;
  }
  return count;
}

int turbo_media_list_video_inputs(turbo_media_device_t *devices, int max_count) {
  turbo_capture_device_t cap_devices[TURBO_CAPTURE_MAX_DEVICES];
  int count = turbo_capture_list_video_devices(
      cap_devices, max_count < TURBO_CAPTURE_MAX_DEVICES ? max_count : TURBO_CAPTURE_MAX_DEVICES);
  if (count < 0) return 0;

  for (int i = 0; i < count; i++) {
    devices[i].index = cap_devices[i].index;
    strncpy(devices[i].name, cap_devices[i].name, sizeof(devices[i].name) - 1);
    strncpy(devices[i].id, cap_devices[i].id, sizeof(devices[i].id) - 1);
    devices[i].is_default = cap_devices[i].is_default;
  }
  return count;
}

int turbo_media_list_screens(turbo_media_device_t *devices, int max_count) {
  turbo_capture_device_t cap_devices[TURBO_CAPTURE_MAX_DEVICES];
  int count = turbo_capture_list_screens(
      cap_devices, max_count < TURBO_CAPTURE_MAX_DEVICES ? max_count : TURBO_CAPTURE_MAX_DEVICES);
  if (count < 0) return 0;

  for (int i = 0; i < count; i++) {
    devices[i].index = cap_devices[i].index;
    strncpy(devices[i].name, cap_devices[i].name, sizeof(devices[i].name) - 1);
    strncpy(devices[i].id, cap_devices[i].id, sizeof(devices[i].id) - 1);
    devices[i].is_default = cap_devices[i].is_default;
  }
  return count;
}

/* Handle incoming RTCP NACK (retransmission request) */
static void handle_rtcp_nack(turbo_media_context_t *ctx, const void *data, size_t len) {
  uint32_t sender_ssrc = 0;
  uint32_t media_ssrc = 0;
  uint16_t seq_nums[64];
  int seq_count = 0;

  if (!ctx || !data || len < 16) return;
  if (nack_parse_rtcp((const uint8_t *)data, len, &sender_ssrc, &media_ssrc, seq_nums, &seq_count,
                      (int)(sizeof(seq_nums) / sizeof(seq_nums[0]))) != 0) {
    return;
  }
  if (seq_count <= 0) {
    return;
  }

  /* Find matching track */
  turbo_media_track_t *track = NULL;
  for (int i = 0; i < ctx->track_count; i++) {
    if (ctx->tracks[i] && (ctx->tracks[i]->direction & TURBO_MEDIA_DIRECTION_SENDONLY)) {
      if (rtp_session_get_ssrc(ctx->tracks[i]->rtp_session) == media_ssrc) {
        track = ctx->tracks[i];
        break;
      }
    }
  }

  if (!track || !track->history) return;

  uint8_t rtp_buf[RTP_MAX_PACKET];
  size_t rtp_len;

  for (int i = 0; i < seq_count; i++) {
    rtp_len = sizeof(rtp_buf);
    if (rtp_history_get(track->history, seq_nums[i], rtp_buf, &rtp_len, sizeof(rtp_buf)) == 0) {
      turbo_dc_peer_send_transport_data(ctx->peer, rtp_buf, rtp_len);
    }
  }
}

/* =============================================================================
 * TWCC Functions
 * ============================================================================= */

/* Handle incoming TWCC feedback (sender side) */
static void handle_rtcp_twcc(turbo_media_context_t *ctx, const rtcp_twcc_t *twcc) {
  if (!ctx || !twcc) return;

  /* Find track by media SSRC */
  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (!track || !track->twcc_tracker) continue;

    if ((track->direction & TURBO_MEDIA_DIRECTION_SENDONLY) &&
        rtp_session_get_ssrc(track->rtp_session) == twcc->media_ssrc) {

      /* Process feedback and get bandwidth estimation */
      twcc_bwe_result_t bwe;
      if (twcc_tracker_process_feedback(track->twcc_tracker, twcc, &bwe) == 0) {
        apply_track_target_bitrate(track, bwe.target_bitrate_bps);

        /* Update stats */
        track->stats.bitrate_bps = bwe.estimated_bw_bps;
        track->stats.rtt_ms = bwe.rtt_ms;
        track->stats.fraction_lost = bwe.packet_loss_ratio;
      }
      break;
    }
  }
}

/* Send TWCC feedback (receiver side) */
static void send_twcc_feedback(turbo_media_context_t *ctx, uint64_t now) {
  if (!ctx) return;

  for (int i = 0; i < ctx->track_count; i++) {
    turbo_media_track_t *track = ctx->tracks[i];
    if (!track || !track->twcc_receiver) continue;

    /* Check if it's time to send feedback (every 100ms) */
    if (now - track->last_twcc_feedback_time < 100) continue;

    /* Generate TWCC feedback */
    rtcp_twcc_t *twcc = (rtcp_twcc_t *)calloc(1, sizeof(*twcc));
    int result;

    if (!twcc) {
      continue;
    }
    result = twcc_receiver_generate_feedback(track->twcc_receiver, twcc);

    if (result == 1) {
      /* Set media SSRC to remote sender's SSRC */
      twcc->media_ssrc = rtp_session_get_remote_ssrc(track->rtp_session);
      if (twcc->media_ssrc == 0) {
        free(twcc);
        continue;
      }

      /* Build RTCP compound packet with TWCC */
      uint8_t buf[RTP_MAX_PACKET];
      rtcp_compound_t rtcp;
      rtcp_compound_init(&rtcp, buf, sizeof(buf));

      /* Add TWCC feedback */
      rtcp_compound_add_twcc(&rtcp, twcc);

      size_t len = rtcp_compound_finish(&rtcp);

      if (len > 0) {
        /* Encrypt with SRTCP if session exists */
        if (ctx->rtcp_session) {
          size_t srtcp_len = len;
          if (turbo_srtcp_protect(ctx->rtcp_session, buf, &srtcp_len, sizeof(buf)) == 0) {
            len = srtcp_len;
          }
        }

        /* Send over ICE transport */
        turbo_dc_peer_send_transport_data(ctx->peer, buf, len);
      }

      track->last_twcc_feedback_time = now;
    }
    free(twcc);
  }
}
