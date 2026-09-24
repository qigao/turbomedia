#include "source_identity.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <string.h>

static int source_key_from_text_span(
    const char *value, size_t length,
    signaling_source_key_t *key) {
  char text[INET6_ADDRSTRLEN];
  const char *start = value;
  const char *end = value ? value + length : NULL;
  uint8_t address[16];
  size_t index;
  int mapped_ipv4 = 1;

  if (!value || !key) {
    return -1;
  }
  while (start < end && (*start == ' ' || *start == '\t')) {
    start++;
  }
  while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
    end--;
  }
  length = (size_t)(end - start);
  if (length == 0U || length >= sizeof(text) ||
      memchr(start, ',', length) != NULL ||
      memchr(start, '%', length) != NULL ||
      memchr(start, '[', length) != NULL ||
      memchr(start, ']', length) != NULL) {
    return -1;
  }
  memcpy(text, start, length);
  text[length] = '\0';
  memset(key, 0, sizeof(*key));

#ifdef _WIN32
  if (InetPtonA(AF_INET, text, address) == 1) {
#else
  if (inet_pton(AF_INET, text, address) == 1) {
#endif
    key->family = SIGNALING_SOURCE_FAMILY_IPV4;
    memcpy(key->address, address, 4U);
    return 0;
  }
#ifdef _WIN32
  if (InetPtonA(AF_INET6, text, address) != 1) {
#else
  if (inet_pton(AF_INET6, text, address) != 1) {
#endif
    return -1;
  }

  for (index = 0U; index < 10U; ++index) {
    if (address[index] != 0U) {
      mapped_ipv4 = 0;
      break;
    }
  }
  if (mapped_ipv4 &&
      address[10] == 0xffU && address[11] == 0xffU) {
    key->family = SIGNALING_SOURCE_FAMILY_IPV4;
    memcpy(key->address, address + 12U, 4U);
    return 0;
  }

  key->family = SIGNALING_SOURCE_FAMILY_IPV6;
  memcpy(key->address, address, sizeof(key->address));
  return 0;
}

int signaling_source_key_from_peer(
    const cnet_stream_peer *peer,
    signaling_source_key_t *key) {
  const uint8_t *ipv6_bytes;
  size_t index;
  int mapped_ipv4 = 1;

  if (!peer || !key) {
    return -1;
  }
  memset(key, 0, sizeof(*key));
  if (peer->family == CNET_DATAGRAM_ADDRESS_IPV4) {
    key->family = SIGNALING_SOURCE_FAMILY_IPV4;
    memcpy(key->address, peer->address, 4U);
    return 0;
  }
  if (peer->family != CNET_DATAGRAM_ADDRESS_IPV6) {
    return -1;
  }

  ipv6_bytes = peer->address;
  for (index = 0U; index < 10U; ++index) {
    if (ipv6_bytes[index] != 0U) {
      mapped_ipv4 = 0;
      break;
    }
  }
  if (mapped_ipv4 &&
      ipv6_bytes[10] == 0xffU && ipv6_bytes[11] == 0xffU) {
    key->family = SIGNALING_SOURCE_FAMILY_IPV4;
    memcpy(key->address, ipv6_bytes + 12U, 4U);
    return 0;
  }

  key->family = SIGNALING_SOURCE_FAMILY_IPV6;
  memcpy(key->address, ipv6_bytes, sizeof(key->address));
  key->scope_id = peer->scope_id;
  return 0;
}

int signaling_source_key_from_text(
    const char *value, signaling_source_key_t *key) {
  return value
             ? source_key_from_text_span(value, strlen(value), key)
             : -1;
}

int signaling_source_key_equal(
    const signaling_source_key_t *left,
    const signaling_source_key_t *right) {
  size_t bytes;

  if (!left || !right || left->family != right->family) {
    return 0;
  }
  bytes = left->family == SIGNALING_SOURCE_FAMILY_IPV4 ? 4U : 16U;
  return memcmp(left->address, right->address, bytes) == 0 &&
         (left->family == SIGNALING_SOURCE_FAMILY_IPV4 ||
          left->scope_id == right->scope_id);
}

int signaling_trusted_proxy_set_parse(
    const char *addresses,
    signaling_trusted_proxy_set_t *set) {
  const char *cursor;

  if (!set) {
    return -1;
  }
  memset(set, 0, sizeof(*set));
  if (!addresses || addresses[0] == '\0') {
    return 0;
  }

  cursor = addresses;
  while (cursor && *cursor != '\0') {
    const char *comma = strchr(cursor, ',');
    const char *end = comma ? comma : cursor + strlen(cursor);
    signaling_source_key_t parsed;

    if (set->count >= SIGNALING_MAX_TRUSTED_PROXIES ||
        source_key_from_text_span(
            cursor, (size_t)(end - cursor), &parsed) != 0) {
      memset(set, 0, sizeof(*set));
      return -1;
    }
    for (size_t index = 0U; index < set->count; ++index) {
      if (signaling_source_key_equal(&set->keys[index], &parsed)) {
        memset(set, 0, sizeof(*set));
        return -1;
      }
    }
    set->keys[set->count++] = parsed;
    cursor = comma ? comma + 1 : NULL;
    if (comma && (!cursor || *cursor == '\0')) {
      memset(set, 0, sizeof(*set));
      return -1;
    }
  }
  return 0;
}

int signaling_source_identity_is_trusted_proxy(
    const signaling_trusted_proxy_set_t *set,
    const signaling_source_key_t *socket_key) {
  if (!set || !socket_key) {
    return 0;
  }
  for (size_t index = 0U; index < set->count; ++index) {
    if (signaling_source_key_equal(
            &set->keys[index], socket_key)) {
      return 1;
    }
  }
  return 0;
}

int signaling_source_identity_resolve(
    const signaling_trusted_proxy_set_t *set,
    const cnet_stream_peer *peer,
    const char *forwarded_for,
    signaling_source_key_t *key) {
  signaling_source_key_t socket_key;

  if (!set || !key ||
      signaling_source_key_from_peer(peer, &socket_key) != 0) {
    return -1;
  }
  if (!signaling_source_identity_is_trusted_proxy(
          set, &socket_key)) {
    *key = socket_key;
    return 0;
  }
  return signaling_source_key_from_text(forwarded_for, key);
}
