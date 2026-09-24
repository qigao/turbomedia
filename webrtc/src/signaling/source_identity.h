#ifndef TURBO_MEDIA_SIGNALING_SOURCE_IDENTITY_H
#define TURBO_MEDIA_SIGNALING_SOURCE_IDENTITY_H

#include <chttp/chttp.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  SIGNALING_SOURCE_FAMILY_IPV4 = 4,
  SIGNALING_SOURCE_FAMILY_IPV6 = 6,
  SIGNALING_MAX_TRUSTED_PROXIES = 32
};

typedef struct signaling_source_key_s {
  uint8_t family;
  uint8_t address[16];
  uint32_t scope_id;
} signaling_source_key_t;

typedef struct signaling_trusted_proxy_set_s {
  signaling_source_key_t keys[SIGNALING_MAX_TRUSTED_PROXIES];
  size_t count;
} signaling_trusted_proxy_set_t;

int signaling_source_key_from_peer(
    const cnet_stream_peer *peer, signaling_source_key_t *key);

int signaling_source_key_from_text(
    const char *value, signaling_source_key_t *key);

int signaling_source_key_equal(
    const signaling_source_key_t *left,
    const signaling_source_key_t *right);

int signaling_trusted_proxy_set_parse(
    const char *addresses,
    signaling_trusted_proxy_set_t *set);

int signaling_source_identity_is_trusted_proxy(
    const signaling_trusted_proxy_set_t *set,
    const signaling_source_key_t *socket_key);

int signaling_source_identity_resolve(
    const signaling_trusted_proxy_set_t *set,
    const cnet_stream_peer *peer,
    const char *forwarded_for,
    signaling_source_key_t *key);

#ifdef __cplusplus
}
#endif

#endif
