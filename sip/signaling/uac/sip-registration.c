#include "sip-registration.h"
#include "sip-internal.h"
#include "sip-register-digest.h"
#include "sip-uac-transaction.h"

#include <core/fmt.h>
#include <core/tstr.h>
#include <core/uuid.h>
#include <crypto/symmetric.h>
#include <limits.h>

enum { SIP_REGISTRATION_VALUE_LIMIT = 511 };
enum { SIP_REGISTRAR_AUTH, SIP_PROXY_AUTH, SIP_REGISTRATION_AUTH_COUNT };

struct sip_registration_reply {
    int code;
    int expires;
    struct sip_register_digest challenge;
};

struct sip_registration {
    struct sip_agent_t* agent;
    struct sip_transport_t transport;
    void* transport_context;
    tstr address_of_record;
    tstr registrar;
    tstr contact;
    tstr username;
    tstr password;
    char callid[SALTS_UUID_STRING_SIZE];
    int cseq;
    int requested_expires;
    int retried_interval;
    int unregistering;
    struct sip_register_digest auth[SIP_REGISTRATION_AUTH_COUNT];
    int renewed_nonce[SIP_REGISTRATION_AUTH_COUNT];
    struct sip_uac_transaction_t* active;
    struct sip_registration_reply reply;
    sip_registration_status_t status;
    uint64_t now;
    uint64_t sent_at;
};

static int registration_value_valid(const char* value)
{
    if (!value || !*value || strlen(value) > SIP_REGISTRATION_VALUE_LIMIT) return 0;
    for (; *value; ++value) {
        unsigned char c = (unsigned char)*value;
        if (c < 32 || c == 127) return 0;
    }
    return 1;
}

sip_registration_t* sip_registration_create(const sip_registration_config_t* config)
{
    if (!config || !config->agent || !config->transport ||
        !config->transport->send || !config->transport->via ||
        !registration_value_valid(config->address_of_record) ||
        !registration_value_valid(config->registrar) ||
        (config->contact && !registration_value_valid(config->contact)) ||
        ((config->username == NULL) != (config->password == NULL)) ||
        (config->username && (!registration_value_valid(config->username) ||
                              strchr(config->username, ':') || !*config->password ||
                              strlen(config->password) > SIP_REGISTRATION_VALUE_LIMIT)) ||
        config->expires_seconds == 0 || config->expires_seconds > INT_MAX) return NULL;

    sip_registration_t* reg = calloc(1, sizeof(*reg));
    if (!reg) return NULL;
    reg->agent = config->agent;
    reg->transport = *config->transport;
    reg->transport_context = config->transport_context;
    reg->requested_expires = (int)config->expires_seconds;

    uuid_t uuid;
    if (uuid_v4_generate(uuid) != SALTS_OK ||
        uuid_format(uuid, reg->callid, sizeof(reg->callid)) != SALTS_OK) {
        sip_registration_destroy(reg);
        return NULL;
    }

    reg->address_of_record = tstr_from_v(vstr_from_cstr(config->address_of_record));
    reg->registrar = tstr_from_v(vstr_from_cstr(config->registrar));
    if (config->contact) reg->contact = tstr_from_v(vstr_from_cstr(config->contact));
    if (config->username) {
        reg->username = tstr_from_v(vstr_from_cstr(config->username));
        reg->password = tstr_from_v(vstr_from_cstr(config->password));
    }
    if (!reg->address_of_record || !reg->registrar || (config->contact && !reg->contact) ||
        (config->username && (!reg->username || !reg->password))) {
        sip_registration_destroy(reg);
        return NULL;
    }
    return reg;
}

static int registration_seconds(const vstr* field)
{
    if (!field) return 0;
    vstr value = vstr_trim(*field, " \t");
    int seconds = 0;
    for (size_t i = 0; i < value.len; ++i) {
        int digit = value.data[i] - '0';
        if (digit < 0 || digit > 9 || seconds > (INT_MAX - digit) / 10) return 0;
        seconds = seconds * 10 + digit;
    }
    return seconds;
}

static const struct sip_contact_t* registration_find_contact(const struct sip_message_t* response,
                                                             const struct sip_uri_t* uri)
{
    for (int i = 0; i < sip_contacts_count(&response->contacts); ++i) {
        const struct sip_contact_t* contact = sip_contacts_get(&response->contacts, i);
        if (sip_uri_equal(uri, &contact->uri)) return contact;
    }
    return NULL;
}

/* A registrar can advertise the observed UDP contact instead of the local
 * address. Require the returned Via mapping of this request, and compare the
 * complete translated URI so another device on the same account cannot match. */
static const struct sip_contact_t* registration_find_mapped_contact(
    const struct sip_message_t* response, const struct sip_message_t* request,
    const struct sip_uri_t* uri)
{
    const struct sip_via_t* sent = sip_vias_get(&request->vias, 0);
    const struct sip_via_t* seen = sip_vias_get(&response->vias, 0);
    const char* at = sip_sv_find_char(&uri->host, '@');
    if (!sent || !seen || !at ||
        sip_sv_compare_cstr_ci(&sent->transport, "UDP") ||
        sip_sv_compare_cstr_ci(&seen->transport, "UDP") ||
        !sip_params_find(&sent->params, "rport", 5) ||
        !vstr_ieq(sent->host, seen->host)) return NULL;

    size_t user_bytes = (size_t)(at - uri->host.data);
    vstr local = vstr_from_buf(at + 1, uri->host.len - user_bytes - 1);
    int port = registration_seconds(sip_params_find_string(&seen->params, "rport", 5));
    vstr received = seen->received;
    if (!vstr_ieq(local, sent->host) || port <= 0 || port > UINT16_MAX ||
        received.len == 0 || received.len > 45) return NULL;
    for (size_t i = 0; i < received.len; ++i) {
        if (!strchr("0123456789abcdefABCDEF:.", received.data[i])) return NULL;
    }

    char host[SIP_REGISTRATION_VALUE_LIMIT + 1];
    vstr userinfo = vstr_from_buf(uri->host.data, user_bytes);
    int n = sip_sv_find_char(&received, ':')
        ? fmt(host, sizeof(host), "{}@[{}]:{}", userinfo, received, port)
        : fmt(host, sizeof(host), "{}@{}:{}", userinfo, received, port);
    if (n < 0 || (size_t)n >= sizeof(host)) return NULL;
    struct sip_uri_t mapped = *uri;
    mapped.host = vstr_from_buf(host, (size_t)n);
    return registration_find_contact(response, &mapped);
}

static int registration_contact_expiry(const struct sip_message_t* response,
                                       const struct sip_message_t* request)
{
    const struct sip_contact_t* binding = sip_contacts_get(&request->contacts, 0);
    if (!binding) return 0;
    const struct sip_contact_t* contact = registration_find_contact(response, &binding->uri);
    if (!contact) contact = registration_find_mapped_contact(response, request, &binding->uri);
    if (!contact) return 0;
    const vstr* expires = sip_params_find_string(&contact->params, "expires", 7);
    return registration_seconds(expires ? expires : sip_message_get_header_by_name(response, "Expires"));
}

/* Both receive and timeout call this under the transaction lock. The owner
 * consumes the reply under that same lock before sending another request. */
static int registration_on_reply(void* context, const struct sip_message_t* response,
                                  struct sip_uac_transaction_t* transaction, int code)
{
    if (code < 200) return 0;
    sip_registration_t* reg = context;
    reg->reply = (struct sip_registration_reply){.code = code};
    if (code >= 200 && code < 300) {
        reg->reply.expires = registration_contact_expiry(response, transaction->req);
    } else if (code == 401 || code == 407) {
        sip_register_digest_challenge(response, code == 401 ? "WWW-Authenticate" : "Proxy-Authenticate",
                                      &reg->reply.challenge);
    } else if (code == 423) {
        reg->reply.expires = registration_seconds(sip_message_get_header_by_name(response, "Min-Expires"));
    }
    return 0;
}

static int registration_add_auth(sip_registration_t* reg, struct sip_uac_transaction_t* transaction)
{
    const char* names[] = {"Authorization", "Proxy-Authorization"};
    if (!reg->auth[SIP_REGISTRAR_AUTH].algorithm && !reg->auth[SIP_PROXY_AUTH].algorithm) return 0;

    char uri[1024];
    int size = sip_message_write_target(transaction->req, uri, uri + sizeof(uri));
    if (size < 0 || (size_t)size >= sizeof(uri)) return -E2BIG;
    uri[size] = '\0';
    for (unsigned i = 0; i < SIP_REGISTRATION_AUTH_COUNT; ++i) {
        if (!reg->auth[i].algorithm) continue;
        char header[UDP_PACKET_SIZE];
        int r = sip_digest_write(&reg->auth[i], reg->username, transaction->req->cseq.method, reg->password, uri,
                                          header, sizeof(header));
        if (r != 0) return r;
        r = sip_uac_add_header(transaction, names[i], header);
        if (r != 0) return r;
    }
    return 0;
}

static void registration_release_transaction(sip_registration_t* reg, int abort)
{
    if (abort) sip_uac_transaction_abort(reg->active);
    sip_uac_transaction_release(reg->active);
    reg->active = NULL;
}

static int registration_send(sip_registration_t* reg)
{
    if (reg->cseq == INT_MAX) return -EOVERFLOW;
    struct sip_uac_transaction_t* t = sip_uac_register(reg->agent, reg->address_of_record,
        reg->registrar, reg->unregistering ? 0 : reg->requested_expires, registration_on_reply, reg);
    if (!t) return -ENOMEM;

    int r = sip_uac_add_header(t, "Call-ID", reg->callid);
    if (r == 0 && reg->contact) {
        r = sip_uac_add_header(t, "Contact", reg->contact);
        if (r == 0 && sip_contacts_count(&t->req->contacts) != 1) r = -EINVAL;
        if (r == 0 && reg->unregistering) {
            struct sip_contact_t* contact = sip_contacts_get(&t->req->contacts, 0);
            for (int i = 0; i < sip_params_count(&contact->params); ++i) {
                struct sip_param_t* parameter = sip_params_get(&contact->params, i);
                if (!sip_sv_compare_cstr_ci(&parameter->name, "expires"))
                    parameter->value = vstr_from_cstr("0");
            }
        }
    }
    if (r == 0) r = registration_add_auth(reg, t);
    if (r == 0) {
        t->req->cseq.id = (uint32_t)++reg->cseq;
        reg->reply.code = 0;
        reg->active = t;
        reg->sent_at = reg->now;
        r = sip_uac_send(t, NULL, 0, &reg->transport, reg->transport_context);
        if (r == 0) {
            for (unsigned i = 0; i < SIP_REGISTRATION_AUTH_COUNT; ++i) {
                if (reg->auth[i].algorithm && reg->auth[i].qop) ++reg->auth[i].nonce_count;
            }
            reg->status.refresh_at_ms = 0;
            return 0;
        }
        registration_release_transaction(reg, 1);
    } else {
        sip_uac_transaction_release(t);
    }
    return r < 0 ? r : -EIO;
}

static uint64_t registration_deadline(uint64_t start, uint64_t delay)
{
    return delay <= UINT64_MAX - start ? start + delay : UINT64_MAX;
}

static int registration_handle_reply(sip_registration_t* reg)
{
    const struct sip_registration_reply* reply = &reg->reply;
    reg->status.sip_code = reply->code;
    if (reply->code >= 200 && reply->code < 300) {
        if (reg->unregistering) {
            reg->status.state = SIP_REGISTRATION_INACTIVE;
            reg->status.expires_at_ms = 0;
            reg->status.refresh_at_ms = 0;
            return 0;
        }
        if (!reply->expires) return -EPROTO;
        uint64_t lifetime = (uint64_t)reply->expires * 1000;
        reg->status.expires_at_ms = registration_deadline(reg->sent_at, lifetime);
        reg->status.refresh_at_ms = registration_deadline(reg->sent_at, lifetime * 4 / 5);
        if (reg->now < reg->status.expires_at_ms) reg->status.state = SIP_REGISTRATION_ACTIVE;
        reg->retried_interval = 0;
        memset(reg->renewed_nonce, 0, sizeof(reg->renewed_nonce));
        return 0;
    }

    if ((reply->code == 401 || reply->code == 407) && reg->username && reply->challenge.algorithm) {
        unsigned i = reply->code == 401 ? SIP_REGISTRAR_AUTH : SIP_PROXY_AUTH;
        struct sip_register_digest* auth = &reg->auth[i];
        const struct sip_register_digest* challenge = &reply->challenge;
        if (!auth->algorithm ||
            (challenge->stale && !reg->renewed_nonce[i] && challenge->algorithm >= auth->algorithm &&
             strcmp(challenge->realm, auth->realm) == 0 && strcmp(challenge->nonce, auth->nonce) != 0)) {
            reg->renewed_nonce[i] |= auth->algorithm != SIP_DIGEST_NONE;
            *auth = *challenge;
            return registration_send(reg);
        }
    } else if (reply->code == 423 && !reg->unregistering &&
               !reg->retried_interval && reply->expires > reg->requested_expires) {
        reg->requested_expires = reply->expires;
        reg->retried_interval = 1;
        return registration_send(reg);
    }

    reg->status.state = SIP_REGISTRATION_FAILED;
    return 0;
}

int sip_registration_poll(sip_registration_t* reg, uint64_t now_ms, sip_registration_status_t* status)
{
    if (!reg || !status || now_ms < reg->now) return -EINVAL;
    reg->now = now_ms;
    if (reg->status.state == SIP_REGISTRATION_ACTIVE && now_ms >= reg->status.expires_at_ms)
        reg->status.state = SIP_REGISTRATION_PENDING;

    int r = 0;
    if (reg->active) {
        sync_mutex_lock(&reg->active->locker);
        int complete = reg->reply.code != 0;
        sync_mutex_unlock(&reg->active->locker);
        if (complete) {
            registration_release_transaction(reg, 0);
            r = registration_handle_reply(reg);
        }
    } else if (reg->status.state != SIP_REGISTRATION_FAILED &&
               reg->status.state != SIP_REGISTRATION_INACTIVE && now_ms >= reg->status.refresh_at_ms) {
        r = registration_send(reg);
    }
    if (r != 0) {
        reg->status.error = r;
        reg->status.state = SIP_REGISTRATION_FAILED;
    }
    if (reg->status.state == SIP_REGISTRATION_FAILED) reg->status.refresh_at_ms = 0;
    *status = reg->status;
    return 0;
}

int sip_registration_unregister(sip_registration_t* reg)
{
    if (!reg) return -EINVAL;
    if (reg->status.state == SIP_REGISTRATION_INACTIVE || reg->unregistering)
        return -EALREADY;
    if (reg->active) registration_release_transaction(reg, 1);
    reg->unregistering = 1;
    reg->status = (sip_registration_status_t){.state = SIP_REGISTRATION_PENDING};
    memset(reg->renewed_nonce, 0, sizeof(reg->renewed_nonce));
    return 0;
}

void sip_registration_destroy(sip_registration_t* reg)
{
    if (!reg) return;
    if (reg->active) registration_release_transaction(reg, 1);
    if (reg->password) salts_crypto_clear(reg->password, tstr_len(reg->password));
    tstr_free(reg->password);
    tstr_free(reg->username);
    tstr_free(reg->contact);
    tstr_free(reg->registrar);
    tstr_free(reg->address_of_record);
    free(reg);
}
