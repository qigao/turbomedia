#ifndef TURBO_MEDIA_SIGNALING_SOURCE_IDENTITY_H
#define TURBO_MEDIA_SIGNALING_SOURCE_IDENTITY_H

#include <cnet/cnet.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SIGNALING_SOURCE_FAMILY_IPV4 = 4,
    SIGNALING_SOURCE_FAMILY_IPV6 = 6,
    SIGNALING_TRUSTED_PROXY_MAX = 32,
    SIGNALING_PROXY_CERT_SHA256_HEX_BYTES = 64,
    SIGNALING_TRUSTED_PROXY_MAP_MAX_BYTES = 8192
};

typedef struct signaling_source_key_s {
    uint8_t family;
    uint8_t address[16];
    uint32_t scope_id;
} signaling_source_key_t;

typedef struct signaling_trusted_proxy_entry_s {
    signaling_source_key_t socket_key;
    char certificate_sha256[SIGNALING_PROXY_CERT_SHA256_HEX_BYTES + 1U];
} signaling_trusted_proxy_entry_t;

typedef struct signaling_source_identity_policy_s {
    signaling_trusted_proxy_entry_t proxies[SIGNALING_TRUSTED_PROXY_MAX];
    size_t proxy_count;
} signaling_source_identity_policy_t;

typedef enum signaling_source_identity_result_e {
    SIGNALING_SOURCE_IDENTITY_OK = 0,
    SIGNALING_SOURCE_IDENTITY_INVALID_PEER = -1,
    SIGNALING_SOURCE_IDENTITY_UNTRUSTED_PROXY = -2,
    SIGNALING_SOURCE_IDENTITY_INVALID_FORWARDED = -3,
    SIGNALING_SOURCE_IDENTITY_INVALID_CONFIG = -4
} signaling_source_identity_result_t;

int signaling_source_key_from_peer(
    const cnet_stream_peer *peer, signaling_source_key_t *key);

int signaling_source_key_from_numeric(
    const char *literal, signaling_source_key_t *key);

int signaling_source_key_equal(
    const signaling_source_key_t *left,
    const signaling_source_key_t *right);

int signaling_source_identity_policy_init(
    signaling_source_identity_policy_t *policy,
    const char *trusted_proxy_map);

int signaling_source_identity_policy_enabled(
    const signaling_source_identity_policy_t *policy);

signaling_source_identity_result_t signaling_source_identity_resolve(
    const signaling_source_identity_policy_t *policy,
    const cnet_stream_peer *socket_peer,
    const char *peer_certificate_sha256,
    const char *forwarded_for,
    signaling_source_key_t *source_key,
    int *used_trusted_proxy);

#ifdef __cplusplus
}
#endif

#endif
