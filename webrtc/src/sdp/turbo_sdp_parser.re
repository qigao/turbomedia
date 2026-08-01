// re2c $INPUT -o $OUTPUT
/**
 * @file turbo_sdp_parser.re
 * @brief SDP Parser using re2c
 *
 * RFC 4566 (SDP) + WebRTC extensions parser using re2c state machine.
 * Follows the same pattern as mqtt_topic_parser.re for consistency.
 *
 * Build: re2c -o turbo_sdp_parser.c turbo_sdp_parser.re
 */

#include <stddef.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "turbo_sdp.h"

// Suppress MSVC warnings
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4101)
#pragma warning(disable: 4701)
#pragma warning(disable: 4703)
#endif

/* =============================================================================
 * Forward Declarations
 * ============================================================================= */

static int parse_origin_line(sdp_session_t *sdp, const char *line);
static int parse_media_line(sdp_session_t *sdp, const char *line, sdp_media_t **out_media);
static int parse_attribute_line(sdp_media_t *media, const char *line);
static int parse_rtpmap(sdp_media_t *media, const char *value);
static int parse_fmtp(sdp_media_t *media, const char *value);
static int parse_candidate(sdp_media_t *media, const char *value);
static int parse_fingerprint(sdp_media_t *media, const char *value);
static int parse_extmap(sdp_media_t *media, const char *value);

/* =============================================================================
 * re2c-based Utility Parsers
 * ============================================================================= */

/**
 * @brief Parse direction string to enum using re2c
 * @param str Direction string (sendrecv, sendonly, recvonly, inactive)
 * @param len Length of string
 * @return sdp_direction_t enum value
 */
sdp_direction_t sdp_parse_direction_re2c(const char *str, size_t len) {
    if (!str || len == 0 || len > 8) {
        return SDP_DIRECTION_SENDRECV;
    }

    const char *YYCURSOR = str;
    const char *YYLIMIT = str + len;
    const char *YYMARKER = str;

    /*!re2c
      re2c:define:YYCTYPE = "char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;

      "sendrecv" { return (YYCURSOR == YYLIMIT) ? SDP_DIRECTION_SENDRECV : SDP_DIRECTION_SENDRECV; }
      "sendonly" { return (YYCURSOR == YYLIMIT) ? SDP_DIRECTION_SENDONLY : SDP_DIRECTION_SENDRECV; }
      "recvonly" { return (YYCURSOR == YYLIMIT) ? SDP_DIRECTION_RECVONLY : SDP_DIRECTION_SENDRECV; }
      "inactive" { return (YYCURSOR == YYLIMIT) ? SDP_DIRECTION_INACTIVE : SDP_DIRECTION_SENDRECV; }
      *          { return SDP_DIRECTION_SENDRECV; }
    */
}

/**
 * @brief Parse media type string to enum using re2c
 * @param str Media type string (audio, video, application)
 * @param len Length of string
 * @return sdp_media_type_t enum value
 */
sdp_media_type_t sdp_parse_media_type_re2c(const char *str, size_t len) {
    if (!str || len == 0 || len > 11) {
        return SDP_MEDIA_UNKNOWN;
    }

    const char *YYCURSOR = str;
    const char *YYLIMIT = str + len;
    const char *YYMARKER = str;

    /*!re2c
      re2c:define:YYCTYPE = "char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;

      "audio"       { return (YYCURSOR == YYLIMIT) ? SDP_MEDIA_AUDIO : SDP_MEDIA_UNKNOWN; }
      "video"       { return (YYCURSOR == YYLIMIT) ? SDP_MEDIA_VIDEO : SDP_MEDIA_UNKNOWN; }
      "application" { return (YYCURSOR == YYLIMIT) ? SDP_MEDIA_APPLICATION : SDP_MEDIA_UNKNOWN; }
      *             { return SDP_MEDIA_UNKNOWN; }
    */
}

/**
 * @brief Parse setup role string to enum using re2c
 * @param str Setup role string (actpass, active, passive)
 * @param len Length of string
 * @return sdp_setup_role_t enum value
 */
sdp_setup_role_t sdp_parse_setup_role_re2c(const char *str, size_t len) {
    if (!str || len == 0 || len > 7) {
        return SDP_ROLE_ACTPASS;
    }

    const char *YYCURSOR = str;
    const char *YYLIMIT = str + len;
    const char *YYMARKER = str;

    /*!re2c
      re2c:define:YYCTYPE = "char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;

      "actpass" { return (YYCURSOR == YYLIMIT) ? SDP_ROLE_ACTPASS : SDP_ROLE_ACTPASS; }
      "active"  { return (YYCURSOR == YYLIMIT) ? SDP_ROLE_ACTIVE : SDP_ROLE_ACTPASS; }
      "passive" { return (YYCURSOR == YYLIMIT) ? SDP_ROLE_PASSIVE : SDP_ROLE_ACTPASS; }
      *         { return SDP_ROLE_ACTPASS; }
    */
}

/**
 * @brief Case-insensitive codec name matching using re2c
 * @param name Codec name to match
 * @param len Length of name
 * @return Codec type identifier (for common codecs) or -1 for unknown
 */
int sdp_match_codec_name_re2c(const char *name, size_t len) {
    if (!name || len == 0 || len > 16) {
        return -1;
    }

    const char *YYCURSOR = name;
    const char *YYLIMIT = name + len;
    const char *YYMARKER = name;

    /*!re2c
      re2c:define:YYCTYPE = "unsigned char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;
      re2c:flags:case-insensitive = 1;

      // Audio codecs
      "opus"    { return (YYCURSOR == YYLIMIT) ? 1 : -1; }
      "pcmu"    { return (YYCURSOR == YYLIMIT) ? 2 : -1; }
      "pcma"    { return (YYCURSOR == YYLIMIT) ? 3 : -1; }
      "g722"    { return (YYCURSOR == YYLIMIT) ? 4 : -1; }
      "isac"    { return (YYCURSOR == YYLIMIT) ? 5 : -1; }
      "ilbc"    { return (YYCURSOR == YYLIMIT) ? 6 : -1; }
      
      // Video codecs
      "vp8"     { return (YYCURSOR == YYLIMIT) ? 10 : -1; }
      "vp9"     { return (YYCURSOR == YYLIMIT) ? 11 : -1; }
      "h264"    { return (YYCURSOR == YYLIMIT) ? 12 : -1; }
      "h265"    { return (YYCURSOR == YYLIMIT) ? 13 : -1; }
      "av1"     { return (YYCURSOR == YYLIMIT) ? 14 : -1; }
      
      // RTX (retransmission)
      "rtx"     { return (YYCURSOR == YYLIMIT) ? 20 : -1; }
      "red"     { return (YYCURSOR == YYLIMIT) ? 21 : -1; }
      "ulpfec"  { return (YYCURSOR == YYLIMIT) ? 22 : -1; }
      "flexfec" { return (YYCURSOR == YYLIMIT) ? 23 : -1; }
      
      *         { return -1; }
    */
}

/* =============================================================================
 * Main SDP Parser
 * ============================================================================= */

/**
 * @brief Parse SDP string into session structure
 */
int sdp_parse(const char *sdp_str, size_t len, sdp_session_t *sdp) {
    if (!sdp_str || !sdp)
        return -1;

    sdp_session_init(sdp);

    const char *YYCURSOR = sdp_str;
    const char *YYMARKER;
    const char *line_start;
    const char *line_end;
    sdp_media_t *current_media = NULL;

parse_loop:
    line_start = YYCURSOR;

    /* Debug: show what we're parsing */
    if (*YYCURSOR == '\0') {
        return 0;  /* Reached end */
    }

    /* Safety check: prevent infinite loop */
    if (YYCURSOR >= sdp_str + len) {
        return 0;  /* Past end of input */
    }

    /*!re2c
        re2c:define:YYCTYPE = "char";
        re2c:yyfill:enable = 0;

        EOL = "\r"? "\n";
        END = "\x00";
        CHAR = [^\r\n\x00];
        LINE = CHAR+;

        // Version line: v=0
        "v=" [0-9]+ EOL {
            // Version is always 0 for SDP
            goto parse_loop;
        }

        // Origin line: o=- 123456 2 IN IP4 127.0.0.1
        "o=" LINE EOL {
            line_end = YYCURSOR - 1;
            while (line_end > line_start && (*line_end == '\r' || *line_end == '\n'))
                line_end--;
            if (parse_origin_line(sdp, line_start + 2) < 0)
                return -1;
            goto parse_loop;
        }

        // Session name: s=-
        "s=" LINE EOL {
            goto parse_loop;
        }

        // Timing: t=0 0
        "t=" LINE EOL {
            goto parse_loop;
        }

        // Connection: c=IN IP4 0.0.0.0
        "c=" LINE EOL {
            goto parse_loop;
        }

        // Bundle group: a=group:BUNDLE 0 1
        "a=group:BUNDLE" [^\r\n]* EOL {
            const char *p = line_start + 14;
            int idx = 0;
            while (*p && *p != '\r' && *p != '\n' && idx < SDP_MAX_MEDIA_SECTIONS) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p && *p != '\r' && *p != '\n') {
                    const char *mid_start = p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
                        p++;
                    size_t mid_len = p - mid_start;
                    if (mid_len < sizeof(sdp->bundle_mids[idx])) {
                        memcpy(sdp->bundle_mids[idx], mid_start, mid_len);
                        sdp->bundle_mids[idx][mid_len] = '\0';
                        idx++;
                    }
                }
            }
            sdp->bundle_count = idx;
            goto parse_loop;
        }

        // Media line: m=audio 9 UDP/TLS/RTP/SAVPF 111
        "m=" LINE EOL {
            if (parse_media_line(sdp, line_start + 2, &current_media) < 0)
                return -1;
            goto parse_loop;
        }

        // Attribute line: a=...
        "a=" LINE EOL {
            if (current_media && parse_attribute_line(current_media, line_start + 2) < 0)
                return -1;
            goto parse_loop;
        }

        // Empty line
        EOL {
            goto parse_loop;
        }

        // End of string
        END {
            return 0;
        }

        // Unknown line - skip it
        LINE EOL {
            goto parse_loop;
        }

        * {
            return -1;
        }
    */
}

/* =============================================================================
 * Session-Level Parsers
 * ============================================================================= */

static int parse_origin_line(sdp_session_t *sdp, const char *line) {
    char username[64];
    char session_id[32];
    uint64_t version;
    char addr[64];

    if (sscanf(line, "%63s %31s %" SCNu64 " IN IP%*d %63s",
               username, session_id, &version, addr) == 4) {
        strncpy(sdp->username, username, sizeof(sdp->username) - 1);
        strncpy(sdp->session_id, session_id, sizeof(sdp->session_id) - 1);
        sdp->session_version = version;
        strncpy(sdp->origin_addr, addr, sizeof(sdp->origin_addr) - 1);
        return 0;
    }
    return -1;
}

/* =============================================================================
 * Media-Level Parsers
 * ============================================================================= */

static int parse_media_line(sdp_session_t *sdp, const char *line, sdp_media_t **out_media) {
    if (sdp->media_count >= SDP_MAX_MEDIA_SECTIONS)
        return -1;

    sdp_media_t *media = &sdp->media[sdp->media_count++];
    memset(media, 0, sizeof(*media));
    *out_media = media;

    const char *YYCURSOR = line;
    const char *YYMARKER;
    const char *type_start = line;
    const char *type_end;

    /*!re2c
        re2c:define:YYCTYPE = "char";
        re2c:yyfill:enable = 0;

        // Match "audio 9 UDP/TLS/RTP/SAVPF 111 112"
        "audio" {
            media->type = SDP_MEDIA_AUDIO;
            goto parse_port;
        }

        "video" {
            media->type = SDP_MEDIA_VIDEO;
            goto parse_port;
        }

        "application" {
            media->type = SDP_MEDIA_APPLICATION;
            goto parse_port;
        }

        * {
            return -1;
        }
    */

parse_port:
    {
        int port;
        char protocol[32];
        if (sscanf(YYCURSOR, " %d %31s", &port, protocol) >= 2) {
            media->port = port;
            strncpy(media->protocol, protocol, sizeof(media->protocol) - 1);

            // Parse payload types
            const char *p = strchr(YYCURSOR, ' ');
            if (p) {
                p = strchr(p + 1, ' ');
                if (p) {
                    p = strchr(p + 1, ' '); // Skip protocol
                    if (p) {
                        p++;
                        while (*p) {
                            if (*p >= '0' && *p <= '9') {
                                int pt = atoi(p);
                                if (media->codec_count < SDP_MAX_CODECS) {
                                    media->codecs[media->codec_count].payload_type = pt;
                                    media->codec_count++;
                                }
                                while (*p && *p >= '0' && *p <= '9') p++;
                            } else {
                                while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;
                            }
                            while (*p == ' ') p++;
                            if (*p == '\r' || *p == '\n' || *p == '\0') break;
                        }
                    }
                }
            }
    }

    return 0;
        }
    }

 
/* =============================================================================
 * Attribute Parsers
 * ============================================================================= */

static int parse_attribute_line(sdp_media_t *media, const char *line) {
    const char *YYCURSOR = line;
    const char *YYMARKER;

    /*!re2c
        re2c:define:YYCTYPE = "char";
        re2c:yyfill:enable = 0;

        // rtpmap:111 opus/48000/2
        "rtpmap:" {
            return parse_rtpmap(media, YYCURSOR);
        }

        // fmtp:111 minptime=10;useinbandfec=1
        "fmtp:" {
            return parse_fmtp(media, YYCURSOR);
        }

        // candidate:1 1 udp 2130706431 192.168.1.1 54321 typ host
        "candidate:" {
            return parse_candidate(media, YYCURSOR);
        }

        // ice-ufrag:abcd
        "ice-ufrag:" {
            const char *p = YYCURSOR;
            while (*p && *p != '\r' && *p != '\n') p++;
            size_t len = p - YYCURSOR;
            if (len < sizeof(media->ice_ufrag)) {
                memcpy(media->ice_ufrag, YYCURSOR, len);
                media->ice_ufrag[len] = '\0';
            }
            return 0;
        }

        // ice-pwd:1234567890abcdef
        "ice-pwd:" {
            const char *p = YYCURSOR;
            while (*p && *p != '\r' && *p != '\n') p++;
            size_t len = p - YYCURSOR;
            if (len < sizeof(media->ice_pwd)) {
                memcpy(media->ice_pwd, YYCURSOR, len);
                media->ice_pwd[len] = '\0';
            }
            return 0;
        }

        // fingerprint:sha-256 AA:BB:CC:...
        "fingerprint:" {
            return parse_fingerprint(media, YYCURSOR);
        }

        // setup:actpass|active|passive
        "setup:actpass" {
            media->setup = SDP_ROLE_ACTPASS;
            return 0;
        }

        "setup:active" {
            media->setup = SDP_ROLE_ACTIVE;
            return 0;
        }

        "setup:passive" {
            media->setup = SDP_ROLE_PASSIVE;
            return 0;
        }

        // mid:0
        "mid:" {
            const char *p = YYCURSOR;
            while (*p && *p != '\r' && *p != '\n') p++;
            size_t len = p - YYCURSOR;
            if (len < sizeof(media->mid)) {
                memcpy(media->mid, YYCURSOR, len);
                media->mid[len] = '\0';
            }
            return 0;
        }

        // rtcp-mux
        "rtcp-mux" {
            media->rtcp_mux = 1;
            return 0;
        }

        // extmap:1 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
        "extmap:" {
            return parse_extmap(media, YYCURSOR);
        }

        // sendrecv|sendonly|recvonly|inactive
        "sendrecv" {
            media->direction = SDP_DIRECTION_SENDRECV;
            return 0;
        }

        "sendonly" {
            media->direction = SDP_DIRECTION_SENDONLY;
            return 0;
        }

        "recvonly" {
            media->direction = SDP_DIRECTION_RECVONLY;
            return 0;
        }

        "inactive" {
            media->direction = SDP_DIRECTION_INACTIVE;
            return 0;
        }

        // sctp-port:5000
        "sctp-port:" {
            media->sctp_port = atoi(YYCURSOR);
            return 0;
        }

        // max-message-size:262144
        "max-message-size:" {
            media->max_message_size = atoi(YYCURSOR);
            return 0;
        }

        // rtcp-fb (feedback)
        "rtcp-fb:" {
            int pt = atoi(YYCURSOR);
            const char *p = YYCURSOR;
            while (*p && *p != ' ') p++;
            if (*p) p++;

            for (int i = 0; i < media->codec_count; i++) {
                if (media->codecs[i].payload_type == pt) {
                    if (strstr(p, "nack")) media->codecs[i].supports_nack = 1;
                    if (strstr(p, "pli")) media->codecs[i].supports_pli = 1;
                    if (strstr(p, "fir")) media->codecs[i].supports_fir = 1;
                    if (strstr(p, "goog-remb")) media->codecs[i].supports_remb = 1;
                    if (strstr(p, "transport-cc")) media->codecs[i].supports_transport_cc = 1;
                    break;
                }
            }
            return 0;
        }

        // ssrc:12345 cname:...
        "ssrc:" {
            uint32_t ssrc = strtoul(YYCURSOR, NULL, 10);
            if (media->ssrc_count < SDP_MAX_SSRCS) {
                media->ssrcs[media->ssrc_count].ssrc = ssrc;
                const char *p = strchr(YYCURSOR, ' ');
                if (p && strncmp(p + 1, "cname:", 6) == 0) {
                    p += 7;
                    const char *end = p;
                    while (*end && *end != '\r' && *end != '\n') end++;
                    size_t len = end - p;
                    if (len < sizeof(media->ssrcs[media->ssrc_count].cname)) {
                        memcpy(media->ssrcs[media->ssrc_count].cname, p, len);
                        media->ssrcs[media->ssrc_count].cname[len] = '\0';
                    }
                }
                media->ssrc_count++;
            }
            return 0;
        }

        // Unknown attribute - skip
        * {
            return 0;
        }
    */
}

/* =============================================================================
 * Complex Value Parsers
 * ============================================================================= */

static int parse_rtpmap(sdp_media_t *media, const char *value) {
    int pt;
    char name[32];
    int clock_rate;
    int channels = 1;

    if (sscanf(value, "%d %31[^/]/%d/%d", &pt, name, &clock_rate, &channels) >= 3 ||
        sscanf(value, "%d %31[^/]/%d", &pt, name, &clock_rate) >= 3) {

        for (int i = 0; i < media->codec_count; i++) {
            if (media->codecs[i].payload_type == pt) {
                strncpy(media->codecs[i].name, name, sizeof(media->codecs[i].name) - 1);
                media->codecs[i].clock_rate = clock_rate;
                media->codecs[i].channels = channels;
                return 0;
            }
        }
    }
    return -1;
}

static int parse_fmtp(sdp_media_t *media, const char *value) {
    int pt;
    char fmtp[256];

    if (sscanf(value, "%d %255[^\r\n]", &pt, fmtp) == 2) {
        for (int i = 0; i < media->codec_count; i++) {
            if (media->codecs[i].payload_type == pt) {
                strncpy(media->codecs[i].fmtp, fmtp, sizeof(media->codecs[i].fmtp) - 1);
                return 0;
            }
        }
    }
    return 0;
}

static int parse_candidate(sdp_media_t *media, const char *value) {
    if (media->candidate_count >= SDP_MAX_CANDIDATES)
        return 0;

    sdp_candidate_t *cand = &media->candidates[media->candidate_count];
    memset(cand, 0, sizeof(*cand));

    if (sscanf(value, "%31s %d %7s %u %63s %hu typ %15s",
               cand->foundation, &cand->component, cand->transport,
               &cand->priority, cand->address, &cand->port, cand->type) == 7) {

        // Parse optional related address
        const char *raddr = strstr(value, "raddr ");
        const char *rport = strstr(value, "rport ");
        if (raddr) {
            sscanf(raddr + 6, "%63s", cand->rel_addr);
        }
        if (rport) {
            sscanf(rport + 6, "%hu", &cand->rel_port);
        }

        media->candidate_count++;
        return 0;
    }
    return -1;
}

static int parse_fingerprint(sdp_media_t *media, const char *value) {
    char hash[16];
    char fingerprint[128];

    if (sscanf(value, "%15s %127s", hash, fingerprint) == 2) {
        strncpy(media->fingerprint_hash, hash, sizeof(media->fingerprint_hash) - 1);
        strncpy(media->fingerprint, fingerprint, sizeof(media->fingerprint) - 1);
        return 0;
    }
    return -1;
}

static int parse_extmap(sdp_media_t *media, const char *value) {
    char *end = NULL;
    long id;
    const char *uri_start;
    const char *uri_end;
    size_t uri_len;
    sdp_extension_t *ext;

    if (!media || !value) {
        return -1;
    }
    if (media->extension_count >= SDP_MAX_EXTENSIONS) {
        return 0;
    }

    id = strtol(value, &end, 10);
    if (id <= 0 || id > 14 || !end) {
        return -1;
    }

    if (*end == '/') {
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
            end++;
        }
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end == '\0' || *end == '\r' || *end == '\n') {
        return -1;
    }

    uri_start = end;
    uri_end = uri_start;
    while (*uri_end && *uri_end != ' ' && *uri_end != '\t' &&
           *uri_end != '\r' && *uri_end != '\n') {
        uri_end++;
    }

    uri_len = (size_t)(uri_end - uri_start);
    if (uri_len == 0 || uri_len >= sizeof(media->extensions[0].uri)) {
        return -1;
    }

    ext = &media->extensions[media->extension_count++];
    memset(ext, 0, sizeof(*ext));
    ext->id = (int)id;
    memcpy(ext->uri, uri_start, uri_len);
    ext->uri[uri_len] = '\0';
    return 0;
}

// Restore MSVC warning settings
#ifdef _MSC_VER
#pragma warning(pop)
#endif
