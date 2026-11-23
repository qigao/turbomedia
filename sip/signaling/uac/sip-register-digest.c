#include "sip-register-digest.h"
#include "sip-uac-transaction.h"
#include <core/fmt.h>
#include <crypto/sha256.h>
#include <crypto/symmetric.h>

static int space(char c)
{
    return c == ' ' || c == '\t';
}

static int digest_param(vstr* input, vstr* name, char* value, size_t capacity)
{
    const char* p = input->data;
    const char* end = p + input->len;
    while (p < end && space(*p)) ++p;
    const char* start = p;
    while (p < end && *p != '=' && !space(*p) && *p != ',') ++p;
    *name = vstr_from_buf(start, (size_t)(p - start));
    while (p < end && space(*p)) ++p;
    if (name->len == 0 || p == end || *p++ != '=') return -1;
    while (p < end && space(*p)) ++p;

    int quoted = p < end && *p == '"';
    if (quoted) ++p;
    size_t n = 0;
    while (p < end && (quoted ? *p != '"' : *p != ',' && !space(*p))) {
        if (quoted && *p == '\\' && ++p == end) return -1;
        unsigned char c = (unsigned char)*p++;
        if (c < 32 || c == 127 || n == capacity - 1) return -1;
        value[n++] = (char)c;
    }
    if (quoted && (p == end || *p++ != '"')) return -1;
    value[n] = '\0';
    while (p < end && space(*p)) ++p;
    if (p < end && *p++ != ',') return -1;
    *input = vstr_from_buf(p, (size_t)(end - p));
    return 0;
}

static int digest_auth_qop(const char* value)
{
    const char* start = value;
    for (const char* p = value; ; ++p) {
        if (*p != ',' && *p) continue;
        vstr qop = vstr_trim(vstr_from_buf(start, (size_t)(p - start)), " \t");
        if (sip_sv_compare_cstr_ci(&qop, "auth") == 0) return 1;
        if (!*p) return 0;
        start = p + 1;
    }
}

static int digest_parse(vstr value, struct sip_register_digest* digest)
{
    value = vstr_trim(value, " \t");
    if (value.len < 7 || sip_mem_casecmp(value.data, "Digest", 6) || !space(value.data[6]))
        return -1;

    memset(digest, 0, sizeof(*digest));
    digest->algorithm = SIP_DIGEST_MD5; /* RFC 3261 implicit algorithm for legacy peers. */
    value = vstr_trim(vstr_from_buf(value.data + 7, value.len - 7), " \t");
    enum { REALM, NONCE, OPAQUE, ALGORITHM, QOP, STALE, USERHASH };
    const char* const fields[] = {"realm", "nonce", "opaque", "algorithm", "qop", "stale", "userhash"};
    unsigned seen = 0;

    while (value.len) {
        vstr name;
        char param[sizeof(digest->nonce)];
        if (digest_param(&value, &name, param, sizeof(param))) return -1;
        value = vstr_trim(value, " \t");

        unsigned field = 0;
        while (field < sizeof(fields) / sizeof(fields[0]) &&
               sip_sv_compare_cstr_ci(&name, fields[field]) != 0) ++field;
        if (field == sizeof(fields) / sizeof(fields[0])) continue;
        if (seen & (1u << field)) return -1;
        seen |= 1u << field;

        switch (field) {
        case REALM:
            if (strlen(param) >= sizeof(digest->realm)) return -1;
            strcpy(digest->realm, param);
            break;
        case NONCE:
            strcpy(digest->nonce, param);
            break;
        case OPAQUE:
            if (strlen(param) >= sizeof(digest->opaque)) return -1;
            strcpy(digest->opaque, param);
            break;
        case ALGORITHM:
            if (sip_cstr_casecmp(param, "SHA-256") == 0) digest->algorithm = SIP_DIGEST_SHA256;
            else if (sip_cstr_casecmp(param, "MD5") != 0) return -1;
            break;
        case QOP:
            if (!digest_auth_qop(param)) return -1;
            digest->qop = 1;
            break;
        case STALE:
            digest->stale = sip_cstr_casecmp(param, "true") == 0;
            break;
        case USERHASH:
            if (sip_cstr_casecmp(param, "false") != 0) return -1;
            break;
        }
    }
    return digest->realm[0] && digest->nonce[0] ? 0 : -1;
}

int sip_register_digest_challenge(const struct sip_message_t* msg, const char* name,
                                  struct sip_register_digest* best)
{
    best->algorithm = SIP_DIGEST_NONE;
    for (int i = 0; i < sip_message_get_header_count(msg); ++i) {
        vstr field, value;
        struct sip_register_digest candidate;
        if (sip_message_get_header(msg, i, &field, &value) == 0 &&
            sip_sv_compare_cstr_ci(&field, name) == 0 && digest_parse(value, &candidate) == 0 &&
            candidate.algorithm > best->algorithm) {
            *best = candidate;
        }
    }
    return best->algorithm != SIP_DIGEST_NONE ? 0 : -1;
}

static void digest_hex(char* text, const uint8_t* bytes, size_t count)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        text[i * 2] = digits[bytes[i] >> 4];
        text[i * 2 + 1] = digits[bytes[i] & 15];
    }
    text[count * 2] = '\0';
}

enum { DIGEST_HEX_BYTES = SALTS_CRYPTO_SHA256_DIGEST_SIZE * 2 + 1 };

static int digest_hash(enum sip_digest_algorithm algorithm, const char* input, int length,
                       size_t capacity, char output[DIGEST_HEX_BYTES])
{
    if (length < 0 || (size_t)length >= capacity) return -E2BIG;
    uint8_t hash[SALTS_CRYPTO_SHA256_DIGEST_SIZE];
    int r = algorithm == SIP_DIGEST_SHA256 ? salts_crypto_sha256(input, (size_t)length, hash)
                                          : salts_crypto_md5(input, (size_t)length, hash);
    if (r == SALTS_CRYPTO_OK)
        digest_hex(output, hash, algorithm == SIP_DIGEST_SHA256 ? sizeof(hash) : SALTS_CRYPTO_MD5_DIGEST_SIZE);
    salts_crypto_clear(hash, sizeof(hash));
    return r == SALTS_CRYPTO_OK ? 0 : -EIO;
}

/* Escaping is applied only to the wire header. Digest calculation uses the
 * unescaped values defined by the authentication scheme. */
static int digest_escape(const char* value, char* out, size_t capacity)
{
    size_t n = 0;
    for (; *value; ++value) {
        unsigned char c = (unsigned char)*value;
        if (c < 32 || c == 127) return -EINVAL;
        int escape = c == '"' || c == '\\';
        if (capacity - n <= (size_t)(1 + escape)) return -E2BIG;
        if (escape) out[n++] = '\\';
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return 0;
}

int sip_digest_write(const struct sip_register_digest* digest, const char* username, vstr method,
                              const char* password, const char* uri, char* header, size_t capacity)
{
    char input[UDP_PACKET_SIZE], a1[DIGEST_HEX_BYTES], a2[DIGEST_HEX_BYTES], response[DIGEST_HEX_BYTES];
    char cnonce[33] = "", nc[9] = "";
    int n = fmt(input, sizeof(input), "{}:{}:{}", username, digest->realm, password);
    int r = digest_hash(digest->algorithm, input, n, sizeof(input), a1);
    salts_crypto_clear(input, sizeof(input));
    if (r) return r;

    n = fmt(input, sizeof(input), "{}:{}", method, uri);
    r = digest_hash(digest->algorithm, input, n, sizeof(input), a2);
    if (r == 0 && digest->qop) {
        uint8_t random[16];
        if (digest->nonce_count == UINT32_MAX) r = -EOVERFLOW;
        else if (salts_crypto_random(random, sizeof(random)) != SALTS_CRYPTO_OK) r = -EIO;
        else {
            digest_hex(cnonce, random, sizeof(random));
            uint32_t count = digest->nonce_count + 1;
            const uint8_t big_endian[] = {(uint8_t)(count >> 24), (uint8_t)(count >> 16),
                                          (uint8_t)(count >> 8), (uint8_t)count};
            digest_hex(nc, big_endian, sizeof(big_endian));
        }
    }
    if (r == 0) {
        n = digest->qop ? fmt(input, sizeof(input), "{}:{}:{}:{}:auth:{}", a1, digest->nonce, nc, cnonce, a2)
                        : fmt(input, sizeof(input), "{}:{}:{}", a1, digest->nonce, a2);
        r = digest_hash(digest->algorithm, input, n, sizeof(input), response);
    }
    salts_crypto_clear(input, sizeof(input));
    salts_crypto_clear(a1, sizeof(a1));
    if (r) return r;

    char user[1024], realm[sizeof(digest->realm) * 2], nonce[sizeof(digest->nonce) * 2];
    char opaque[sizeof(digest->opaque) * 2], target[1024];
    if (digest_escape(username, user, sizeof(user)) ||
        digest_escape(digest->realm, realm, sizeof(realm)) ||
        digest_escape(digest->nonce, nonce, sizeof(nonce)) ||
        digest_escape(digest->opaque, opaque, sizeof(opaque)) ||
        digest_escape(uri, target, sizeof(target))) return -EINVAL;

    n = fmt(header, capacity,
            "Digest username=\"{}\", realm=\"{}\", nonce=\"{}\", uri=\"{}\", response=\"{}\", algorithm={}",
            user, realm, nonce, target, response, digest->algorithm == SIP_DIGEST_SHA256 ? "SHA-256" : "MD5");
    if (n < 0 || (size_t)n >= capacity) return -E2BIG;
    size_t used = (size_t)n;
    if (digest->qop) {
        n = fmt(header + used, capacity - used, ", qop=auth, nc={}, cnonce=\"{}\"", nc, cnonce);
        if (n < 0 || (size_t)n >= capacity - used) return -E2BIG;
        used += (size_t)n;
    }
    if (digest->opaque[0]) {
        n = fmt(header + used, capacity - used, ", opaque=\"{}\"", opaque);
        if (n < 0 || (size_t)n >= capacity - used) return -E2BIG;
    }
    return 0;
}
