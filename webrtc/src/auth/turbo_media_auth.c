#include "turbo_media_auth.h"

#include <openssl/base64.h>
#include <turbo_crypto.h>
#include <json_parser.h>

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AUTH_BEARER_PREFIX "Bearer "
#define AUTH_TOKEN_TYPE "turbomedia-auth+jwt"
#define AUTH_ALGORITHM "HS256"
_Static_assert(TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES == TURBO_CRYPTO_SHA256_SIZE,
               "auth token SHA-256 size must match crypto provider");

#define AUTH_MAX_TOKEN_BYTES 4096U
#define AUTH_MAX_HEADER_BYTES 512U
#define AUTH_MAX_PAYLOAD_BYTES 2048U
#define AUTH_MAX_CLAIM_BYTES 255U
#define AUTH_SIGNATURE_BYTES 32U
#define AUTH_SHA256_HEX_BYTES (TURBO_CRYPTO_SHA256_SIZE * 2U)
#define AUTH_MAX_REVOCATION_LIST_BYTES                                      \
    (TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS * AUTH_SHA256_HEX_BYTES +          \
     TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS - 1U)

static int auth_string_present(const char *value) {
    return value && value[0] != '\0';
}

static int auth_identifier_valid(const char *value) {
    size_t length;

    if (!auth_string_present(value)) {
        return 0;
    }
    length = strlen(value);
    if (length > AUTH_MAX_CLAIM_BYTES) {
        return 0;
    }
    for (size_t index = 0; index < length; ++index) {
        unsigned char ch = (unsigned char)value[index];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.' || ch == '~' ||
              ch == ':' || ch == '/')) {
            return 0;
        }
    }
    return 1;
}

static int auth_tenant_identifier_valid(const char *value) {
    return auth_identifier_valid(value) && strchr(value, '/') == NULL;
}

static int auth_tenant_room_consistent(
    const char *tenant_id, const char *room_id) {
    size_t tenant_length;

    if (!tenant_id || !room_id) {
        return 1;
    }
    tenant_length = strlen(tenant_id);
    return strncmp(room_id, tenant_id, tenant_length) == 0 &&
           room_id[tenant_length] == '/' &&
           room_id[tenant_length + 1U] != '\0';
}

static int auth_scope_list_valid(const char *scope) {
    size_t length;
    int at_token_start = 1;

    if (!auth_string_present(scope)) {
        return 0;
    }
    length = strlen(scope);
    if (length > AUTH_MAX_CLAIM_BYTES) {
        return 0;
    }
    for (size_t index = 0; index < length; ++index) {
        unsigned char ch = (unsigned char)scope[index];
        if (ch == ' ') {
            if (at_token_start) {
                return 0;
            }
            at_token_start = 1;
            continue;
        }
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.' || ch == ':' ||
              ch == '/')) {
            return 0;
        }
        at_token_start = 0;
    }
    return !at_token_start;
}

static int auth_key_pair_valid(const char *key_id, const char *secret) {
    return auth_identifier_valid(key_id) &&
           auth_string_present(secret) &&
           strlen(secret) >= TURBO_MEDIA_AUTH_MIN_SECRET_BYTES;
}

static int auth_hex_nibble(char value, uint8_t *output) {
    if (value >= '0' && value <= '9') {
        *output = (uint8_t)(value - '0');
        return 0;
    }
    if (value >= 'a' && value <= 'f') {
        *output = (uint8_t)(value - 'a' + 10);
        return 0;
    }
    return -1;
}

static int auth_revocation_list_valid(const char *list) {
    size_t length;
    size_t entry_count = 1U;
    size_t entry_length = 0U;

    if (!auth_string_present(list)) {
        return 1;
    }
    for (length = 0U;
         length <= AUTH_MAX_REVOCATION_LIST_BYTES && list[length] != '\0';
         ++length) {
    }
    if (length > AUTH_MAX_REVOCATION_LIST_BYTES) {
        return 0;
    }
    for (size_t index = 0; index < length; ++index) {
        uint8_t ignored;
        if (list[index] == ',') {
            if (entry_length != AUTH_SHA256_HEX_BYTES ||
                entry_count >= TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS) {
                return 0;
            }
            entry_count++;
            entry_length = 0U;
        } else if (auth_hex_nibble(list[index], &ignored) != 0 ||
                   entry_length >= AUTH_SHA256_HEX_BYTES) {
            return 0;
        } else {
            entry_length++;
        }
    }
    return entry_length == AUTH_SHA256_HEX_BYTES;
}

static int auth_token_revoked(const char *token, size_t token_length,
                              const char *list) {
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    const char *entry;

    if (!auth_string_present(list)) {
        return 0;
    }
    if (turbo_crypto_sha256(token, token_length, digest) != TURBO_CRYPTO_OK) {
        return 1;
    }
    entry = list;
    while (*entry != '\0') {
        uint8_t expected[TURBO_CRYPTO_SHA256_SIZE];
        for (size_t index = 0; index < TURBO_CRYPTO_SHA256_SIZE; ++index) {
            uint8_t high;
            uint8_t low;
            if (auth_hex_nibble(entry[index * 2U], &high) != 0 ||
                auth_hex_nibble(entry[index * 2U + 1U], &low) != 0) {
                return 1;
            }
            expected[index] = (uint8_t)((high << 4U) | low);
        }
        if (turbo_crypto_verify(digest, expected, sizeof(digest)) ==
            TURBO_CRYPTO_OK) {
            return 1;
        }
        entry += AUTH_SHA256_HEX_BYTES;
        if (*entry == ',') {
            entry++;
        }
    }
    return 0;
}

int turbo_media_auth_config_enabled(const turbo_media_auth_config_t *config) {
    return config && auth_string_present(config->active_key_id) &&
           auth_string_present(config->active_secret);
}

int turbo_media_auth_config_validate(const turbo_media_auth_config_t *config) {
    int previous_id_present;
    int previous_secret_present;

    if (!config || !turbo_media_auth_config_enabled(config) ||
        !auth_identifier_valid(config->issuer) ||
        !auth_key_pair_valid(config->active_key_id, config->active_secret) ||
        config->clock_skew_seconds < 0 ||
        config->clock_skew_seconds > 300 ||
        config->max_ttl_seconds < 1 ||
        config->max_ttl_seconds > 86400 ||
        !auth_revocation_list_valid(config->revoked_token_sha256) ||
        ((config->revocation_check == NULL) !=
         (config->revocation_context == NULL))) {
        return -1;
    }

    previous_id_present = auth_string_present(config->previous_key_id);
    previous_secret_present = auth_string_present(config->previous_secret);
    if (previous_id_present != previous_secret_present) {
        return -1;
    }
    if (previous_id_present &&
        (!auth_key_pair_valid(config->previous_key_id,
                              config->previous_secret) ||
         strcmp(config->active_key_id, config->previous_key_id) == 0)) {
        return -1;
    }
    return 0;
}

static char *auth_base64url_encode(const uint8_t *input, size_t input_length) {
    uint8_t *encoded;
    size_t capacity;
    size_t length;

    if ((!input && input_length != 0U) ||
        !EVP_EncodedLength(&capacity, input_length)) {
        return NULL;
    }
    encoded = (uint8_t *)malloc(capacity);
    if (!encoded) {
        return NULL;
    }
    length = EVP_EncodeBlock(encoded, input, input_length);
    while (length > 0U && encoded[length - 1U] == '=') {
        --length;
    }
    for (size_t index = 0; index < length; ++index) {
        if (encoded[index] == '+') {
            encoded[index] = '-';
        } else if (encoded[index] == '/') {
            encoded[index] = '_';
        }
    }
    encoded[length] = '\0';
    return (char *)encoded;
}

static int auth_base64url_decode(const char *input, size_t input_length,
                                 size_t maximum_output, uint8_t **output,
                                 size_t *output_length) {
    uint8_t *normalized = NULL;
    uint8_t *decoded = NULL;
    size_t normalized_length;
    size_t decoded_capacity;
    size_t decoded_length = 0U;
    size_t padding;
    int result = -1;

    if (!input || input_length == 0U || !output || !output_length ||
        input_length > AUTH_MAX_TOKEN_BYTES || input_length % 4U == 1U) {
        return -1;
    }
    *output = NULL;
    *output_length = 0U;
    padding = (4U - (input_length % 4U)) % 4U;
    if (input_length > SIZE_MAX - padding) {
        return -1;
    }
    normalized_length = input_length + padding;
    normalized = (uint8_t *)malloc(normalized_length);
    if (!normalized) {
        return -1;
    }
    for (size_t index = 0; index < input_length; ++index) {
        unsigned char ch = (unsigned char)input[index];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9')) {
            normalized[index] = ch;
        } else if (ch == '-') {
            normalized[index] = '+';
        } else if (ch == '_') {
            normalized[index] = '/';
        } else {
            goto cleanup;
        }
    }
    for (size_t index = input_length; index < normalized_length; ++index) {
        normalized[index] = '=';
    }
    if (maximum_output > SIZE_MAX - 2U ||
        !EVP_DecodedLength(&decoded_capacity, normalized_length) ||
        decoded_capacity > maximum_output + 2U) {
        goto cleanup;
    }
    decoded = (uint8_t *)malloc(decoded_capacity + 1U);
    if (!decoded ||
        !EVP_DecodeBase64(decoded, &decoded_length, decoded_capacity,
                          normalized, normalized_length) ||
        decoded_length > maximum_output) {
        goto cleanup;
    }
    decoded[decoded_length] = '\0';
    *output = decoded;
    *output_length = decoded_length;
    decoded = NULL;
    result = 0;

cleanup:
    free(decoded);
    free(normalized);
    return result;
}

static int auth_json_key_allowed(const char *key,
                                 const char *const *allowed,
                                 size_t allowed_count) {
    for (size_t index = 0; index < allowed_count; ++index) {
        if (strcmp(key, allowed[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int auth_json_object_strict(const json_value_t *object,
                                   const char *const *allowed,
                                   size_t allowed_count) {
    size_t count;

    if (!object || json_type(object) != JSON_OBJECT) {
        return 0;
    }
    count = json_object_size(object);
    for (size_t index = 0; index < count; ++index) {
        const char *key = json_object_key(object, index);
        if (!key || !auth_json_key_allowed(key, allowed, allowed_count)) {
            return 0;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const char *previous_key =
                json_object_key(object, previous);
            if (previous_key && strcmp(key, previous_key) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

static const char *auth_json_string(const json_value_t *object,
                                    const char *key) {
    json_value_t *value = json_object_get(object, key);
    if (!value || json_type(value) != JSON_STRING) {
        return NULL;
    }
    return json_string(value);
}

static int auth_json_int64(const json_value_t *object, const char *key,
                           int64_t *output) {
    json_value_t *value = json_object_get(object, key);
    double number;
    int64_t integer;

    if (!value || json_type(value) != JSON_NUMBER || !output) {
        return -1;
    }
    number = json_number(value);
    if (number < 0.0 || number > 9007199254740991.0) {
        return -1;
    }
    integer = (int64_t)number;
    if ((double)integer != number) {
        return -1;
    }
    *output = integer;
    return 0;
}

static int auth_scope_contains(const char *scope_list,
                               const char *required_scope) {
    size_t required_length;
    const char *cursor;

    if (!auth_scope_list_valid(scope_list) ||
        !auth_identifier_valid(required_scope)) {
        return 0;
    }
    required_length = strlen(required_scope);
    cursor = scope_list;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, ' ');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length == required_length &&
            memcmp(cursor, required_scope, length) == 0) {
            return 1;
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }
    return 0;
}

static int auth_resource_claim_matches(const char *actual,
                                       const char *expected) {
    if (!expected) {
        return actual == NULL;
    }
    return actual && strcmp(actual, expected) == 0;
}

static const char *auth_select_secret(const turbo_media_auth_config_t *config,
                                      const char *key_id) {
    if (strcmp(key_id, config->active_key_id) == 0) {
        return config->active_secret;
    }
    if (auth_string_present(config->previous_key_id) &&
        strcmp(key_id, config->previous_key_id) == 0) {
        return config->previous_secret;
    }
    return NULL;
}

static int auth_verify_signed_token(
    const char *token,
    const turbo_media_auth_config_t *config,
    const turbo_media_auth_policy_t *policy) {
    static const char *const header_keys[] = {"alg", "typ", "kid"};
    static const char *const payload_keys[] = {
        "iss", "sub", "aud", "scope", "iat", "exp", "tenant_id",
        "room_id", "participant_id"
    };
    const char *first_dot;
    const char *second_dot;
    size_t token_length;
    uint8_t *header_text = NULL;
    uint8_t *payload_text = NULL;
    uint8_t *signature = NULL;
    size_t header_length = 0U;
    size_t payload_length = 0U;
    size_t signature_length = 0U;
    json_value_t *header = NULL;
    json_value_t *payload = NULL;
    const char *algorithm;
    const char *type;
    const char *key_id;
    const char *secret;
    const char *issuer;
    const char *subject;
    const char *audience;
    const char *scope;
    const char *tenant_id;
    const char *room_id;
    const char *participant_id;
    uint8_t expected_signature[AUTH_SIGNATURE_BYTES];
    int64_t issued_at;
    int64_t expires_at;
    int64_t now;
    int valid = 0;

    if (!token || !policy ||
        turbo_media_auth_config_validate(config) != 0 ||
        !auth_identifier_valid(policy->audience) ||
        !auth_identifier_valid(policy->required_scope) ||
        (policy->tenant_id &&
         !auth_tenant_identifier_valid(policy->tenant_id)) ||
        (policy->room_id && !auth_identifier_valid(policy->room_id)) ||
        !auth_tenant_room_consistent(policy->tenant_id, policy->room_id) ||
        (policy->participant_id &&
         !auth_identifier_valid(policy->participant_id))) {
        return 0;
    }
    token_length = strlen(token);
    if (token_length == 0U || token_length > AUTH_MAX_TOKEN_BYTES) {
        return 0;
    }
    first_dot = strchr(token, '.');
    second_dot = first_dot ? strchr(first_dot + 1, '.') : NULL;
    if (!first_dot || !second_dot || strchr(second_dot + 1, '.')) {
        return 0;
    }
    if (auth_base64url_decode(token, (size_t)(first_dot - token),
                              AUTH_MAX_HEADER_BYTES, &header_text,
                              &header_length) != 0) {
        goto cleanup;
    }
    if (auth_base64url_decode(first_dot + 1,
                              (size_t)(second_dot - first_dot - 1),
                              AUTH_MAX_PAYLOAD_BYTES, &payload_text,
                              &payload_length) != 0) {
        goto cleanup;
    }
    if (auth_base64url_decode(second_dot + 1,
                              token_length - (size_t)(second_dot + 1 - token),
                              AUTH_SIGNATURE_BYTES, &signature,
                              &signature_length) != 0 ||
        signature_length != AUTH_SIGNATURE_BYTES) {
        goto cleanup;
    }
    if (((header = json_parse((const char *)(header_text), header_length)) ? 0 : -1) != 0) {
        goto cleanup;
    }
    if (((payload = json_parse((const char *)(payload_text), payload_length)) ? 0 : -1) != 0) {
        goto cleanup;
    }
    if (!auth_json_object_strict(
            header, header_keys,
            sizeof(header_keys) / sizeof(header_keys[0]))) {
        goto cleanup;
    }
    if (!auth_json_object_strict(
            payload, payload_keys,
            sizeof(payload_keys) / sizeof(payload_keys[0]))) {
        goto cleanup;
    }

    algorithm = auth_json_string(header, "alg");
    type = auth_json_string(header, "typ");
    key_id = auth_json_string(header, "kid");
    if (!algorithm || strcmp(algorithm, AUTH_ALGORITHM) != 0 ||
        !type || strcmp(type, AUTH_TOKEN_TYPE) != 0 ||
        !auth_identifier_valid(key_id)) {
        goto cleanup;
    }
    secret = auth_select_secret(config, key_id);
    if (!secret ||
        turbo_crypto_hmac_sha256(
            secret, strlen(secret), token, (size_t)(second_dot - token),
            expected_signature) != TURBO_CRYPTO_OK ||
        turbo_crypto_verify(signature, expected_signature,
                            AUTH_SIGNATURE_BYTES) != TURBO_CRYPTO_OK) {
        goto cleanup;
    }

    issuer = auth_json_string(payload, "iss");
    subject = auth_json_string(payload, "sub");
    audience = auth_json_string(payload, "aud");
    scope = auth_json_string(payload, "scope");
    tenant_id = auth_json_string(payload, "tenant_id");
    room_id = auth_json_string(payload, "room_id");
    participant_id = auth_json_string(payload, "participant_id");
    if (!auth_identifier_valid(issuer) ||
        strcmp(issuer, config->issuer) != 0 ||
        !auth_identifier_valid(subject) ||
        !auth_identifier_valid(audience) ||
        strcmp(audience, policy->audience) != 0 ||
        !auth_scope_contains(scope, policy->required_scope) ||
        (tenant_id && !auth_tenant_identifier_valid(tenant_id)) ||
        (room_id && !auth_identifier_valid(room_id)) ||
        !auth_tenant_room_consistent(tenant_id, room_id) ||
        (participant_id && !auth_identifier_valid(participant_id)) ||
        (participant_id && !room_id) ||
        !auth_resource_claim_matches(tenant_id, policy->tenant_id) ||
        !auth_resource_claim_matches(room_id, policy->room_id) ||
        !auth_resource_claim_matches(participant_id,
                                     policy->participant_id) ||
        auth_json_int64(payload, "iat", &issued_at) != 0 ||
        auth_json_int64(payload, "exp", &expires_at) != 0) {
        goto cleanup;
    }

    now = policy->now > 0 ? policy->now : (int64_t)time(NULL);
    if (expires_at <= issued_at ||
        expires_at - issued_at > config->max_ttl_seconds ||
        issued_at > now + config->clock_skew_seconds ||
        expires_at <= now - config->clock_skew_seconds) {
        goto cleanup;
    }
    if (auth_token_revoked(token, token_length,
                           config->revoked_token_sha256)) {
        goto cleanup;
    }
    if (config->revocation_check) {
        uint8_t digest[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
        turbo_media_auth_revocation_status_t revocation_status;
        if (turbo_crypto_sha256(token, token_length, digest) !=
            TURBO_CRYPTO_OK) {
            goto cleanup;
        }
        revocation_status = config->revocation_check(
            config->revocation_context, digest, sizeof(digest));
        memset(digest, 0, sizeof(digest));
        if (revocation_status != TURBO_MEDIA_AUTH_REVOCATION_CLEAR) {
            goto cleanup;
        }
    }
    valid = 1;

cleanup:
    json_free(payload);
    payload = NULL;
    json_free(header);
    header = NULL;
    free(signature);
    free(payload_text);
    free(header_text);
    return valid;
}

static int auth_static_token_matches(const char *authorization,
                                     const char *static_token) {
    size_t prefix_length = strlen(AUTH_BEARER_PREFIX);
    size_t token_length;
    const char *presented;

    if (!authorization || !auth_string_present(static_token) ||
        strncmp(authorization, AUTH_BEARER_PREFIX, prefix_length) != 0) {
        return 0;
    }
    presented = authorization + prefix_length;
    token_length = strlen(static_token);
    return strlen(presented) == token_length &&
           turbo_crypto_verify(static_token, presented, token_length) ==
               TURBO_CRYPTO_OK;
}

turbo_media_auth_result_t turbo_media_auth_authorize(
    const char *authorization,
    const char *static_token,
    const turbo_media_auth_config_t *config,
    const turbo_media_auth_policy_t *policy) {
    size_t prefix_length = strlen(AUTH_BEARER_PREFIX);

    if ((!config || !config->revocation_check) &&
        auth_static_token_matches(authorization, static_token)) {
        return TURBO_MEDIA_AUTH_STATIC_TOKEN;
    }
    if (!authorization ||
        strncmp(authorization, AUTH_BEARER_PREFIX, prefix_length) != 0 ||
        !turbo_media_auth_config_enabled(config) ||
        !auth_verify_signed_token(authorization + prefix_length, config,
                                  policy)) {
        return TURBO_MEDIA_AUTH_DENIED;
    }
    return TURBO_MEDIA_AUTH_SIGNED_TOKEN;
}

turbo_media_auth_result_t turbo_media_auth_authorize_token(
    const char *token,
    const turbo_media_auth_config_t *config,
    const turbo_media_auth_policy_t *policy) {
    if (!token || !turbo_media_auth_config_enabled(config) ||
        !auth_verify_signed_token(token, config, policy)) {
        return TURBO_MEDIA_AUTH_DENIED;
    }
    return TURBO_MEDIA_AUTH_SIGNED_TOKEN;
}

static int auth_payload_append(
    char *output, size_t capacity, size_t *used,
    const char *format, ...) {
    va_list args;
    int written;
    size_t available;

    if (!used || !format) {
        return -1;
    }
    available = output && capacity > *used ? capacity - *used : 0U;
    va_start(args, format);
    written = vsnprintf(output ? output + *used : NULL, available,
                        format, args);
    va_end(args);
    if (written < 0) {
        return -1;
    }
    if ((size_t)written > AUTH_MAX_PAYLOAD_BYTES - *used) {
        return -1;
    }
    if (output && (size_t)written >= available) {
        return -1;
    }
    *used += (size_t)written;
    return 0;
}

static char *auth_format_payload(const turbo_media_auth_config_t *config,
                                 const turbo_media_auth_claims_t *claims) {
    size_t required = 0U;
    size_t used = 0U;
    char *output;

    if (auth_payload_append(
            NULL, 0U, &required,
            "{\"iss\":\"%s\",\"sub\":\"%s\",\"aud\":\"%s\","
            "\"scope\":\"%s\"",
            config->issuer, claims->subject, claims->audience,
            claims->scope) != 0) {
        return NULL;
    }
    if (claims->tenant_id &&
        auth_payload_append(NULL, 0U, &required,
                            ",\"tenant_id\":\"%s\"",
                            claims->tenant_id) != 0) {
        return NULL;
    }
    if (claims->room_id &&
        auth_payload_append(NULL, 0U, &required,
                            ",\"room_id\":\"%s\"",
                            claims->room_id) != 0) {
        return NULL;
    }
    if (claims->participant_id &&
        auth_payload_append(NULL, 0U, &required,
                            ",\"participant_id\":\"%s\"",
                            claims->participant_id) != 0) {
        return NULL;
    }
    if (auth_payload_append(
            NULL, 0U, &required,
            ",\"iat\":%lld,\"exp\":%lld}",
            (long long)claims->issued_at,
            (long long)claims->expires_at) != 0 ||
        required > AUTH_MAX_PAYLOAD_BYTES) {
        return NULL;
    }

    output = (char *)malloc(required + 1U);
    if (!output) {
        return NULL;
    }
    if (auth_payload_append(
            output, required + 1U, &used,
            "{\"iss\":\"%s\",\"sub\":\"%s\",\"aud\":\"%s\","
            "\"scope\":\"%s\"",
            config->issuer, claims->subject, claims->audience,
            claims->scope) != 0 ||
        (claims->tenant_id &&
         auth_payload_append(output, required + 1U, &used,
                             ",\"tenant_id\":\"%s\"",
                             claims->tenant_id) != 0) ||
        (claims->room_id &&
         auth_payload_append(output, required + 1U, &used,
                             ",\"room_id\":\"%s\"",
                             claims->room_id) != 0) ||
        (claims->participant_id &&
         auth_payload_append(output, required + 1U, &used,
                             ",\"participant_id\":\"%s\"",
                             claims->participant_id) != 0) ||
        auth_payload_append(
            output, required + 1U, &used,
            ",\"iat\":%lld,\"exp\":%lld}",
            (long long)claims->issued_at,
            (long long)claims->expires_at) != 0 ||
        used != required) {
        free(output);
        return NULL;
    }
    return output;
}

char *turbo_media_auth_issue(const turbo_media_auth_config_t *config,
                             const turbo_media_auth_claims_t *claims) {
    char header[AUTH_MAX_HEADER_BYTES];
    char *payload = NULL;
    char *encoded_header = NULL;
    char *encoded_payload = NULL;
    char *encoded_signature = NULL;
    char *signing_input = NULL;
    char *token = NULL;
    uint8_t signature[AUTH_SIGNATURE_BYTES];
    int header_length;
    int signing_length;
    int token_length;

    if (turbo_media_auth_config_validate(config) != 0 || !claims ||
        !auth_identifier_valid(claims->subject) ||
        !auth_identifier_valid(claims->audience) ||
        !auth_scope_list_valid(claims->scope) ||
        (claims->tenant_id &&
         !auth_tenant_identifier_valid(claims->tenant_id)) ||
        (claims->room_id && !auth_identifier_valid(claims->room_id)) ||
        !auth_tenant_room_consistent(
            claims->tenant_id, claims->room_id) ||
        (claims->participant_id &&
         !auth_identifier_valid(claims->participant_id)) ||
        (claims->participant_id && !claims->room_id) ||
        claims->issued_at < 0 ||
        claims->expires_at <= claims->issued_at ||
        claims->expires_at - claims->issued_at > config->max_ttl_seconds) {
        return NULL;
    }

    header_length = snprintf(
        header, sizeof(header),
        "{\"alg\":\"%s\",\"typ\":\"%s\",\"kid\":\"%s\"}",
        AUTH_ALGORITHM, AUTH_TOKEN_TYPE, config->active_key_id);
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return NULL;
    }

    payload = auth_format_payload(config, claims);
    if (!payload) {
        goto cleanup;
    }

    encoded_header = auth_base64url_encode(
        (const uint8_t *)header, (size_t)header_length);
    encoded_payload = auth_base64url_encode(
        (const uint8_t *)payload, strlen(payload));
    if (!encoded_header || !encoded_payload) {
        goto cleanup;
    }
    signing_length = snprintf(NULL, 0, "%s.%s", encoded_header,
                              encoded_payload);
    if (signing_length < 0 ||
        (size_t)signing_length > AUTH_MAX_TOKEN_BYTES) {
        goto cleanup;
    }
    signing_input = (char *)malloc((size_t)signing_length + 1U);
    if (!signing_input ||
        snprintf(signing_input, (size_t)signing_length + 1U, "%s.%s",
                 encoded_header, encoded_payload) != signing_length ||
        turbo_crypto_hmac_sha256(
            config->active_secret, strlen(config->active_secret),
            signing_input, (size_t)signing_length,
            signature) != TURBO_CRYPTO_OK) {
        goto cleanup;
    }
    encoded_signature =
        auth_base64url_encode(signature, sizeof(signature));
    if (!encoded_signature) {
        goto cleanup;
    }
    token_length = snprintf(NULL, 0, "%s.%s", signing_input,
                            encoded_signature);
    if (token_length < 0 || (size_t)token_length > AUTH_MAX_TOKEN_BYTES) {
        goto cleanup;
    }
    token = (char *)malloc((size_t)token_length + 1U);
    if (!token ||
        snprintf(token, (size_t)token_length + 1U, "%s.%s",
                 signing_input, encoded_signature) != token_length) {
        free(token);
        token = NULL;
    }

cleanup:
    free(signing_input);
    free(encoded_signature);
    free(encoded_payload);
    free(encoded_header);
    free(payload);
    return token;
}
