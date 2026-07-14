/**
 * RTP Session Implementation
 *
 * Manages RTP session state, sequence numbers, timestamps, and statistics
 */
#include "turbo_rtp.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef NORPC
    #define NORPC
  #endif
  #ifndef NOSERVICE
    #define NOSERVICE
  #endif
  #include <platform.h>
#else
  #include <sys/time.h>
#endif

/* RTP Session internal structure */
struct rtp_session_s {
  /* Configuration */
  uint32_t ssrc;
  uint8_t payload_type;
  uint32_t clock_rate;
  int is_audio;

  /* Sender state */
  uint16_t sequence;
  uint32_t timestamp;
  uint64_t packets_sent;
  uint64_t octets_sent;

  /* Receiver state */
  uint32_t remote_ssrc;
  int received_first;
  uint16_t max_seq;
  uint32_t cycles;
  uint32_t base_seq;
  uint64_t packets_recv;
  uint64_t packets_lost;
  uint32_t jitter;
  uint32_t last_transit;

  /* RTCP timing */
  uint32_t last_sr_ntp;
  uint64_t last_sr_time_ms;
  uint32_t rtt_ms;
};

static atomic_uint_fast32_t g_ssrc_counter = 0x9E3779B9u;

/* Get current time in milliseconds */
static uint64_t get_time_ms(void) {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return t / 10000 - 11644473600000ULL;
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

/* Generate random SSRC */
static uint32_t generate_ssrc(void) {
  uint64_t now_ms = get_time_ms();
  uint32_t counter =
      (uint32_t)atomic_fetch_add_explicit(&g_ssrc_counter, 0x9E3779B9u, memory_order_relaxed);
  uint32_t ssrc = (uint32_t)now_ms ^ (uint32_t)(now_ms >> 32) ^ counter;

  ssrc ^= (uint32_t)(((uintptr_t)&counter) >> 4);
  if (ssrc == 0) {
    ssrc = counter ^ 0x13572468u;
  }
  return ssrc;
}

rtp_session_t *rtp_session_create(const rtp_session_config_t *config) {
  if (!config) return NULL;

  rtp_session_t *session = (rtp_session_t *)calloc(1, sizeof(rtp_session_t));
  if (!session) return NULL;

  session->ssrc = config->ssrc ? config->ssrc : generate_ssrc();
  session->payload_type = config->payload_type;
  session->clock_rate = config->clock_rate;
  session->is_audio = config->is_audio;

  /* Initialize sequence to random value */
  session->sequence = (uint16_t)(session->ssrc & 0xFFFF);

  /* Initialize timestamp to random value */
  session->timestamp = session->ssrc;

  return session;
}

void rtp_session_destroy(rtp_session_t *session) {
  if (session) {
    free(session);
  }
}

static int rtp_session_send_internal(rtp_session_t *session, const uint8_t *payload,
                                     size_t payload_len, int marker, uint32_t timestamp,
                                     rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len) {
  int result;

  if (!session || !pkt || !buffer) return -1;

  result = rtp_packet_build(pkt, session->payload_type, session->sequence, timestamp, session->ssrc,
                            marker, payload, payload_len, buffer, buffer_len);

  if (result > 0) {
    session->timestamp = timestamp;
    session->sequence++;
    session->packets_sent++;
    session->octets_sent += payload_len;
  }

  return result;
}

int rtp_session_send(rtp_session_t *session, const uint8_t *payload, size_t payload_len, int marker,
                     rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len) {
  return rtp_session_send_internal(session, payload, payload_len, marker,
                                   session ? session->timestamp : 0, pkt, buffer, buffer_len);
}

int rtp_session_send_with_timestamp(rtp_session_t *session, const uint8_t *payload,
                                    size_t payload_len, int marker, uint32_t timestamp,
                                    rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len) {
  return rtp_session_send_internal(session, payload, payload_len, marker, timestamp, pkt, buffer,
                                   buffer_len);
}

int rtp_session_recv(rtp_session_t *session, const rtp_packet_t *pkt) {
  if (!session || !pkt) return -1;

  uint16_t seq = pkt->header.sequence;
  uint32_t ts = pkt->header.timestamp;

  if (!session->received_first) {
    /* First packet from this source */
    session->received_first = 1;
    session->remote_ssrc = pkt->header.ssrc;
    session->base_seq = seq;
    session->max_seq = seq;
    session->cycles = 0;
    session->packets_recv = 0;
    session->packets_lost = 0;
    session->jitter = 0;
    session->last_transit = 0;
  } else {
    /* Check SSRC match */
    if (pkt->header.ssrc != session->remote_ssrc) {
      return -1; /* Different source */
    }

    /* Check for sequence wraparound */
    if (rtp_seq_newer(seq, session->max_seq)) {
      if (seq < session->max_seq) {
        session->cycles += RTP_SEQ_MOD;
      }
      session->max_seq = seq;
    }

    /* Calculate jitter (RFC 3550 A.8) */
    uint64_t time_ms = get_time_ms();
    uint32_t arrival = (uint32_t)(time_ms * session->clock_rate / 1000);
    uint32_t transit = arrival - ts;

    if (session->last_transit != 0) {
      int d = (int)(transit - session->last_transit);
      if (d < 0) d = -d;
      /* jitter = jitter + (|D| - jitter) / 16 */
      session->jitter += ((uint32_t)d - session->jitter) / 16;
    }
    session->last_transit = transit;
  }

  session->packets_recv++;
  return 0;
}

void rtp_session_advance_timestamp(rtp_session_t *session, uint32_t samples) {
  if (session) {
    session->timestamp += samples;
  }
}

int rtp_session_build_sr(rtp_session_t *session, rtcp_sr_t *sr) {
  if (!session || !sr) return -1;

  uint64_t now_ms = get_time_ms();

  /* Convert to NTP timestamp (seconds since 1900-01-01) */
  uint64_t ntp_sec = now_ms / 1000 + 2208988800ULL; /* Unix to NTP epoch */
  uint32_t ntp_frac = (uint32_t)((now_ms % 1000) * 4294967.296);

  sr->ssrc = session->ssrc;
  sr->ntp_sec = (uint32_t)ntp_sec;
  sr->ntp_frac = ntp_frac;
  sr->rtp_ts = session->timestamp;
  sr->packet_count = (uint32_t)session->packets_sent;
  sr->octet_count = (uint32_t)session->octets_sent;

  return 0;
}

int rtp_session_build_rr_block(rtp_session_t *session, uint32_t ssrc, rtcp_rr_block_t *block) {
  if (!session || !block) return -1;

  /* Calculate extended highest sequence */
  uint32_t extended_seq = session->cycles + session->max_seq;

  /* Calculate expected packets */
  uint32_t expected = extended_seq - session->base_seq + 1;

  /* Calculate lost packets */
  int64_t lost = (int64_t)expected - (int64_t)session->packets_recv;
  if (lost < 0) lost = 0;
  if (lost > 0x7FFFFF) lost = 0x7FFFFF; /* 24-bit signed max */

  /* Calculate fraction lost (8.8 fixed point) */
  uint8_t fraction = 0;
  if (expected > 0 && lost > 0) {
    fraction = (uint8_t)((lost * 256) / expected);
  }

  block->ssrc = ssrc ? ssrc : session->remote_ssrc;
  block->fraction_lost = fraction;
  block->cumulative_lost = (uint32_t)lost;
  block->extended_seq = extended_seq;
  block->jitter = session->jitter;

  /* LSR: middle 32 bits of last SR NTP timestamp */
  block->lsr = session->last_sr_ntp;

  /* DLSR: delay since last SR in 1/65536 seconds */
  if (session->last_sr_time_ms > 0) {
    uint64_t delay_ms = get_time_ms() - session->last_sr_time_ms;
    block->dlsr = (uint32_t)((delay_ms * 65536) / 1000);
  } else {
    block->dlsr = 0;
  }

  return 0;
}

void rtp_session_process_sr(rtp_session_t *session, const rtcp_sr_t *sr) {
  if (!session || !sr) return;

  /* Store middle 32 bits of NTP timestamp for LSR calculation */
  session->last_sr_ntp = ((sr->ntp_sec & 0xFFFF) << 16) | (sr->ntp_frac >> 16);
  session->last_sr_time_ms = get_time_ms();
}

void rtp_session_process_rr(rtp_session_t *session, const rtcp_rr_block_t *block) {
  if (!session || !block) return;

  /* Only process if it's about our SSRC */
  if (block->ssrc != session->ssrc) return;

  /* Calculate RTT if we have LSR/DLSR */
  if (block->lsr != 0) {
    uint32_t dlsr = block->dlsr;

    /* RTT = now - LSR - DLSR (all in 1/65536 seconds) */
    /* This is simplified - real impl needs NTP to local clock mapping */
    if (block->dlsr > 0) {
      session->rtt_ms = (dlsr * 1000) / 65536; /* Approximate */
    }
  }
}

void rtp_session_get_stats(const rtp_session_t *session, rtp_session_stats_t *stats) {
  if (!session || !stats) return;

  stats->packets_sent = session->packets_sent;
  stats->octets_sent = session->octets_sent;
  stats->packets_recv = session->packets_recv;
  stats->packets_lost = session->packets_lost;
  stats->jitter = session->jitter;
  stats->max_seq = session->max_seq;
  stats->cycles = session->cycles;
  stats->base_seq = session->base_seq;
  stats->last_sr_ntp = session->last_sr_ntp;
  stats->last_sr_time = session->last_sr_time_ms;
  stats->rtt_ms = session->rtt_ms;
}

uint32_t rtp_session_get_ssrc(const rtp_session_t *session) { return session ? session->ssrc : 0; }

uint32_t rtp_session_get_remote_ssrc(const rtp_session_t *session) {
  return session ? session->remote_ssrc : 0;
}

void rtp_session_set_remote_ssrc(rtp_session_t *session, uint32_t ssrc) {
  if (session) {
    session->remote_ssrc = ssrc;
  }
}

uint8_t rtp_session_get_payload_type(const rtp_session_t *session) {
  return session ? session->payload_type : 0;
}

void rtp_session_set_payload_type(rtp_session_t *session, uint8_t payload_type) {
  if (session) {
    session->payload_type = payload_type;
  }
}
