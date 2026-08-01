/**
 * @file turbo_sdp.c
 * @brief SDP Generation and Utility Functions
 *
 * Implements SDP session/media creation and serialization.
 * Parsing is handled by turbo_sdp_parser.re (re2c generated).
 */

#include "turbo_sdp.h"
#include "platform.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> 

#ifdef _WIN32
  #define strcasecmp _stricmp
#endif

// Forward declarations for re2c functions
extern sdp_direction_t sdp_parse_direction_re2c(const char *str, size_t len);
extern sdp_media_type_t sdp_parse_media_type_re2c(const char *str, size_t len);
extern sdp_setup_role_t sdp_parse_setup_role_re2c(const char *str, size_t len);
extern int sdp_match_codec_name_re2c(const char *name, size_t len);

/* =============================================================================
 * Session Initialization
 * ============================================================================= */

void sdp_session_init(sdp_session_t *sdp) {
  memset(sdp, 0, sizeof(*sdp));

  /* Generate default session ID */
  sdp_generate_session_id(sdp->session_id, sizeof(sdp->session_id));
  sdp->session_version = (uint64_t)time(NULL);
  strcpy(sdp->username, "-");
  strcpy(sdp->origin_addr, "0.0.0.0");
}

/* =============================================================================
 * Media Section Creation
 * ============================================================================= */

sdp_media_t *sdp_add_audio(sdp_session_t *sdp, const char *mid, sdp_direction_t direction) {
  if (sdp->media_count >= SDP_MAX_MEDIA_SECTIONS)
    return NULL;

  sdp_media_t *media = &sdp->media[sdp->media_count++];
  memset(media, 0, sizeof(*media));

  media->type = SDP_MEDIA_AUDIO;
  media->port = 9;
  strcpy(media->protocol, "UDP/TLS/RTP/SAVPF");
  media->direction = direction;
  media->rtcp_mux = 1;

  if (mid) {
    strncpy(media->mid, mid, sizeof(media->mid) - 1);
    if (sdp->bundle_count < SDP_MAX_MEDIA_SECTIONS) {
      strncpy(sdp->bundle_mids[sdp->bundle_count++], mid, 15);
    }
  }

  return media;
}

sdp_media_t *sdp_add_video(sdp_session_t *sdp, const char *mid, sdp_direction_t direction) {
  if (sdp->media_count >= SDP_MAX_MEDIA_SECTIONS)
    return NULL;

  sdp_media_t *media = &sdp->media[sdp->media_count++];
  memset(media, 0, sizeof(*media));

  media->type = SDP_MEDIA_VIDEO;
  media->port = 9;
  strcpy(media->protocol, "UDP/TLS/RTP/SAVPF");
  media->direction = direction;
  media->rtcp_mux = 1;

  if (mid) {
    strncpy(media->mid, mid, sizeof(media->mid) - 1);
    if (sdp->bundle_count < SDP_MAX_MEDIA_SECTIONS) {
      strncpy(sdp->bundle_mids[sdp->bundle_count++], mid, 15);
    }
  }

  return media;
}

sdp_media_t *sdp_add_datachannel(sdp_session_t *sdp, const char *mid, int sctp_port) {
  if (sdp->media_count >= SDP_MAX_MEDIA_SECTIONS)
    return NULL;

  sdp_media_t *media = &sdp->media[sdp->media_count++];
  memset(media, 0, sizeof(*media));

  media->type = SDP_MEDIA_APPLICATION;
  media->port = 9;
  strcpy(media->protocol, "UDP/DTLS/SCTP");
  media->sctp_port = sctp_port;
  media->max_message_size = 262144;

  if (mid) {
    strncpy(media->mid, mid, sizeof(media->mid) - 1);
    if (sdp->bundle_count < SDP_MAX_MEDIA_SECTIONS) {
      strncpy(sdp->bundle_mids[sdp->bundle_count++], mid, 15);
    }
  }

  return media;
}

/* =============================================================================
 * Media Configuration
 * ============================================================================= */

int sdp_media_add_codec(sdp_media_t *media, const sdp_codec_t *codec) {
  if (media->codec_count >= SDP_MAX_CODECS)
    return -1;

  media->codecs[media->codec_count++] = *codec;
  return 0;
}

int sdp_media_add_ssrc(sdp_media_t *media, uint32_t ssrc, const char *cname, const char *msid) {
  if (media->ssrc_count >= SDP_MAX_SSRCS)
    return -1;

  sdp_ssrc_t *s = &media->ssrcs[media->ssrc_count++];
  s->ssrc = ssrc;
  if (cname)
    strncpy(s->cname, cname, sizeof(s->cname) - 1);
  if (msid)
    strncpy(s->msid, msid, sizeof(s->msid) - 1);

  return 0;
}

int sdp_media_add_extension(sdp_media_t *media, int id, const char *uri) {
  if (!media || !uri || id <= 0 || id > 14 || media->extension_count >= SDP_MAX_EXTENSIONS)
    return -1;

  sdp_extension_t *ext = &media->extensions[media->extension_count++];
  memset(ext, 0, sizeof(*ext));
  ext->id = id;
  strncpy(ext->uri, uri, sizeof(ext->uri) - 1);

  return 0;
}

int sdp_media_add_candidate(sdp_media_t *media, const sdp_candidate_t *candidate) {
  if (media->candidate_count >= SDP_MAX_CANDIDATES)
    return -1;

  media->candidates[media->candidate_count++] = *candidate;
  return 0;
}

void sdp_media_set_ice(sdp_media_t *media, const char *ufrag, const char *pwd) {
  if (ufrag)
    strncpy(media->ice_ufrag, ufrag, sizeof(media->ice_ufrag) - 1);
  if (pwd)
    strncpy(media->ice_pwd, pwd, sizeof(media->ice_pwd) - 1);
}

void sdp_media_set_fingerprint(sdp_media_t *media, const char *hash, const char *fingerprint) {
  if (hash)
    strncpy(media->fingerprint_hash, hash, sizeof(media->fingerprint_hash) - 1);
  if (fingerprint)
    strncpy(media->fingerprint, fingerprint, sizeof(media->fingerprint) - 1);
}

/* =============================================================================
 * SDP Generation
 * ============================================================================= */

static int sdp_appendf(char *buffer, size_t size, size_t *written, const char *format, ...) {
  va_list args;
  int needed;
  size_t remaining;

  if (!buffer || !written || !format || *written >= size)
    return -1;

  remaining = size - *written;
  va_start(args, format);
  needed = vsnprintf(buffer + *written, remaining, format, args);
  va_end(args);
  if (needed < 0 || (size_t)needed >= remaining)
    return -1;

  *written += (size_t)needed;
  return 0;
}

int sdp_generate(const sdp_session_t *sdp, char *buffer, size_t size) {
  size_t written = 0;

  if (!sdp || !buffer || size == 0)
    return -1;
  buffer[0] = '\0';

#define SDP_APPEND(...)                                                        \
  do {                                                                         \
    if (sdp_appendf(buffer, size, &written, __VA_ARGS__) != 0)                 \
      goto fail;                                                               \
  } while (0)

  /* Session-level fields */
  SDP_APPEND("v=0\r\n");
  SDP_APPEND("o=%s %s %llu IN IP4 %s\r\n", sdp->username, sdp->session_id,
             (unsigned long long)sdp->session_version, sdp->origin_addr);
  SDP_APPEND("s=-\r\n");
  SDP_APPEND("t=0 0\r\n");

  /* BUNDLE group */
  if (sdp->bundle_count > 0) {
    SDP_APPEND("a=group:BUNDLE");
    for (int i = 0; i < sdp->bundle_count; i++) {
      SDP_APPEND(" %s", sdp->bundle_mids[i]);
    }
    SDP_APPEND("\r\n");
  }

  /* Media sections */
  for (int m = 0; m < sdp->media_count; m++) {
    const sdp_media_t *media = &sdp->media[m];

    /* m= line */
    SDP_APPEND("m=%s %d %s", sdp_media_type_str(media->type), media->port, media->protocol);

    if (media->type == SDP_MEDIA_APPLICATION) {
      SDP_APPEND(" webrtc-datachannel\r\n");
    } else {
      for (int i = 0; i < media->codec_count; i++) {
        SDP_APPEND(" %d", media->codecs[i].payload_type);
      }
      SDP_APPEND("\r\n");
    }

    /* c= line */
    SDP_APPEND("c=IN IP4 0.0.0.0\r\n");

    /* ICE */
    if (media->ice_ufrag[0]) {
      SDP_APPEND("a=ice-ufrag:%s\r\n", media->ice_ufrag);
    }
    if (media->ice_pwd[0]) {
      SDP_APPEND("a=ice-pwd:%s\r\n", media->ice_pwd);
    }

    /* DTLS fingerprint */
    if (media->fingerprint[0]) {
      SDP_APPEND("a=fingerprint:%s %s\r\n", media->fingerprint_hash, media->fingerprint);
    }

    /* Setup role */
    const char *setup = media->setup == SDP_ROLE_ACTIVE    ? "active"
                        : media->setup == SDP_ROLE_PASSIVE ? "passive"
                                                           : "actpass";
    SDP_APPEND("a=setup:%s\r\n", setup);

    /* Mid */
    if (media->mid[0]) {
      SDP_APPEND("a=mid:%s\r\n", media->mid);
    }

    /* Direction */
    SDP_APPEND("a=%s\r\n", sdp_direction_str(media->direction));

    /* RTCP-mux */
    if (media->rtcp_mux) {
      SDP_APPEND("a=rtcp-mux\r\n");
    }

    /* RTP header extensions */
    for (int i = 0; i < media->extension_count; i++) {
      const sdp_extension_t *ext = &media->extensions[i];
      if (ext->id > 0 && ext->uri[0]) {
        SDP_APPEND("a=extmap:%d %s\r\n", ext->id, ext->uri);
      }
    }

    /* Codecs */
    for (int i = 0; i < media->codec_count; i++) {
      const sdp_codec_t *codec = &media->codecs[i];
      SDP_APPEND("a=rtpmap:%d %s/%d", codec->payload_type, codec->name, codec->clock_rate);
      if (codec->channels > 1) {
        SDP_APPEND("/%d", codec->channels);
      }
      SDP_APPEND("\r\n");

      if (codec->fmtp[0]) {
        SDP_APPEND("a=fmtp:%d %s\r\n", codec->payload_type, codec->fmtp);
      }

      if (codec->supports_nack) {
        SDP_APPEND("a=rtcp-fb:%d nack\r\n", codec->payload_type);
      }
      if (codec->supports_pli) {
        SDP_APPEND("a=rtcp-fb:%d nack pli\r\n", codec->payload_type);
      }
      if (codec->supports_fir) {
        SDP_APPEND("a=rtcp-fb:%d ccm fir\r\n", codec->payload_type);
      }
      if (codec->supports_remb) {
        SDP_APPEND("a=rtcp-fb:%d goog-remb\r\n", codec->payload_type);
      }
      if (codec->supports_transport_cc) {
        SDP_APPEND("a=rtcp-fb:%d transport-cc\r\n", codec->payload_type);
      }
    }

    /* SCTP (DataChannel) */
    if (media->type == SDP_MEDIA_APPLICATION) {
      SDP_APPEND("a=sctp-port:%d\r\n", media->sctp_port);
      if (media->max_message_size > 0) {
        SDP_APPEND("a=max-message-size:%d\r\n", media->max_message_size);
      }
    }

    /* ICE candidates */
    for (int i = 0; i < media->candidate_count; i++) {
      const sdp_candidate_t *cand = &media->candidates[i];
      SDP_APPEND("a=candidate:%s %d %s %u %s %hu typ %s", cand->foundation,
                 cand->component, cand->transport, cand->priority, cand->address,
                 cand->port, cand->type);
      if (cand->rel_addr[0]) {
        SDP_APPEND(" raddr %s rport %hu", cand->rel_addr, cand->rel_port);
      }
      SDP_APPEND("\r\n");
    }

    for (int i = 0; i < media->ssrc_count; i++) {
      const sdp_ssrc_t *ssrc = &media->ssrcs[i];

      if (ssrc->cname[0]) {
        SDP_APPEND("a=ssrc:%u cname:%s\r\n", ssrc->ssrc, ssrc->cname);
      }
      if (ssrc->msid[0]) {
        const char *track_id = ssrc->track_id[0] ? ssrc->track_id : ssrc->msid;
        SDP_APPEND("a=ssrc:%u msid:%s %s\r\n", ssrc->ssrc, ssrc->msid, track_id);
      }
    }
  }

#undef SDP_APPEND
  if (written > INT_MAX)
    goto fail;
  return (int)written;

fail:
  buffer[0] = '\0';
  return -1;
}

int sdp_generate_offer(const sdp_session_t *sdp, char *buffer, size_t size) {
  return sdp_generate(sdp, buffer, size);
}

int sdp_generate_answer(const sdp_session_t *local, const sdp_session_t *remote, char *buffer,
                        size_t size) {
  return sdp_generate(local, buffer, size);
}

/* =============================================================================
 * Query Functions
 * ============================================================================= */

sdp_media_t *sdp_find_media_by_mid(sdp_session_t *sdp, const char *mid) {
  for (int i = 0; i < sdp->media_count; i++) {
    if (strcmp(sdp->media[i].mid, mid) == 0)
      return &sdp->media[i];
  }
  return NULL;
}

sdp_media_t *sdp_find_media_by_type(sdp_session_t *sdp, sdp_media_type_t type) {
  for (int i = 0; i < sdp->media_count; i++) {
    if (sdp->media[i].type == type)
      return &sdp->media[i];
  }
  return NULL;
}

sdp_codec_t *sdp_find_codec_by_pt(sdp_media_t *media, int payload_type) {
  for (int i = 0; i < media->codec_count; i++) {
    if (media->codecs[i].payload_type == payload_type)
      return &media->codecs[i];
  }
  return NULL;
}

sdp_codec_t *sdp_find_codec_by_name(sdp_media_t *media, const char *name) {
  if (!media || !name)
    return NULL;

  size_t name_len = strlen(name);
  int target_id = sdp_match_codec_name_re2c(name, name_len);

  for (int i = 0; i < media->codec_count; i++) {
    // Use re2c for known codecs (fast path)
    if (target_id > 0) {
      int codec_id =
          sdp_match_codec_name_re2c(media->codecs[i].name, strlen(media->codecs[i].name));
      if (codec_id == target_id) {
        return &media->codecs[i];
      }
    } else {
      // Fallback to strcasecmp for unknown codecs
      if (strcasecmp(media->codecs[i].name, name) == 0)
        return &media->codecs[i];
    }
  }
  return NULL;
}

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

const char *sdp_direction_str(sdp_direction_t dir) {
  switch (dir) {
  case SDP_DIRECTION_SENDRECV:
    return "sendrecv";
  case SDP_DIRECTION_SENDONLY:
    return "sendonly";
  case SDP_DIRECTION_RECVONLY:
    return "recvonly";
  case SDP_DIRECTION_INACTIVE:
    return "inactive";
  default:
    return "sendrecv";
  }
}

sdp_direction_t sdp_parse_direction(const char *str) {
  if (!str)
    return SDP_DIRECTION_SENDRECV;
  return sdp_parse_direction_re2c(str, strlen(str));
}

const char *sdp_media_type_str(sdp_media_type_t type) {
  switch (type) {
  case SDP_MEDIA_AUDIO:
    return "audio";
  case SDP_MEDIA_VIDEO:
    return "video";
  case SDP_MEDIA_APPLICATION:
    return "application";
  default:
    return "unknown";
  }
}

void sdp_generate_session_id(char *buffer, size_t size) {
  int written;

  if (!buffer || size == 0)
    return;

  written = snprintf(buffer, size, "%llu", (unsigned long long)time(NULL));
  if (written < 0 || (size_t)written >= size)
    buffer[0] = '\0';
}

static int sdp_fill_ice_credential(char *output, size_t output_size) {
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  enum { RANDOM_BATCH_SIZE = 32, ACCEPT_LIMIT = 248 };
  uint8_t random_bytes[RANDOM_BATCH_SIZE];
  size_t random_offset = RANDOM_BATCH_SIZE;
  size_t output_offset = 0;

  if (!output || output_size < 2) return -1;

  while (output_offset + 1 < output_size) {
    uint8_t value;

    if (random_offset == RANDOM_BATCH_SIZE) {
      if (turbo_secure_random(random_bytes, sizeof(random_bytes)) != 0) {
        memset(random_bytes, 0, sizeof(random_bytes));
        return -1;
      }
      random_offset = 0;
    }

    value = random_bytes[random_offset++];
    if (value >= ACCEPT_LIMIT) continue;
    output[output_offset++] = alphabet[value % (sizeof(alphabet) - 1)];
  }

  output[output_offset] = '\0';
  memset(random_bytes, 0, sizeof(random_bytes));
  return 0;
}

void sdp_generate_ice_credentials(char *ufrag, size_t ufrag_size, char *pwd, size_t pwd_size) {
  if (ufrag && ufrag_size > 0) ufrag[0] = '\0';
  if (pwd && pwd_size > 0) pwd[0] = '\0';

  if (sdp_fill_ice_credential(ufrag, ufrag_size) != 0 ||
      sdp_fill_ice_credential(pwd, pwd_size) != 0) {
    if (ufrag && ufrag_size > 0) ufrag[0] = '\0';
    if (pwd && pwd_size > 0) pwd[0] = '\0';
  }
}

/* =============================================================================
 * Codec Negotiation
 * ============================================================================= */

int sdp_negotiate_codecs(const sdp_media_t *local, const sdp_media_t *remote, sdp_codec_t *result,
                         int max_count) {
  int count = 0;

  for (int i = 0; i < local->codec_count && count < max_count; i++) {
    int local_id = sdp_match_codec_name_re2c(local->codecs[i].name, strlen(local->codecs[i].name));

    for (int j = 0; j < remote->codec_count; j++) {
      int remote_id =
          sdp_match_codec_name_re2c(remote->codecs[j].name, strlen(remote->codecs[j].name));

      // Fast path: use re2c IDs for known codecs
      int names_match =
          (local_id > 0 && local_id == remote_id) ||
          (local_id <= 0 && strcasecmp(local->codecs[i].name, remote->codecs[j].name) == 0);

      if (names_match && local->codecs[i].clock_rate == remote->codecs[j].clock_rate) {
        result[count++] = local->codecs[i];
        break;
      }
    }
  }

  return count;
}

int sdp_media_compatible(const sdp_media_t *local, const sdp_media_t *remote) {
  if (local->type != remote->type)
    return 0;

  sdp_codec_t negotiated[SDP_MAX_CODECS];
  int count = sdp_negotiate_codecs(local, remote, negotiated, SDP_MAX_CODECS);

  return count > 0;
}
