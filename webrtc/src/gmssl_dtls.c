#include "gmssl_dtls.h"

#include <cmeta_crypto.h>
#include <cstl/deque.h>
#include <gmssl/aes.h>
#include <gmssl/digest.h>
#include <gmssl/mem.h>
#include <gmssl/rand.h>
#include <gmssl/tls.h>
#include <gmssl/x509.h>
#include <gmssl/x509_alg.h>
#include <stdlib.h>
#include <string.h>

/* Wire sizes and resource budgets are independent of peer-supplied lengths. */
enum {
    RECORD_HEADER = 13, HANDSHAKE_HEADER = 12, GCM_OVERHEAD = 24,
    MAX_PLAIN = 16384, MAX_DATAGRAM = 65535, MAX_HANDSHAKE = 65536,
    MAX_REASSEMBLY = 8, MAX_FLIGHT = 16, MAX_FUTURE = 8,
    MAX_PACKETS = 256, BYTE_BUDGET = 256 * 1024,
    INITIAL_RTO = 1000, MAX_RTO = 60000,
    HANDSHAKE_LIFETIME = 60000, FINAL_FLIGHT_LIFETIME = 120000,
    DTLS12 = 0xfefd, DTLS10 = 0xfeff,
    CIPHER_ECDSA128 = 0xc02b, CIPHER_ECDSA256 = 0xc02c,
    CIPHER_RSA128 = 0xc02f, CIPHER_RSA256 = 0xc030,
    SIG_ECDSA = 0x0403, SIG_RSA = 0x0401,
    EXT_GROUPS = 10, EXT_POINTS = 11, EXT_SIGNATURES = 13, EXT_SRTP = 14,
    EXT_EMS = 23, EXT_RENEGOTIATION = 0xff01,
    REC_CCS = 20, REC_ALERT = 21, REC_HANDSHAKE = 22, REC_APPLICATION = 23,
    HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_HELLO_VERIFY = 3,
    HS_CERTIFICATE = 11, HS_SERVER_KEY = 12, HS_CERT_REQUEST = 13,
    HS_SERVER_DONE = 14, HS_CERT_VERIFY = 15, HS_CLIENT_KEY = 16, HS_FINISHED = 20
};

typedef enum {
    STATE_NEW, SERVER_HELLO, SERVER_CERT, SERVER_KEY, SERVER_REQUEST_OR_DONE,
    SERVER_DONE, CLIENT_HELLO, CLIENT_CERT, CLIENT_KEY, CLIENT_VERIFY,
    WAIT_CCS, WAIT_FINISHED, READY, CLOSED, FAILED
} handshake_state;

typedef struct { deque_t values; size_t bytes; int initialized; } packet_queue;
typedef struct { uint8_t type; uint16_t epoch; tstr payload; } flight_item;
typedef struct {
    uint16_t sequence;
    uint8_t type;
    tstr body;
    tstr coverage;
    size_t received;
} assembly;

struct turbo_gdtls_context {
    X509_KEY key;
    tstr certificates; /* TLS Certificate body, including the uint24 list size. */
    uint8_t fingerprint[32];
    uint16_t signature;
};

struct turbo_gdtls {
    const turbo_gdtls_context *context;
    handshake_state state;
    int server, started, error, keys_ready, pending_ccs, read_epoch;
    int requested_certificate, ems;
    size_t mtu;
    uint16_t profiles[4], selected_profile, cipher;
    size_t profile_count;
    uint8_t expected_fingerprint[32], client_random[32], server_random[32];
    uint8_t cookie[255];
    size_t cookie_size;
    uint8_t cookie_hello_hash[32];
    int cookie_issued;
    X509_KEY peer_key;
    SECP256R1_KEY ephemeral;
    uint8_t peer_ephemeral[65];
    uint8_t master[48], write_iv[4], read_iv[4];
    AES_KEY write_key, read_key;
    tstr transcript;
    packet_queue output, plaintext, future;
    flight_item flight[MAX_FLIGHT];
    size_t flight_count, flight_bytes;
    assembly fragments[MAX_REASSEMBLY];
    size_t fragment_bytes;
    uint16_t send_handshake, receive_handshake;
    uint64_t send_record[2], replay_top, replay_bits;
    int replay_initialized;
    uint8_t peer_finished[12];
    uint16_t peer_finished_sequence;
    uint64_t now, started_at, deadline, last_flight_sent, final_until;
    uint32_t rto;
};

typedef struct { const uint8_t *data; size_t size; } reader;
typedef struct { uint8_t *data; size_t size, capacity; } writer;

static uint16_t load16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static size_t load24(const uint8_t *p) { return ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2]; }
static void store16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void store24(uint8_t *p, size_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static uint64_t load48(const uint8_t *p) {
    uint64_t v = 0;
    for (size_t i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}
static void store48(uint8_t *p, uint64_t v) {
    for (size_t i = 6; i; --i) { p[i - 1] = (uint8_t)v; v >>= 8; }
}
static const uint8_t *read_bytes(reader *r, size_t n) {
    if (n > r->size) return NULL;
    const uint8_t *p = r->data;
    r->data += n; r->size -= n;
    return p;
}
static int read_vector(reader *r, unsigned width, reader *out) {
    const uint8_t *p = read_bytes(r, width);
    if (!p) return 0;
    size_t n = width == 1 ? p[0] : width == 2 ? load16(p) : load24(p);
    out->data = read_bytes(r, n); out->size = n;
    return out->data != NULL;
}
static int write_bytes(writer *w, const void *p, size_t n) {
    if (n > w->capacity - w->size) return 0;
    if (n) memcpy(w->data + w->size, p, n);
    w->size += n;
    return 1;
}
static int write16(writer *w, uint16_t v) {
    uint8_t b[2]; store16(b, v); return write_bytes(w, b, sizeof(b));
}
static int equal_secret(const void *a, const void *b, size_t n) {
    return gmssl_secure_memcmp(a, b, n) == 0;
}
static void clear_string(tstr s) {
    if (s) cmeta_crypto_clear(s, tstr_len(s));
    tstr_free(s);
}

static int queue_init(packet_queue *q) {
    q->initialized = deque_init_bytes(&q->values, sizeof(tstr), _Alignof(tstr), MAX_PACKETS) == STL_OK;
    return q->initialized;
}
static tstr queue_take(packet_queue *q) {
    tstr value = NULL;
    if (q->initialized && deque_pop_front(&q->values, &value) == STL_OK)
        q->bytes -= tstr_len(value);
    return value;
}
static int queue_push(packet_queue *q, const void *data, size_t size, size_t max_packets) {
    if (size > BYTE_BUDGET - q->bytes || deque_size(&q->values) >= max_packets)
        return TURBO_GDTLS_CAPACITY;
    tstr value = tstr_new_len(data, size);
    if (!value) return TURBO_GDTLS_NOMEM;
    if (deque_push_back(&q->values, &value) != STL_OK) {
        clear_string(value); return TURBO_GDTLS_NOMEM;
    }
    q->bytes += size;
    return 0;
}
static void queue_cleanup(packet_queue *q) {
    if (!q->initialized) return;
    tstr value;
    while ((value = queue_take(q)) != NULL) clear_string(value);
    deque_destroy(&q->values);
    q->initialized = 0;
}
static void clear_flight(turbo_gdtls *s) {
    for (size_t i = 0; i < s->flight_count; ++i) clear_string(s->flight[i].payload);
    s->flight_count = s->flight_bytes = 0;
    s->deadline = 0;
}
static void clear_assembly(turbo_gdtls *s, assembly *a) {
    if (!a->body) return;
    s->fragment_bytes -= tstr_len(a->body) + tstr_len(a->coverage);
    clear_string(a->body); clear_string(a->coverage);
    memset(a, 0, sizeof(*a));
}
static int finish_session(turbo_gdtls *s, int error, int keep_output) {
    s->state = error == TURBO_GDTLS_CLOSED ? CLOSED : FAILED;
    s->error = error;
    clear_flight(s);
    for (size_t i = 0; i < MAX_REASSEMBLY; ++i) clear_assembly(s, &s->fragments[i]);
    clear_string(s->transcript); s->transcript = NULL;
    cmeta_crypto_clear(s->master, sizeof(s->master));
    cmeta_crypto_clear(&s->write_key, sizeof(s->write_key));
    cmeta_crypto_clear(&s->read_key, sizeof(s->read_key));
    s->keys_ready = 0;
    x509_key_cleanup(&s->peer_key);
    secp256r1_key_cleanup(&s->ephemeral);
    tstr packet;
    if (!keep_output)
        while ((packet = queue_take(&s->output)) != NULL) clear_string(packet);
    while ((packet = queue_take(&s->plaintext)) != NULL) clear_string(packet);
    while ((packet = queue_take(&s->future)) != NULL) clear_string(packet);
    return error;
}
static int stop_session(turbo_gdtls *s, int error) {
    return finish_session(s, error, 0);
}
static int append_transcript(turbo_gdtls *s, const void *data, size_t size) {
    size_t used = tstr_len(s->transcript);
    if (size > BYTE_BUDGET - used) return TURBO_GDTLS_CAPACITY;
    tstr next = tstr_reserve(s->transcript, size);
    if (!next) return TURBO_GDTLS_NOMEM;
    s->transcript = next;
    if (size) memcpy(next + used, data, size);
    return tstr_set_len_checked(next, used + size) ? 0 : TURBO_GDTLS_CAPACITY;
}
static const DIGEST *prf_digest(const turbo_gdtls *s) {
    return s->cipher == CIPHER_ECDSA256 || s->cipher == CIPHER_RSA256
        ? DIGEST_sha384() : DIGEST_sha256();
}
static int transcript_hash(const turbo_gdtls *s, uint8_t out[64], size_t *size) {
    return digest(prf_digest(s), (const uint8_t *)s->transcript,
                  tstr_len(s->transcript), out, size) == 1 ? 0 : TURBO_GDTLS_AUTH;
}

static int emit_record(turbo_gdtls *s, uint8_t type, uint16_t epoch,
                       const uint8_t *body, size_t size) {
    uint8_t packet[RECORD_HEADER + MAX_PLAIN + GCM_OVERHEAD];
    if (epoch > 1 || size > MAX_PLAIN ||
        size + RECORD_HEADER + (epoch ? GCM_OVERHEAD : 0) > s->mtu ||
        s->send_record[epoch] >= UINT64_C(0x1000000000000)) return TURBO_GDTLS_CAPACITY;
    packet[0] = type; store16(packet + 1, DTLS12); store16(packet + 3, epoch);
    store48(packet + 5, s->send_record[epoch]);
    size_t wire_size = size;
    if (epoch) {
        if (!s->keys_ready) return TURBO_GDTLS_PROTOCOL;
        uint8_t nonce[12], aad[13];
        memcpy(nonce, s->write_iv, 4); memcpy(nonce + 4, packet + 3, 8);
        memcpy(aad, packet + 3, 8); memcpy(aad + 8, packet, 3); store16(aad + 11, (uint16_t)size);
        memcpy(packet + RECORD_HEADER, nonce + 4, 8);
        if (aes_gcm_encrypt(&s->write_key, nonce, sizeof(nonce), aad, sizeof(aad),
                body, size, packet + RECORD_HEADER + 8, 16,
                packet + RECORD_HEADER + 8 + size) != 1) return TURBO_GDTLS_AUTH;
        wire_size += GCM_OVERHEAD;
    } else if (size) memcpy(packet + RECORD_HEADER, body, size);
    store16(packet + 11, (uint16_t)wire_size);
    int result = queue_push(&s->output, packet, RECORD_HEADER + wire_size, MAX_PACKETS);
    if (!result) ++s->send_record[epoch];
    return result;
}
static int emit_flight_item(turbo_gdtls *s, const flight_item *item) {
    const uint8_t *p = (const uint8_t *)item->payload;
    size_t size = tstr_len(item->payload);
    if (item->type != REC_HANDSHAKE) return emit_record(s, item->type, item->epoch, p, size);
    size_t body_size = size - HANDSHAKE_HEADER;
    size_t chunk = s->mtu - RECORD_HEADER - HANDSHAKE_HEADER - (item->epoch ? GCM_OVERHEAD : 0);
    if (chunk > MAX_PLAIN - HANDSHAKE_HEADER) chunk = MAX_PLAIN - HANDSHAKE_HEADER;
    uint8_t fragment[MAX_PLAIN];
    size_t offset = 0;
    do {
        size_t n = body_size - offset;
        if (n > chunk) n = chunk;
        memcpy(fragment, p, HANDSHAKE_HEADER);
        store24(fragment + 6, offset); store24(fragment + 9, n);
        if (n) memcpy(fragment + HANDSHAKE_HEADER, p + HANDSHAKE_HEADER + offset, n);
        int result = emit_record(s, REC_HANDSHAKE, item->epoch, fragment, HANDSHAKE_HEADER + n);
        if (result) return result;
        offset += n;
    } while (offset < body_size);
    return 0;
}
static int retain_flight(turbo_gdtls *s, uint8_t type, uint16_t epoch,
                         const void *data, size_t size) {
    if (s->flight_count == MAX_FLIGHT || size > BYTE_BUDGET - s->flight_bytes)
        return TURBO_GDTLS_CAPACITY;
    tstr payload = tstr_new_len(data, size);
    if (!payload) return TURBO_GDTLS_NOMEM;
    flight_item *item = &s->flight[s->flight_count++];
    item->type = type; item->epoch = epoch; item->payload = payload;
    s->flight_bytes += size;
    return emit_flight_item(s, item);
}
static int send_handshake(turbo_gdtls *s, uint8_t type, const void *body, size_t size,
                          uint16_t epoch, int hash) {
    if (size > MAX_HANDSHAKE || s->send_handshake == UINT16_MAX) return TURBO_GDTLS_CAPACITY;
    tstr message = tstr_new_len(NULL, HANDSHAKE_HEADER + size);
    if (!message) return TURBO_GDTLS_NOMEM;
    uint8_t *p = (uint8_t *)message;
    p[0] = type; store24(p + 1, size); store16(p + 4, s->send_handshake++);
    store24(p + 6, 0); store24(p + 9, size);
    if (size) memcpy(p + HANDSHAKE_HEADER, body, size);
    int result = hash ? append_transcript(s, p, HANDSHAKE_HEADER + size) : 0;
    if (!result) result = retain_flight(s, REC_HANDSHAKE, epoch, p, HANDSHAKE_HEADER + size);
    clear_string(message);
    return result;
}
static void arm_flight(turbo_gdtls *s) {
    s->rto = INITIAL_RTO;
    s->last_flight_sent = s->now;
    s->deadline = s->now + s->rto;
}
static int resend_flight(turbo_gdtls *s) {
    for (size_t i = 0; i < s->flight_count; ++i) {
        int result = emit_flight_item(s, &s->flight[i]);
        if (result) return result;
    }
    s->last_flight_sent = s->now;
    return 0;
}

turbo_gdtls_context *turbo_gdtls_context_create(const void *chain, size_t chain_len,
                                              const void *key, size_t key_len) {
    if (!chain || !chain_len || chain_len > MAX_HANDSHAKE - 32 || !key || !key_len)
        return NULL;
    turbo_gdtls_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    const uint8_t *p = key, *attrs;
    size_t left = key_len, attrs_size;
    const uint8_t *info; size_t info_size; int version, algorithm, parameter;
    if (asn1_sequence_from_der(&info, &info_size, &p, &left) != 1 || left ||
        asn1_int_from_der(&version, &info, &info_size) != 1 ||
        x509_public_key_algor_from_der(&algorithm, &parameter, &info, &info_size) != 1)
        goto error;
    p = key; left = key_len;
    if (algorithm == OID_rsa_encryption) {
        if (rsa_private_key_info_from_der(&ctx->key.rsa_private_key, &attrs, &attrs_size, &p, &left) != 1 || left)
            goto error;
        ctx->key.algor = OID_rsa_encryption; ctx->key.algor_param = OID_undef;
        ctx->key.has_private_key = 1;
        ctx->key.u.rsa_public_key = ctx->key.rsa_private_key.public_key;
    } else if (algorithm != OID_ec_public_key ||
        x509_private_key_info_from_der(&ctx->key, &attrs, &attrs_size, &p, &left) != 1 || left) goto error;
    if (ctx->key.algor == OID_ec_public_key && ctx->key.algor_param == OID_secp256r1)
        ctx->signature = SIG_ECDSA;
    else if (ctx->key.algor == OID_rsa_encryption && ctx->key.has_private_key &&
             ctx->key.u.rsa_public_key.modulus_size >= 256)
        ctx->signature = SIG_RSA;
    else goto error;
    ctx->certificates = tstr_new_len(NULL, chain_len + 27);
    if (!ctx->certificates) goto error;
    uint8_t *out = (uint8_t *)ctx->certificates + 3;
    size_t total = 0, count = 0;
    p = chain; left = chain_len;
    while (left) {
        const uint8_t *cert;
        size_t size;
        if (++count > 8 || x509_cert_from_der(&cert, &size, &p, &left) != 1) goto error;
        if (count == 1) {
            X509_KEY public_key = {0};
            int valid = x509_cert_get_subject_public_key(cert, size, &public_key) == 1 &&
                        x509_public_key_equ(&ctx->key, &public_key) == 1;
            x509_key_cleanup(&public_key);
            if (!valid || cmeta_sha256(cert, size, ctx->fingerprint) != SALTS_OK) goto error;
        }
        store24(out, size); memcpy(out + 3, cert, size);
        out += 3 + size; total += 3 + size;
    }
    store24((uint8_t *)ctx->certificates, total);
    if (!tstr_set_len_checked(ctx->certificates, total + 3)) goto error;
    return ctx;
error:
    turbo_gdtls_context_destroy(ctx);
    return NULL;
}
void turbo_gdtls_context_destroy(turbo_gdtls_context *ctx) {
    if (!ctx) return;
    x509_key_cleanup(&ctx->key);
    clear_string(ctx->certificates);
    cmeta_crypto_clear(ctx, sizeof(*ctx)); free(ctx);
}
int turbo_gdtls_context_fingerprint(const turbo_gdtls_context *ctx, uint8_t out[32]) {
    if (!ctx || !out) return TURBO_GDTLS_INVALID;
    memcpy(out, ctx->fingerprint, 32); return 0;
}
turbo_gdtls *turbo_gdtls_create(const turbo_gdtls_context *ctx, int server,
                              size_t mtu, const uint8_t expected_sha256[32],
                              const uint16_t *profiles, size_t profile_count) {
    if (!ctx || !expected_sha256 || !profiles || !profile_count || profile_count > 4 ||
        mtu < 256 || mtu > MAX_DATAGRAM || (server != 0 && server != 1)) return NULL;
    for (size_t i = 0; i < profile_count; ++i) {
        if (profiles[i] != 1 && profiles[i] != 2 && profiles[i] != 7 && profiles[i] != 8) return NULL;
        for (size_t j = 0; j < i; ++j) if (profiles[i] == profiles[j]) return NULL;
    }
    turbo_gdtls *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->context = ctx; s->server = server; s->mtu = mtu;
    s->profile_count = profile_count;
    memcpy(s->profiles, profiles, profile_count * sizeof(*profiles));
    memcpy(s->expected_fingerprint, expected_sha256, 32);
    s->transcript = tstr_new_len(NULL, 0);
    if (!s->transcript || !queue_init(&s->output) || !queue_init(&s->plaintext) ||
        !queue_init(&s->future)) { turbo_gdtls_destroy(s); return NULL; }
    return s;
}
void turbo_gdtls_destroy(turbo_gdtls *s) {
    if (!s) return;
    clear_flight(s);
    for (size_t i = 0; i < MAX_REASSEMBLY; ++i) clear_assembly(s, &s->fragments[i]);
    queue_cleanup(&s->output); queue_cleanup(&s->plaintext); queue_cleanup(&s->future);
    clear_string(s->transcript);
    x509_key_cleanup(&s->peer_key); secp256r1_key_cleanup(&s->ephemeral);
    cmeta_crypto_clear(s, sizeof(*s)); free(s);
}

static int has16(reader values, uint16_t wanted) {
    if (values.size % 2) return 0;
    for (size_t i = 0; i < values.size; i += 2)
        if (load16(values.data + i) == wanted) return 1;
    return 0;
}
static int extension(writer *w, uint16_t type, const void *data, size_t size) {
    return size <= UINT16_MAX && write16(w, type) && write16(w, (uint16_t)size) &&
           write_bytes(w, data, size);
}
static int hello_extensions(turbo_gdtls *s, writer *w, int server) {
    uint8_t sr[11]; writer profiles = {sr, 0, sizeof(sr)};
    size_t count = server ? 1 : s->profile_count;
    if (!write16(&profiles, (uint16_t)(count * 2))) return 0;
    for (size_t i = 0; i < count; ++i)
        if (!write16(&profiles, server ? s->selected_profile : s->profiles[i])) return 0;
    const uint8_t empty = 0;
    if (!write_bytes(&profiles, &empty, 1) ||
        !extension(w, EXT_SRTP, sr, profiles.size) ||
        !extension(w, EXT_RENEGOTIATION, &empty, 1)) return 0;
    if ((!server || s->ems) && !extension(w, EXT_EMS, NULL, 0)) return 0;
    const uint8_t points[] = {1, 0};
    if (!extension(w, EXT_POINTS, points, sizeof(points))) return 0;
    if (!server) {
        const uint8_t groups[] = {0, 2, 0, 23};
        const uint8_t signatures[] = {0, 4, 4, 3, 4, 1};
        if (!extension(w, EXT_GROUPS, groups, sizeof(groups)) ||
            !extension(w, EXT_SIGNATURES, signatures, sizeof(signatures))) return 0;
    }
    return 1;
}
static int parse_extensions(turbo_gdtls *s, reader input, int server_hello) {
    unsigned seen = 0;
    int curve = 0, signature = 0, ems = 0;
    uint16_t selected = 0;
    while (input.size) {
        const uint8_t *type = read_bytes(&input, 2);
        reader value;
        if (!type || !read_vector(&input, 2, &value)) return TURBO_GDTLS_PROTOCOL;
        unsigned bit = 0;
        switch (load16(type)) {
        case EXT_SRTP: {
            bit = 1;
            reader offered, mki;
            if (!read_vector(&value, 2, &offered) || !offered.size || offered.size % 2 ||
                !read_vector(&value, 1, &mki) || mki.size || value.size ||
                (server_hello && offered.size != 2)) return TURBO_GDTLS_PROTOCOL;
            for (size_t i = 0; i < s->profile_count && !selected; ++i)
                if (has16(offered, s->profiles[i])) selected = s->profiles[i];
            if (!selected) return TURBO_GDTLS_PROTOCOL;
            break;
        }
        case EXT_EMS:
            bit = 2; if (value.size) return TURBO_GDTLS_PROTOCOL; ems = 1; break;
        case EXT_RENEGOTIATION:
            bit = 4;
            if (value.size != 1 || value.data[0]) return TURBO_GDTLS_PROTOCOL;
            break;
        case EXT_GROUPS: {
            bit = 8; reader groups;
            if (server_hello || !read_vector(&value, 2, &groups) || value.size ||
                !groups.size || groups.size % 2) return TURBO_GDTLS_PROTOCOL;
            curve = has16(groups, 23); break;
        }
        case EXT_SIGNATURES: {
            bit = 16; reader signatures;
            if (server_hello || !read_vector(&value, 2, &signatures) || value.size ||
                !signatures.size || signatures.size % 2) return TURBO_GDTLS_PROTOCOL;
            signature = has16(signatures, s->context->signature); break;
        }
        case EXT_POINTS: {
            bit = 32; reader formats;
            if (!read_vector(&value, 1, &formats) || value.size || !formats.size ||
                !memchr(formats.data, 0, formats.size)) return TURBO_GDTLS_PROTOCOL;
            break;
        }
        default:
            if (server_hello) return TURBO_GDTLS_PROTOCOL;
            break; /* Unknown ClientHello extensions are ignorable. */
        }
        if (bit && (seen & bit)) return TURBO_GDTLS_PROTOCOL;
        seen |= bit;
    }
    if (!selected || (!server_hello && (!curve || !signature))) return TURBO_GDTLS_PROTOCOL;
    s->selected_profile = selected; s->ems = ems;
    return 0;
}
static int send_client_hello(turbo_gdtls *s) {
    uint8_t body[512], extensions[96];
    writer w = {body, 0, sizeof(body)}, ext = {extensions, 0, sizeof(extensions)};
    const uint8_t empty = 0, compression[] = {1, 0};
    uint8_t cookie_size = (uint8_t)s->cookie_size;
    if (!hello_extensions(s, &ext, 0) || !write16(&w, DTLS12) ||
        !write_bytes(&w, s->client_random, 32) || !write_bytes(&w, &empty, 1) ||
        !write_bytes(&w, &cookie_size, 1) || !write_bytes(&w, s->cookie, s->cookie_size) ||
        !write16(&w, 8) || !write16(&w, CIPHER_ECDSA128) || !write16(&w, CIPHER_RSA128) ||
        !write16(&w, CIPHER_ECDSA256) || !write16(&w, CIPHER_RSA256) ||
        !write_bytes(&w, compression, sizeof(compression)) ||
        !write16(&w, (uint16_t)ext.size) || !write_bytes(&w, ext.data, ext.size))
        return TURBO_GDTLS_CAPACITY;
    clear_flight(s);
    int result = send_handshake(s, HS_CLIENT_HELLO, body, w.size, 0, 1);
    if (!result) { s->state = SERVER_HELLO; arm_flight(s); }
    return result;
}
static int sign_bytes(const turbo_gdtls_context *ctx, const uint8_t *data, size_t size,
                      uint8_t *signature, size_t *signature_size) {
    X509_SIGN_CTX sign = {0};
    /* The admitted EC/RSA implementations copy key state into sign; unlike
     * stateful hash-based signatures, they do not mutate the identity key. */
    int result = x509_sign_init(&sign, (X509_KEY *)&ctx->key, NULL, 0) == 1 &&
                 x509_sign_update(&sign, data, size) == 1 &&
                 x509_sign_finish(&sign, signature, signature_size) == 1;
    x509_sign_ctx_cleanup(&sign);
    cmeta_crypto_clear(&sign, sizeof(sign));
    return result ? 0 : TURBO_GDTLS_AUTH;
}
static int verify_bytes(const X509_KEY *key, uint16_t scheme, const uint8_t *data,
                        size_t size, const uint8_t *signature, size_t signature_size) {
    if ((scheme == SIG_ECDSA && (key->algor != OID_ec_public_key || key->algor_param != OID_secp256r1)) ||
        (scheme == SIG_RSA && key->algor != OID_rsa_encryption) ||
        (scheme != SIG_ECDSA && scheme != SIG_RSA)) return TURBO_GDTLS_AUTH;
    X509_SIGN_CTX verify = {0};
    int result = x509_verify_init(&verify, key, NULL, 0, signature, signature_size) == 1 &&
                 x509_verify_update(&verify, data, size) == 1 &&
                 x509_verify_finish(&verify) == 1;
    x509_sign_ctx_cleanup(&verify);
    cmeta_crypto_clear(&verify, sizeof(verify));
    return result ? 0 : TURBO_GDTLS_AUTH;
}
static int ephemeral_public(turbo_gdtls *s, uint8_t out[65]) {
    uint8_t *p = out; size_t size = 0;
    return secp256r1_public_key_to_bytes(&s->ephemeral, &p, &size) == 1 && size == 65
        ? 0 : TURBO_GDTLS_AUTH;
}
static int send_server_flight(turbo_gdtls *s) {
    uint8_t hello[256], extensions[96];
    writer w = {hello, 0, sizeof(hello)}, ext = {extensions, 0, sizeof(extensions)};
    const uint8_t empty = 0;
    if (rand_bytes(s->server_random, 32) != 1 || secp256r1_key_generate(&s->ephemeral) != 1)
        return TURBO_GDTLS_AUTH;
    if (!hello_extensions(s, &ext, 1) || !write16(&w, DTLS12) ||
        !write_bytes(&w, s->server_random, 32) || !write_bytes(&w, &empty, 1) ||
        !write16(&w, s->cipher) || !write_bytes(&w, &empty, 1) ||
        !write16(&w, (uint16_t)ext.size) || !write_bytes(&w, ext.data, ext.size))
        return TURBO_GDTLS_CAPACITY;
    clear_flight(s);
    int result = send_handshake(s, HS_SERVER_HELLO, hello, w.size, 0, 1);
    if (result) return result;
    result = send_handshake(s, HS_CERTIFICATE, s->context->certificates,
                            tstr_len(s->context->certificates), 0, 1);
    if (result) return result;
    uint8_t signing[64 + 69], key_exchange[69 + 4 + RSA_MAX_MODULUS_SIZE];
    memcpy(signing, s->client_random, 32); memcpy(signing + 32, s->server_random, 32);
    signing[64] = 3; store16(signing + 65, 23); signing[67] = 65;
    if ((result = ephemeral_public(s, signing + 68)) != 0) return result;
    memcpy(key_exchange, signing + 64, 69);
    store16(key_exchange + 69, s->context->signature);
    size_t signature_size = 0;
    result = sign_bytes(s->context, signing, sizeof(signing), key_exchange + 73, &signature_size);
    if (result) return result;
    store16(key_exchange + 71, (uint16_t)signature_size);
    result = send_handshake(s, HS_SERVER_KEY, key_exchange, 73 + signature_size, 0, 1);
    if (result) return result;
    const uint8_t request[] = {2, 1, 64, 0, 4, 4, 3, 4, 1, 0, 0};
    result = send_handshake(s, HS_CERT_REQUEST, request, sizeof(request), 0, 1);
    if (result) return result;
    result = send_handshake(s, HS_SERVER_DONE, NULL, 0, 0, 1);
    if (!result) { s->requested_certificate = 1; s->state = CLIENT_CERT; arm_flight(s); }
    return result;
}
static int derive_keys(turbo_gdtls *s) {
    uint8_t pre_master[32] = {0}, key_block[72] = {0}, hash[64];
    size_t hash_size = 0;
    int result = TURBO_GDTLS_AUTH;
    if (secp256r1_ecdh(&s->ephemeral, s->peer_ephemeral, pre_master) != 1) goto cleanup;
    if (s->ems) {
        if (transcript_hash(s, hash, &hash_size) ||
            tls_prf(prf_digest(s), pre_master, sizeof(pre_master), "extended master secret",
                     hash, hash_size, NULL, 0, sizeof(s->master), s->master) != 1) goto cleanup;
    } else if (tls_prf(prf_digest(s), pre_master, sizeof(pre_master), "master secret",
                        s->client_random, 32, s->server_random, 32,
                        sizeof(s->master), s->master) != 1) goto cleanup;
    size_t key_size = prf_digest(s) == DIGEST_sha384() ? 32 : 16;
    if (tls_prf(prf_digest(s), s->master, sizeof(s->master), "key expansion",
                s->server_random, 32, s->client_random, 32, 2 * key_size + 8, key_block) != 1)
        goto cleanup;
    const uint8_t *client_key = key_block, *server_key = key_block + key_size;
    if (aes_set_encrypt_key(&s->write_key, s->server ? server_key : client_key, key_size) != 1 ||
        aes_set_encrypt_key(&s->read_key, s->server ? client_key : server_key, key_size) != 1)
        goto cleanup;
    memcpy(s->write_iv, key_block + 2 * key_size + (s->server ? 4 : 0), 4);
    memcpy(s->read_iv, key_block + 2 * key_size + (s->server ? 0 : 4), 4);
    s->keys_ready = 1; result = 0;
cleanup:
    cmeta_crypto_clear(pre_master, sizeof(pre_master));
    cmeta_crypto_clear(key_block, sizeof(key_block));
    cmeta_crypto_clear(hash, sizeof(hash));
    secp256r1_key_cleanup(&s->ephemeral);
    return result;
}
static int finished_data(const turbo_gdtls *s, int server, uint8_t out[12]) {
    uint8_t hash[64]; size_t size = 0;
    int result = transcript_hash(s, hash, &size);
    if (!result && tls_prf(prf_digest(s), s->master, sizeof(s->master),
            server ? "server finished" : "client finished", hash, size, NULL, 0, 12, out) != 1)
        result = TURBO_GDTLS_AUTH;
    cmeta_crypto_clear(hash, sizeof(hash));
    return result;
}
static int send_finished(turbo_gdtls *s) {
    const uint8_t ccs = 1;
    int result = retain_flight(s, REC_CCS, 0, &ccs, 1);
    uint8_t verify[12];
    if (!result) result = finished_data(s, s->server, verify);
    if (!result) result = send_handshake(s, HS_FINISHED, verify, sizeof(verify), 1, 1);
    cmeta_crypto_clear(verify, sizeof(verify));
    return result;
}
static int send_client_flight(turbo_gdtls *s) {
    clear_flight(s);
    int result;
    if (s->requested_certificate &&
        (result = send_handshake(s, HS_CERTIFICATE, s->context->certificates,
                                 tstr_len(s->context->certificates), 0, 1)) != 0) return result;
    if (secp256r1_key_generate(&s->ephemeral) != 1) return TURBO_GDTLS_AUTH;
    uint8_t exchange[66]; exchange[0] = 65;
    if ((result = ephemeral_public(s, exchange + 1)) != 0) return result;
    if ((result = send_handshake(s, HS_CLIENT_KEY, exchange, sizeof(exchange), 0, 1)) != 0 ||
        (result = derive_keys(s)) != 0) return result;
    if (s->requested_certificate) {
        uint8_t verify[4 + RSA_MAX_MODULUS_SIZE]; size_t size = 0;
        store16(verify, s->context->signature);
        result = sign_bytes(s->context, (const uint8_t *)s->transcript,
                            tstr_len(s->transcript), verify + 4, &size);
        if (result) return result;
        store16(verify + 2, (uint16_t)size);
        if ((result = send_handshake(s, HS_CERT_VERIFY, verify, 4 + size, 0, 1)) != 0)
            return result;
    }
    result = send_finished(s);
    if (!result) { s->state = WAIT_CCS; arm_flight(s); }
    return result;
}

static int read_peer_certificate(turbo_gdtls *s, reader body) {
    reader list;
    if (!read_vector(&body, 3, &list) || body.size || !list.size) return TURBO_GDTLS_PROTOCOL;
    size_t count = 0;
    while (list.size) {
        reader cert;
        if (++count > 8 || !read_vector(&list, 3, &cert) || !cert.size) return TURBO_GDTLS_PROTOCOL;
        const uint8_t *p = cert.data, *parsed;
        size_t left = cert.size, parsed_size;
        if (x509_cert_from_der(&parsed, &parsed_size, &p, &left) != 1 || left)
            return TURBO_GDTLS_PROTOCOL;
        if (count == 1) {
            uint8_t actual[32];
            if (cmeta_sha256(cert.data, cert.size, actual) != SALTS_OK ||
                !equal_secret(actual, s->expected_fingerprint, sizeof(actual))) return TURBO_GDTLS_AUTH;
            if (x509_cert_get_subject_public_key(cert.data, cert.size, &s->peer_key) != 1)
                return TURBO_GDTLS_AUTH;
            if (s->peer_key.algor == OID_ec_public_key) {
                if (s->peer_key.algor_param != OID_secp256r1) return TURBO_GDTLS_AUTH;
            } else if (s->peer_key.algor != OID_rsa_encryption ||
                       s->peer_key.u.rsa_public_key.modulus_size < 256) return TURBO_GDTLS_AUTH;
        }
    }
    return 0;
}
static int receive_client_hello(turbo_gdtls *s, const uint8_t *message, size_t size) {
    reader body = {message + HANDSHAKE_HEADER, size - HANDSHAKE_HEADER};
    const uint8_t *version = read_bytes(&body, 2), *random = read_bytes(&body, 32);
    reader id, cookie, ciphers, compression, extensions;
    if (!version || !random || load16(version) != DTLS12 ||
        !read_vector(&body, 1, &id) || id.size > 32) return TURBO_GDTLS_PROTOCOL;
    const uint8_t *cookie_start = body.data;
    if (!read_vector(&body, 1, &cookie)) return TURBO_GDTLS_PROTOCOL;
    const uint8_t *cookie_end = body.data;
    if (!read_vector(&body, 2, &ciphers) || !ciphers.size || ciphers.size % 2 ||
        !read_vector(&body, 1, &compression) || !compression.size ||
        !memchr(compression.data, 0, compression.size) ||
        !read_vector(&body, 2, &extensions) || body.size) return TURBO_GDTLS_PROTOCOL;

    // The random cookie is scoped to this immutable peer association. Hash the
    // entire hello except its cookie vector; changing parameters cannot reuse it.
    uint8_t hello_hash[32]; size_t hash_size = 0;
    DIGEST_CTX hash;
    if (digest_init(&hash, DIGEST_sha256()) != 1 ||
        digest_update(&hash, message + HANDSHAKE_HEADER,
                       (size_t)(cookie_start - message - HANDSHAKE_HEADER)) != 1 ||
        digest_update(&hash, cookie_end, (size_t)(message + size - cookie_end)) != 1 ||
        digest_finish(&hash, hello_hash, &hash_size) != 1 || hash_size != sizeof(hello_hash))
        return TURBO_GDTLS_AUTH;
    if (!s->cookie_issued) {
        if (rand_bytes(s->cookie, 32) != 1) return TURBO_GDTLS_AUTH;
        s->cookie_size = 32; s->cookie_issued = 1;
        memcpy(s->cookie_hello_hash, hello_hash, sizeof(hello_hash));
        uint8_t verify[35]; store16(verify, DTLS10); verify[2] = 32;
        memcpy(verify + 3, s->cookie, 32);
        clear_flight(s);
        int result = send_handshake(s, HS_HELLO_VERIFY, verify, sizeof(verify), 0, 0);
        if (!result) arm_flight(s);
        return result;
    }
    if (cookie.size != s->cookie_size || !equal_secret(cookie.data, s->cookie, cookie.size) ||
        !equal_secret(hello_hash, s->cookie_hello_hash, sizeof(hello_hash))) return TURBO_GDTLS_AUTH;
    uint16_t first = s->context->signature == SIG_RSA ? CIPHER_RSA128 : CIPHER_ECDSA128;
    uint16_t second = s->context->signature == SIG_RSA ? CIPHER_RSA256 : CIPHER_ECDSA256;
    s->cipher = has16(ciphers, first) ? first : has16(ciphers, second) ? second : 0;
    if (!s->cipher) return TURBO_GDTLS_PROTOCOL;
    int result = parse_extensions(s, extensions, 0);
    if (result) return result;
    memcpy(s->client_random, random, 32);
    result = append_transcript(s, message, size);
    return result ? result : send_server_flight(s);
}
static int receive_server_hello(turbo_gdtls *s, reader body) {
    const uint8_t *version = read_bytes(&body, 2), *random = read_bytes(&body, 32);
    reader id, extensions;
    if (!version || !random || load16(version) != DTLS12 ||
        !read_vector(&body, 1, &id) || id.size > 32) return TURBO_GDTLS_PROTOCOL;
    const uint8_t *cipher = read_bytes(&body, 2), *compression = read_bytes(&body, 1);
    if (!cipher || !compression || *compression ||
        !read_vector(&body, 2, &extensions) || body.size) return TURBO_GDTLS_PROTOCOL;
    uint16_t selected = load16(cipher);
    if (selected != CIPHER_ECDSA128 && selected != CIPHER_ECDSA256 &&
        selected != CIPHER_RSA128 && selected != CIPHER_RSA256) return TURBO_GDTLS_PROTOCOL;
    int result = parse_extensions(s, extensions, 1);
    if (result) return result;
    s->cipher = selected; memcpy(s->server_random, random, 32);
    return 0;
}
static int receive_server_key(turbo_gdtls *s, reader body) {
    const uint8_t *params = read_bytes(&body, 69), *scheme = read_bytes(&body, 2);
    reader signature;
    if (!params || !scheme || params[0] != 3 || load16(params + 1) != 23 || params[3] != 65 ||
        !read_vector(&body, 2, &signature) || !signature.size || body.size)
        return TURBO_GDTLS_PROTOCOL;
    uint16_t required = s->cipher == CIPHER_RSA128 || s->cipher == CIPHER_RSA256 ? SIG_RSA : SIG_ECDSA;
    if (load16(scheme) != required) return TURBO_GDTLS_AUTH;
    uint8_t signing[64 + 69];
    memcpy(signing, s->client_random, 32); memcpy(signing + 32, s->server_random, 32);
    memcpy(signing + 64, params, 69);
    int result = verify_bytes(&s->peer_key, required, signing, sizeof(signing),
                              signature.data, signature.size);
    if (!result) memcpy(s->peer_ephemeral, params + 4, 65);
    return result;
}
static int receive_certificate_request(turbo_gdtls *s, reader body) {
    reader types, signatures, authorities;
    if (!read_vector(&body, 1, &types) || !types.size ||
        !read_vector(&body, 2, &signatures) || !signatures.size || signatures.size % 2 ||
        !read_vector(&body, 2, &authorities) || body.size) return TURBO_GDTLS_PROTOCOL;
    uint8_t type = s->context->signature == SIG_RSA ? 1 : 64;
    if (!memchr(types.data, type, types.size) || !has16(signatures, s->context->signature))
        return TURBO_GDTLS_PROTOCOL;
    // CA names do not replace the already admitted SDP fingerprint identity.
    while (authorities.size) {
        reader name;
        if (!read_vector(&authorities, 2, &name) || !name.size) return TURBO_GDTLS_PROTOCOL;
    }
    s->requested_certificate = 1;
    return 0;
}
static void promote_ccs(turbo_gdtls *s) {
    if (s->state == WAIT_CCS && s->pending_ccs && s->keys_ready) {
        s->read_epoch = 1; s->pending_ccs = 0; s->state = WAIT_FINISHED;
    }
}
static int process_handshake(turbo_gdtls *s, const uint8_t *message, size_t size) {
    uint8_t type = message[0];
    reader body = {message + HANDSHAKE_HEADER, size - HANDSHAKE_HEADER};
    int result = TURBO_GDTLS_PROTOCOL;
    if (s->state == CLIENT_HELLO && type == HS_CLIENT_HELLO)
        return receive_client_hello(s, message, size);
    if (s->state == SERVER_HELLO && type == HS_HELLO_VERIFY) {
        const uint8_t *version = read_bytes(&body, 2); reader cookie;
        if (!version || (load16(version) != DTLS10 && load16(version) != DTLS12) ||
            !read_vector(&body, 1, &cookie) || !cookie.size || body.size)
            return TURBO_GDTLS_PROTOCOL;
        memcpy(s->cookie, cookie.data, cookie.size); s->cookie_size = cookie.size;
        if (!tstr_set_len_checked(s->transcript, 0)) return TURBO_GDTLS_CAPACITY;
        return send_client_hello(s);
    }
    if (s->state == SERVER_HELLO && type == HS_SERVER_HELLO) {
        result = receive_server_hello(s, body);
        if (!result) { result = append_transcript(s, message, size); s->state = SERVER_CERT; }
    } else if ((s->state == SERVER_CERT || s->state == CLIENT_CERT) && type == HS_CERTIFICATE) {
        result = read_peer_certificate(s, body);
        if (!result) {
            result = append_transcript(s, message, size);
            s->state = s->server ? CLIENT_KEY : SERVER_KEY;
        }
    } else if (s->state == SERVER_KEY && type == HS_SERVER_KEY) {
        result = receive_server_key(s, body);
        if (!result) { result = append_transcript(s, message, size); s->state = SERVER_REQUEST_OR_DONE; }
    } else if (s->state == SERVER_REQUEST_OR_DONE && type == HS_CERT_REQUEST) {
        result = receive_certificate_request(s, body);
        if (!result) { result = append_transcript(s, message, size); s->state = SERVER_DONE; }
    } else if ((s->state == SERVER_REQUEST_OR_DONE || s->state == SERVER_DONE) && type == HS_SERVER_DONE) {
        if (body.size) return TURBO_GDTLS_PROTOCOL;
        result = append_transcript(s, message, size);
        if (!result) result = send_client_flight(s);
    } else if (s->state == CLIENT_KEY && type == HS_CLIENT_KEY) {
        reader point;
        if (!read_vector(&body, 1, &point) || point.size != 65 || body.size)
            return TURBO_GDTLS_PROTOCOL;
        memcpy(s->peer_ephemeral, point.data, 65);
        result = append_transcript(s, message, size);
        if (!result) result = derive_keys(s);
        if (!result) s->state = CLIENT_VERIFY;
    } else if (s->state == CLIENT_VERIFY && type == HS_CERT_VERIFY) {
        const uint8_t *scheme = read_bytes(&body, 2); reader signature;
        if (!scheme || !read_vector(&body, 2, &signature) || !signature.size || body.size)
            return TURBO_GDTLS_PROTOCOL;
        result = verify_bytes(&s->peer_key, load16(scheme), (const uint8_t *)s->transcript,
                               tstr_len(s->transcript), signature.data, signature.size);
        if (!result) { result = append_transcript(s, message, size); s->state = WAIT_CCS; }
    } else if (s->state == WAIT_FINISHED && type == HS_FINISHED) {
        uint8_t expected[12];
        if (body.size != sizeof(expected)) return TURBO_GDTLS_PROTOCOL;
        result = finished_data(s, !s->server, expected);
        if (!result && !equal_secret(expected, body.data, sizeof(expected))) result = TURBO_GDTLS_AUTH;
        cmeta_crypto_clear(expected, sizeof(expected));
        if (!result) result = append_transcript(s, message, size);
        if (!result) {
            memcpy(s->peer_finished, body.data, sizeof(s->peer_finished));
            s->peer_finished_sequence = load16(message + 4);
            clear_flight(s);
            if (s->server) result = send_finished(s);
            if (!result) {
                s->state = READY;
                s->final_until = s->now + FINAL_FLIGHT_LIFETIME;
                s->last_flight_sent = s->now;
                s->deadline = s->server ? s->final_until : 0;
                clear_string(s->transcript); s->transcript = NULL;
            }
        }
    }
    if (!result) promote_ccs(s);
    return result;
}

static int valid_handshake_record(const uint8_t *data, size_t size, uint16_t epoch) {
    // Validate the complete record before committing any fragment. DTLS drops
    // structurally invalid records so a later valid retransmission can recover.
    while (size) {
        if (size < HANDSHAKE_HEADER) return 0;
        size_t total = load24(data + 1), offset = load24(data + 6), length = load24(data + 9);
        if (total > MAX_HANDSHAKE || offset > total || length > total - offset ||
            length > size - HANDSHAKE_HEADER || (data[0] == HS_FINISHED ? epoch != 1 : epoch != 0))
            return 0;
        data += HANDSHAKE_HEADER + length; size -= HANDSHAKE_HEADER + length;
    }
    return 1;
}
static int receive_fragments(turbo_gdtls *s, const uint8_t *data, size_t size, uint16_t epoch) {
    if (!valid_handshake_record(data, size, epoch)) return 0;
    while (size) {
        uint8_t type = data[0];
        size_t total = load24(data + 1), offset = load24(data + 6), length = load24(data + 9);
        uint16_t sequence = load16(data + 4);
        if (s->state == READY) {
            // A valid repeated peer Finished asks the server for its last flight.
            // The record has already passed AEAD authentication, even if replayed.
            if (s->server && type == HS_FINISHED && sequence == s->peer_finished_sequence &&
                total == 12 && offset == 0 && length == 12 &&
                equal_secret(data + HANDSHAKE_HEADER, s->peer_finished, 12) &&
                s->now < s->final_until && s->now - s->last_flight_sent >= INITIAL_RTO) {
                int result = resend_flight(s);
                if (result) return result;
            }
        } else if (sequence < s->receive_handshake) {
            if (s->flight_count && s->now - s->last_flight_sent >= INITIAL_RTO) {
                int result = resend_flight(s);
                if (result) return result;
            }
        } else if ((unsigned)(sequence - s->receive_handshake) < MAX_REASSEMBLY) {
            assembly *a = NULL, *unused = NULL;
            for (size_t i = 0; i < MAX_REASSEMBLY; ++i) {
                if (!s->fragments[i].body) unused = &s->fragments[i];
                else if (s->fragments[i].sequence == sequence) a = &s->fragments[i];
            }
            if (!a) {
                size_t coverage_size = (total + 7) / 8;
                if (!unused || total + coverage_size > BYTE_BUDGET - s->fragment_bytes)
                    return TURBO_GDTLS_CAPACITY;
                a = unused;
                a->body = tstr_new_len(NULL, total);
                a->coverage = tstr_new_len(NULL, coverage_size);
                if (!a->body || !a->coverage) {
                    clear_string(a->body); clear_string(a->coverage); memset(a, 0, sizeof(*a));
                    return TURBO_GDTLS_NOMEM;
                }
                if (coverage_size) memset(a->coverage, 0, coverage_size);
                a->sequence = sequence; a->type = type;
                s->fragment_bytes += total + coverage_size;
            }
            if (tstr_len(a->body) != total || a->type != type) return 0;
            // An overlapping retransmission must agree with already accepted
            // bytes. Do not partially mutate the assembly before that check.
            for (size_t i = 0; i < length; ++i) {
                size_t index = offset + i;
                if (((uint8_t)a->coverage[index / 8] & (1u << (index % 8))) &&
                    (uint8_t)a->body[index] != data[HANDSHAKE_HEADER + i]) return 0;
            }
            for (size_t i = 0; i < length; ++i) {
                size_t index = offset + i;
                uint8_t bit = (uint8_t)(1u << (index % 8));
                if (!((uint8_t)a->coverage[index / 8] & bit)) {
                    a->coverage[index / 8] = (char)((uint8_t)a->coverage[index / 8] | bit);
                    a->body[index] = (char)data[HANDSHAKE_HEADER + i]; ++a->received;
                }
            }
            for (;;) {
                assembly *next = NULL;
                for (size_t i = 0; i < MAX_REASSEMBLY; ++i)
                    if (s->fragments[i].body && s->fragments[i].sequence == s->receive_handshake &&
                        s->fragments[i].received == tstr_len(s->fragments[i].body)) next = &s->fragments[i];
                if (!next) break;
                size_t n = tstr_len(next->body);
                tstr canonical = tstr_new_len(NULL, HANDSHAKE_HEADER + n);
                if (!canonical) return TURBO_GDTLS_NOMEM;
                uint8_t *p = (uint8_t *)canonical;
                p[0] = next->type; store24(p + 1, n); store16(p + 4, next->sequence);
                store24(p + 6, 0); store24(p + 9, n);
                if (n) memcpy(p + HANDSHAKE_HEADER, next->body, n);
                int result = process_handshake(s, p, HANDSHAKE_HEADER + n);
                clear_string(canonical); clear_assembly(s, next);
                if (result) return result;
                if (s->receive_handshake == UINT16_MAX) return TURBO_GDTLS_PROTOCOL;
                ++s->receive_handshake;
            }
        } /* A peer can retransmit a message outside the bounded receive window. */
        data += HANDSHAKE_HEADER + length; size -= HANDSHAKE_HEADER + length;
    }
    return 0;
}

static int replayed(const turbo_gdtls *s, uint64_t sequence) {
    if (!s->replay_initialized || sequence > s->replay_top) return 0;
    uint64_t behind = s->replay_top - sequence;
    return behind >= 64 || (s->replay_bits & (UINT64_C(1) << behind)) != 0;
}
static void accept_sequence(turbo_gdtls *s, uint64_t sequence) {
    if (!s->replay_initialized) {
        s->replay_initialized = 1; s->replay_top = sequence; s->replay_bits = 1;
    } else if (sequence > s->replay_top) {
        uint64_t distance = sequence - s->replay_top;
        s->replay_bits = distance >= 64 ? 1 : (s->replay_bits << distance) | 1;
        s->replay_top = sequence;
    } else if (s->replay_top - sequence < 64)
        s->replay_bits |= UINT64_C(1) << (s->replay_top - sequence);
}
static int receive_record(turbo_gdtls *s, const uint8_t *record, size_t size, int may_queue) {
    uint8_t type = record[0]; uint16_t epoch = load16(record + 3);
    const uint8_t *body = record + RECORD_HEADER;
    size_t body_size = size - RECORD_HEADER;
    uint8_t plain[MAX_PLAIN];
    if (epoch > 1 || (epoch && load16(record + 1) != DTLS12) ||
        type < REC_CCS || type > REC_APPLICATION) return 0;
    if (epoch && !s->read_epoch) {
        if (!may_queue || s->state == READY || s->state == CLOSED ||
            deque_size(&s->future.values) >= MAX_FUTURE) return 0;
        // Future-epoch ciphertext is untrusted, bounded, and cannot change keys.
        return queue_push(&s->future, record, size, MAX_FUTURE);
    }
    if (epoch) {
        if (!s->keys_ready || body_size < GCM_OVERHEAD || body_size - GCM_OVERHEAD > MAX_PLAIN) return 0;
        uint64_t sequence = load48(record + 5);
        int duplicate = replayed(s, sequence);
        if (duplicate && (s->state != READY || type != REC_HANDSHAKE)) return 0;
        uint8_t nonce[12], aad[13];
        size_t plaintext_size = body_size - GCM_OVERHEAD;
        memcpy(nonce, s->read_iv, 4); memcpy(nonce + 4, body, 8);
        memcpy(aad, record + 3, 8); memcpy(aad + 8, record, 3);
        store16(aad + 11, (uint16_t)plaintext_size);
        if (aes_gcm_decrypt(&s->read_key, nonce, sizeof(nonce), aad, sizeof(aad),
                body + 8, plaintext_size, body + 8 + plaintext_size, 16, plain) != 1) return 0;
        if (!duplicate) accept_sequence(s, sequence);
        body = plain; body_size = plaintext_size;
    } else if (s->state == READY || s->read_epoch) return 0;

    int result = 0;
    if (type == REC_HANDSHAKE) result = receive_fragments(s, body, body_size, epoch);
    else if (type == REC_CCS && !epoch && body_size == 1 && body[0] == 1) {
        s->pending_ccs = 1; promote_ccs(s);
    } else if (type == REC_APPLICATION && epoch) {
        // UDP can deliver the peer's first application record before Finished.
        // It is authenticated but cannot be exposed before handshake admission.
        if (s->state == READY && body_size)
            result = queue_push(&s->plaintext, body, body_size, MAX_PACKETS);
    } else if (type == REC_ALERT && body_size == 2) {
        if (body[1] == 0 && epoch) {
            const uint8_t alert[] = {1, 0};
            result = emit_record(s, REC_ALERT, 1, alert, sizeof(alert));
            if (!result) result = TURBO_GDTLS_CLOSED;
        }
        else if (body[0] == 2) result = TURBO_GDTLS_PROTOCOL;
    } /* Ignore records that cannot belong to this epoch/content type. */
    cmeta_crypto_clear(plain, sizeof(plain));
    return result;
}

int turbo_gdtls_start(turbo_gdtls *s, uint64_t now_ms) {
    if (!s || s->started || now_ms > UINT64_MAX - FINAL_FLIGHT_LIFETIME) return TURBO_GDTLS_INVALID;
    s->started = 1; s->now = s->started_at = now_ms;
    if (s->server) { s->state = CLIENT_HELLO; return 0; }
    if (rand_bytes(s->client_random, 32) != 1) return stop_session(s, TURBO_GDTLS_AUTH);
    int result = send_client_hello(s);
    return result ? stop_session(s, result) : 0;
}
int turbo_gdtls_receive(turbo_gdtls *s, const void *packet, size_t size, uint64_t now_ms) {
    if (!s || !s->started || (!packet && size) || size > MAX_DATAGRAM || now_ms < s->now ||
        now_ms > UINT64_MAX - FINAL_FLIGHT_LIFETIME) return TURBO_GDTLS_INVALID;
    if (s->state == FAILED) return s->error;
    if (s->state == CLOSED) return TURBO_GDTLS_CLOSED;
    s->now = now_ms;
    if (s->state != READY && now_ms - s->started_at >= HANDSHAKE_LIFETIME)
        return stop_session(s, TURBO_GDTLS_TIMEOUT);
    const uint8_t *p = packet;
    while (size) {
        if (size < RECORD_HEADER) return 0;
        size_t n = load16(p + 11) + RECORD_HEADER;
        if (n > size || n > RECORD_HEADER + MAX_PLAIN + GCM_OVERHEAD ||
            (load16(p + 1) != DTLS12 && load16(p + 1) != DTLS10)) return 0;
        int result = receive_record(s, p, n, 1);
        if (result) return finish_session(s, result, result == TURBO_GDTLS_CLOSED);
        if (s->read_epoch) {
            tstr future;
            while ((future = queue_take(&s->future)) != NULL) {
                result = receive_record(s, (const uint8_t *)future, tstr_len(future), 0);
                clear_string(future);
                if (result) return finish_session(s, result, result == TURBO_GDTLS_CLOSED);
            }
        }
        p += n; size -= n;
    }
    return 0;
}
int turbo_gdtls_poll(turbo_gdtls *s, uint64_t now_ms) {
    if (!s || !s->started || now_ms < s->now || now_ms > UINT64_MAX - FINAL_FLIGHT_LIFETIME)
        return TURBO_GDTLS_INVALID;
    if (s->state == FAILED) return s->error;
    if (s->state == CLOSED) return TURBO_GDTLS_CLOSED;
    s->now = now_ms;
    if (s->state == READY) {
        if (s->deadline && now_ms >= s->deadline) clear_flight(s);
        return 0;
    }
    if (now_ms - s->started_at >= HANDSHAKE_LIFETIME) return stop_session(s, TURBO_GDTLS_TIMEOUT);
    if (s->deadline && now_ms >= s->deadline) {
        int result = resend_flight(s);
        if (result) return stop_session(s, result);
        s->rto = s->rto >= MAX_RTO / 2 ? MAX_RTO : s->rto * 2;
        s->deadline = now_ms + s->rto;
    }
    return 0;
}
uint64_t turbo_gdtls_deadline(const turbo_gdtls *s) {
    if (!s || !s->started || s->state == CLOSED || s->state == FAILED) return 0;
    if (s->state == READY) return s->deadline;
    uint64_t expires = s->started_at + HANDSHAKE_LIFETIME;
    return s->deadline && s->deadline < expires ? s->deadline : expires;
}
tstr turbo_gdtls_take_datagram(turbo_gdtls *s) { return s ? queue_take(&s->output) : NULL; }
tstr turbo_gdtls_take_plaintext(turbo_gdtls *s) {
    return s && s->state == READY ? queue_take(&s->plaintext) : NULL;
}
int turbo_gdtls_ready(const turbo_gdtls *s) { return s && s->state == READY; }
int turbo_gdtls_write(turbo_gdtls *s, const void *data, size_t size) {
    if (!s || !data || !size) return TURBO_GDTLS_INVALID;
    if (s->state != READY) return TURBO_GDTLS_CLOSED;
    int result = emit_record(s, REC_APPLICATION, 1, data, size);
    return result ? stop_session(s, result) : 0;
}
int turbo_gdtls_close(turbo_gdtls *s) {
    if (!s) return TURBO_GDTLS_INVALID;
    if (s->state != READY) return TURBO_GDTLS_CLOSED;
    const uint8_t alert[] = {1, 0};
    int result = emit_record(s, REC_ALERT, 1, alert, sizeof(alert));
    if (result) return stop_session(s, result);
    finish_session(s, TURBO_GDTLS_CLOSED, 1);
    return 0;
}
uint16_t turbo_gdtls_srtp_profile(const turbo_gdtls *s) {
    return s && s->state == READY ? s->selected_profile : 0;
}
int turbo_gdtls_export_srtp(const turbo_gdtls *s, uint8_t *out, size_t capacity, size_t *size) {
    if (size) *size = 0;
    if (!s || !out || !size) return TURBO_GDTLS_INVALID;
    if (s->state != READY) return TURBO_GDTLS_AUTH;
    size_t required = s->selected_profile == 8 ? 88 : s->selected_profile == 7 ? 56 : 60;
    if (capacity < required) return TURBO_GDTLS_CAPACITY;
    if (tls_prf(prf_digest(s), s->master, sizeof(s->master), "EXTRACTOR-dtls_srtp",
                s->client_random, 32, s->server_random, 32, required, out) != 1) {
        cmeta_crypto_clear(out, required); return TURBO_GDTLS_AUTH;
    }
    *size = required; return 0;
}
