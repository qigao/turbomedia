#include "signaling_source_identity.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

static int signaling_digest_hex_valid(const char *value) {
    size_t index;

    if (!value ||
        strlen(value) != SIGNALING_PROXY_CERT_SHA256_HEX_BYTES) {
        return 0;
    }
    for (index = 0U;
         index < SIGNALING_PROXY_CERT_SHA256_HEX_BYTES; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int signaling_source_key_from_bytes(
    int family, const uint8_t *bytes, uint32_t scope_id,
    signaling_source_key_t *key) {
    size_t index;
    int mapped_ipv4 = 1;

    if (!bytes || !key) {
        return -1;
    }
    memset(key, 0, sizeof(*key));
    if (family == SIGNALING_SOURCE_FAMILY_IPV4) {
        key->family = SIGNALING_SOURCE_FAMILY_IPV4;
        memcpy(key->address, bytes, 4U);
        return 0;
    }
    if (family != SIGNALING_SOURCE_FAMILY_IPV6) {
        return -1;
    }

    for (index = 0U; index < 10U; ++index) {
        if (bytes[index] != 0U) {
            mapped_ipv4 = 0;
            break;
        }
    }
    if (mapped_ipv4 && bytes[10] == 0xffU && bytes[11] == 0xffU) {
        key->family = SIGNALING_SOURCE_FAMILY_IPV4;
        memcpy(key->address, bytes + 12U, 4U);
        return 0;
    }

    key->family = SIGNALING_SOURCE_FAMILY_IPV6;
    memcpy(key->address, bytes, 16U);
    key->scope_id = scope_id;
    return 0;
}

int signaling_source_key_from_peer(
    const cnet_stream_peer *peer, signaling_source_key_t *key) {
    if (!peer || !key) {
        return -1;
    }
    return signaling_source_key_from_bytes(
        (int)peer->family, peer->address, peer->scope_id, key);
}

int signaling_source_key_from_numeric(
    const char *literal, signaling_source_key_t *key) {
    uint8_t bytes[16];
    const unsigned char *cursor;

    if (!literal || !key || literal[0] == '\0') {
        return -1;
    }
    for (cursor = (const unsigned char *)literal; *cursor; ++cursor) {
        if (isspace(*cursor) || *cursor == ',' || *cursor == '%' ||
            *cursor == '[' || *cursor == ']') {
            return -1;
        }
    }

    memset(bytes, 0, sizeof(bytes));
#ifdef _WIN32
    if (InetPtonA(AF_INET, literal, bytes) == 1) {
        return signaling_source_key_from_bytes(
            SIGNALING_SOURCE_FAMILY_IPV4, bytes, 0U, key);
    }
    memset(bytes, 0, sizeof(bytes));
    if (InetPtonA(AF_INET6, literal, bytes) == 1) {
        return signaling_source_key_from_bytes(
            SIGNALING_SOURCE_FAMILY_IPV6, bytes, 0U, key);
    }
#else
    if (inet_pton(AF_INET, literal, bytes) == 1) {
        return signaling_source_key_from_bytes(
            SIGNALING_SOURCE_FAMILY_IPV4, bytes, 0U, key);
    }
    memset(bytes, 0, sizeof(bytes));
    if (inet_pton(AF_INET6, literal, bytes) == 1) {
        return signaling_source_key_from_bytes(
            SIGNALING_SOURCE_FAMILY_IPV6, bytes, 0U, key);
    }
#endif
    return -1;
}

int signaling_source_key_equal(
    const signaling_source_key_t *left,
    const signaling_source_key_t *right) {
    size_t bytes;

    if (!left || !right || left->family != right->family) {
        return 0;
    }
    bytes = left->family == SIGNALING_SOURCE_FAMILY_IPV4 ? 4U : 16U;
    return left->scope_id == right->scope_id &&
           memcmp(left->address, right->address, bytes) == 0;
}

int signaling_source_identity_policy_init(
    signaling_source_identity_policy_t *policy,
    const char *trusted_proxy_map) {
    const char *cursor;
    size_t total_size;

    if (!policy) {
        return -1;
    }
    memset(policy, 0, sizeof(*policy));
    if (!trusted_proxy_map || trusted_proxy_map[0] == '\0') {
        return 0;
    }

    total_size = strlen(trusted_proxy_map);
    if (total_size == 0U ||
        total_size > SIGNALING_TRUSTED_PROXY_MAP_MAX_BYTES) {
        return -1;
    }

    cursor = trusted_proxy_map;
    while (*cursor != '\0') {
        const char *comma = strchr(cursor, ',');
        const char *end = comma ? comma : cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        char ip_literal[64];
        char digest[SIGNALING_PROXY_CERT_SHA256_HEX_BYTES + 1U];
        size_t ip_size;
        size_t digest_size;
        signaling_source_key_t key;
        size_t index;

        if (!equals || equals == cursor || equals + 1 == end ||
            memchr(equals + 1, '=', (size_t)(end - equals - 1)) ||
            policy->proxy_count >= SIGNALING_TRUSTED_PROXY_MAX) {
            return -1;
        }
        ip_size = (size_t)(equals - cursor);
        digest_size = (size_t)(end - equals - 1);
        if (ip_size == 0U || ip_size >= sizeof(ip_literal) ||
            digest_size != SIGNALING_PROXY_CERT_SHA256_HEX_BYTES) {
            return -1;
        }
        memcpy(ip_literal, cursor, ip_size);
        ip_literal[ip_size] = '\0';
        memcpy(digest, equals + 1, digest_size);
        digest[digest_size] = '\0';

        if (signaling_source_key_from_numeric(ip_literal, &key) != 0 ||
            !signaling_digest_hex_valid(digest)) {
            return -1;
        }
        for (index = 0U; index < policy->proxy_count; ++index) {
            if (signaling_source_key_equal(
                    &policy->proxies[index].socket_key, &key)) {
                return -1;
            }
        }

        policy->proxies[policy->proxy_count].socket_key = key;
        memcpy(policy->proxies[policy->proxy_count].certificate_sha256,
               digest, sizeof(digest));
        policy->proxy_count++;

        if (!comma) {
            break;
        }
        cursor = comma + 1;
        if (*cursor == '\0') {
            return -1;
        }
    }
    return policy->proxy_count > 0U ? 0 : -1;
}

int signaling_source_identity_policy_enabled(
    const signaling_source_identity_policy_t *policy) {
    return policy && policy->proxy_count > 0U;
}

signaling_source_identity_result_t signaling_source_identity_resolve(
    const signaling_source_identity_policy_t *policy,
    const cnet_stream_peer *socket_peer,
    const char *peer_certificate_sha256,
    const char *forwarded_for,
    signaling_source_key_t *source_key,
    int *used_trusted_proxy) {
    signaling_source_key_t socket_key;
    size_t index;

    if (used_trusted_proxy) {
        *used_trusted_proxy = 0;
    }
    if (!policy || !socket_peer || !source_key ||
        signaling_source_key_from_peer(socket_peer, &socket_key) != 0) {
        return SIGNALING_SOURCE_IDENTITY_INVALID_PEER;
    }
    if (!signaling_source_identity_policy_enabled(policy)) {
        *source_key = socket_key;
        return SIGNALING_SOURCE_IDENTITY_OK;
    }

    if (!signaling_digest_hex_valid(peer_certificate_sha256)) {
        return SIGNALING_SOURCE_IDENTITY_UNTRUSTED_PROXY;
    }
    for (index = 0U; index < policy->proxy_count; ++index) {
        if (signaling_source_key_equal(
                &policy->proxies[index].socket_key, &socket_key) &&
            strcmp(policy->proxies[index].certificate_sha256,
                   peer_certificate_sha256) == 0) {
            if (signaling_source_key_from_numeric(
                    forwarded_for, source_key) != 0) {
                return SIGNALING_SOURCE_IDENTITY_INVALID_FORWARDED;
            }
            if (used_trusted_proxy) {
                *used_trusted_proxy = 1;
            }
            return SIGNALING_SOURCE_IDENTITY_OK;
        }
    }

    return SIGNALING_SOURCE_IDENTITY_UNTRUSTED_PROXY;
}
