/**
 * turbo_sdp.h - SDP (Session Description Protocol) for WebRTC
 *
 * Implements RFC 4566 (SDP) and WebRTC-specific extensions:
 * - RFC 5245 (ICE)
 * - RFC 5764 (DTLS-SRTP)
 * - RFC 8829 (WebRTC SDP)
 */

#ifndef TURBO_SDP_H
#define TURBO_SDP_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define SDP_MAX_MEDIA_SECTIONS      8
#define SDP_MAX_CODECS              16
#define SDP_MAX_CANDIDATES          32
#define SDP_MAX_FMTP_PARAMS         16
#define SDP_MAX_SSRCS               4
#define SDP_MAX_EXTENSIONS          8

/* =============================================================================
 * Types
 * ============================================================================= */

typedef enum {
    SDP_MEDIA_AUDIO = 0,
    SDP_MEDIA_VIDEO,
    SDP_MEDIA_APPLICATION,  /* DataChannel */
    SDP_MEDIA_UNKNOWN
} sdp_media_type_t;

typedef enum {
    SDP_DIRECTION_SENDRECV = 0,
    SDP_DIRECTION_SENDONLY,
    SDP_DIRECTION_RECVONLY,
    SDP_DIRECTION_INACTIVE
} sdp_direction_t;

typedef enum {
    SDP_ROLE_ACTPASS = 0,   /* Can be either */
    SDP_ROLE_ACTIVE,        /* DTLS client */
    SDP_ROLE_PASSIVE        /* DTLS server */
} sdp_setup_role_t;

/* Codec description */
typedef struct {
    int payload_type;
    char name[32];              /* e.g., "opus", "VP8", "H264" */
    int clock_rate;
    int channels;               /* For audio */

    /* FMTP parameters */
    char fmtp[256];             /* e.g., "minptime=10;useinbandfec=1" */

    /* RTX (retransmission) */
    int rtx_payload_type;       /* 0 if no RTX */

    /* Feedback */
    int supports_nack;
    int supports_pli;
    int supports_fir;
    int supports_remb;
    int supports_transport_cc;
} sdp_codec_t;

/* SSRC info */
typedef struct {
    uint32_t ssrc;
    char cname[64];
    char msid[128];             /* MediaStream ID */
    char track_id[64];
} sdp_ssrc_t;

/* RTP header extension */
typedef struct {
    int id;
    char uri[128];
} sdp_extension_t;

/* ICE candidate */
typedef struct {
    char foundation[32];
    int component;              /* 1 = RTP, 2 = RTCP */
    char transport[8];          /* "udp" or "tcp" */
    uint32_t priority;
    char address[64];
    uint16_t port;
    char type[16];              /* "host", "srflx", "prflx", "relay" */
    char rel_addr[64];          /* Related address (for srflx/relay) */
    uint16_t rel_port;
} sdp_candidate_t;

/* Media section (m= line and associated attributes) */
typedef struct {
    sdp_media_type_t type;
    uint16_t port;
    char protocol[32];          /* e.g., "UDP/TLS/RTP/SAVPF" */
    sdp_direction_t direction;

    /* ICE */
    char ice_ufrag[32];
    char ice_pwd[64];
    sdp_candidate_t candidates[SDP_MAX_CANDIDATES];
    int candidate_count;

    /* DTLS */
    sdp_setup_role_t setup;
    char fingerprint_hash[16];  /* "sha-256" */
    char fingerprint[128];      /* Hex fingerprint */

    /* Codecs */
    sdp_codec_t codecs[SDP_MAX_CODECS];
    int codec_count;

    /* SSRCs */
    sdp_ssrc_t ssrcs[SDP_MAX_SSRCS];
    int ssrc_count;

    /* RTP extensions */
    sdp_extension_t extensions[SDP_MAX_EXTENSIONS];
    int extension_count;

    /* Mid (media ID for BUNDLE) */
    char mid[16];

    /* RTCP-mux */
    int rtcp_mux;

    /* SCTP (for DataChannel) */
    int sctp_port;
    int max_message_size;
} sdp_media_t;

/* Complete SDP session */
typedef struct {
    /* Session-level */
    char session_id[32];
    uint64_t session_version;
    char username[64];

    /* Origin */
    char origin_addr[64];

    /* ICE options */
    int ice_lite;
    int ice_trickle;

    /* BUNDLE group */
    char bundle_mids[SDP_MAX_MEDIA_SECTIONS][16];
    int bundle_count;

    /* Media sections */
    sdp_media_t media[SDP_MAX_MEDIA_SECTIONS];
    int media_count;
} sdp_session_t;

/* =============================================================================
 * SDP Generation
 * ============================================================================= */

/**
 * Initialize SDP session with defaults
 */
CXX_C_API void sdp_session_init(sdp_session_t *sdp);

/**
 * Add audio media section
 *
 * @param sdp       SDP session
 * @param mid       Media ID (e.g., "0", "audio")
 * @param direction Send/recv direction
 * @return          Pointer to media section, or NULL on error
 */
CXX_C_API sdp_media_t *sdp_add_audio(sdp_session_t *sdp, const char *mid,
                            sdp_direction_t direction);

/**
 * Add video media section
 */
CXX_C_API sdp_media_t *sdp_add_video(sdp_session_t *sdp, const char *mid,
                            sdp_direction_t direction);

/**
 * Add DataChannel (application) media section
 */
CXX_C_API sdp_media_t *sdp_add_datachannel(sdp_session_t *sdp, const char *mid,
                                  int sctp_port);

/**
 * Add codec to media section
 */
CXX_C_API int sdp_media_add_codec(sdp_media_t *media, const sdp_codec_t *codec);

/**
 * Add SSRC to media section
 */
CXX_C_API int sdp_media_add_ssrc(sdp_media_t *media, uint32_t ssrc,
                        const char *cname, const char *msid);

/**
 * Add RTP header extension mapping to media section.
 */
CXX_C_API int sdp_media_add_extension(sdp_media_t *media, int id, const char *uri);

/**
 * Add ICE candidate to media section
 */
CXX_C_API int sdp_media_add_candidate(sdp_media_t *media, const sdp_candidate_t *candidate);

/**
 * Set ICE credentials for media section
 */
CXX_C_API void sdp_media_set_ice(sdp_media_t *media, const char *ufrag, const char *pwd);

/**
 * Set DTLS fingerprint for media section
 */
CXX_C_API void sdp_media_set_fingerprint(sdp_media_t *media, const char *hash,
                                const char *fingerprint);

/**
 * Generate SDP string from session
 *
 * @param sdp       SDP session
 * @param buffer    Output buffer
 * @param size      Buffer size
 * @return          Length of generated SDP, or -1 on error
 */
CXX_C_API int sdp_generate(const sdp_session_t *sdp, char *buffer, size_t size);

/**
 * Generate SDP offer
 */
CXX_C_API int sdp_generate_offer(const sdp_session_t *sdp, char *buffer, size_t size);

/**
 * Generate SDP answer based on offer
 */
CXX_C_API int sdp_generate_answer(const sdp_session_t *local, const sdp_session_t *remote,
                         char *buffer, size_t size);

/* =============================================================================
 * SDP Parsing
 * ============================================================================= */

/**
 * Parse SDP string into session structure
 *
 * @param sdp_str   SDP string
 * @param len       String length
 * @param sdp       Output session structure
 * @return          0 on success, -1 on error
 */
CXX_C_API int sdp_parse(const char *sdp_str, size_t len, sdp_session_t *sdp);

/**
 * Find media section by mid
 */
sdp_media_t *sdp_find_media_by_mid(sdp_session_t *sdp, const char *mid);

/**
 * Find media section by type
 */
sdp_media_t *sdp_find_media_by_type(sdp_session_t *sdp, sdp_media_type_t type);

/**
 * Find codec by payload type
 */
sdp_codec_t *sdp_find_codec_by_pt(sdp_media_t *media, int payload_type);

/**
 * Find codec by name
 */
sdp_codec_t *sdp_find_codec_by_name(sdp_media_t *media, const char *name);

/* =============================================================================
 * SDP Negotiation
 * ============================================================================= */

/**
 * Negotiate codecs between local and remote
 *
 * @param local     Local media section
 * @param remote    Remote media section
 * @param result    Output: negotiated codecs
 * @param max_count Maximum codecs to return
 * @return          Number of negotiated codecs
 */
int sdp_negotiate_codecs(const sdp_media_t *local, const sdp_media_t *remote,
                          sdp_codec_t *result, int max_count);

/**
 * Check if media sections are compatible
 */
int sdp_media_compatible(const sdp_media_t *local, const sdp_media_t *remote);

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

/**
 * Get direction string
 */
const char *sdp_direction_str(sdp_direction_t dir);

/**
 * Parse direction string
 */
sdp_direction_t sdp_parse_direction(const char *str);

/**
 * Parse direction string using re2c (with length)
 */
sdp_direction_t sdp_parse_direction_re2c(const char *str, size_t len);

/**
 * Parse media type string using re2c
 */
sdp_media_type_t sdp_parse_media_type_re2c(const char *str, size_t len);

/**
 * Parse setup role string using re2c
 */
sdp_setup_role_t sdp_parse_setup_role_re2c(const char *str, size_t len);

/**
 * Match codec name using re2c (case-insensitive)
 * Returns codec ID for known codecs, -1 for unknown
 */
int sdp_match_codec_name_re2c(const char *name, size_t len);

/**
 * Get media type string
 */
const char *sdp_media_type_str(sdp_media_type_t type);

/**
 * Generate random session ID
 */
void sdp_generate_session_id(char *buffer, size_t size);

/**
 * Generate ICE credentials with the TurboUtils system CSPRNG.
 * Both outputs are empty when arguments are invalid or entropy acquisition
 * fails. Each capacity includes the terminating NUL byte.
 */
void sdp_generate_ice_credentials(char *ufrag, size_t ufrag_size,
                                   char *pwd, size_t pwd_size);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SDP_H */
