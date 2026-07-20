#ifndef _sip_header_h_
#define _sip_header_h_

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "sip-string-view.h"
#include "turbo_vec.h"

#if defined(__cplusplus)
extern "C" {
#endif

// 10.2.6 Discovering a Registrar (p62)
#define SIP_MULTICAST_HOST "sip.mcast.net"
#define SIP_MULTICAST_ADDRESS "224.0.1.75"

#define SIP_VEC_DECLARE(name)				\
	struct name##s_t						\
	{										\
		turbo_vec_t items;					\
	};										\
											\
	void name##s_init(struct name##s_t* p);	\
	void name##s_free(struct name##s_t* p);	\
	int name##s_count(const struct name##s_t* p);	\
	int name##s_push(struct name##s_t* p, struct name##_t* item); \
	struct name##_t* name##s_get(const struct name##s_t* p, int index);

#define SIP_VEC_IMPLEMENT(name, initial_capacity)                                                 \
  void name##s_init(struct name##s_t *items) {                                                    \
    if (turbo_vec_init(&items->items, sizeof(struct name##_t)) != TURBO_OK ||                    \
        turbo_vec_reserve(&items->items, (initial_capacity)) != TURBO_OK)                         \
      abort();                                                                                    \
  }                                                                                               \
  void name##s_free(struct name##s_t *items) {                                                    \
    size_t index;                                                                                 \
    for (index = 0U; index < turbo_vec_size(&items->items); ++index)                              \
      name##_free((struct name##_t *)turbo_vec_at(&items->items, index));                         \
    turbo_vec_destroy(&items->items);                                                             \
  }                                                                                               \
  int name##s_push(struct name##s_t *items, struct name##_t *item) {                             \
    return turbo_vec_push(&items->items, item);                                                   \
  }                                                                                               \
  struct name##_t *name##s_get(const struct name##s_t *items, int index) {                       \
    if (index < 0) return NULL;                                                                   \
    return (struct name##_t *)turbo_vec_at((turbo_vec_t *)&items->items, (size_t)index);          \
  }                                                                                               \
  int name##s_count(const struct name##s_t *items) {                                             \
    return (int)turbo_vec_size(&items->items);                                                    \
  }


struct sip_param_t
{
	tstr_v name;
	tstr_v value;
};
SIP_VEC_DECLARE(sip_param);

struct sip_uri_t
{
	tstr_v scheme;
	tstr_v host; // userinfo@host:port
	
	struct sip_params_t parameters;
	tstr_v transport; // udp/tcp/sctp/tls/other
	tstr_v method;
	tstr_v maddr; // the server address to be contacted for this user, overriding any address derived from the host field
	tstr_v user; // phone/ip
	int ttl;
	int lr;
	int rport; // 0-not found, -1-no-value, other-value

	struct sip_params_t headers;
};
SIP_VEC_DECLARE(sip_uri);

struct sip_requestline_t
{
	tstr_v method;
	struct sip_uri_t uri;
};

struct sip_statusline_t
{
	int code;
	int verminor, vermajor;
	char protocol[64];
	tstr_v reason;
};

struct sip_contact_t
{
	struct sip_uri_t uri;
	tstr_v nickname;

	// parameters
	tstr_v tag; // TO/FROM
	double q; // c-p-q
	int64_t expires; // delta-seconds, default 3600
	struct sip_params_t params; // include tag/q/expires
};
SIP_VEC_DECLARE(sip_contact);

struct sip_via_t
{
	tstr_v protocol;
	tstr_v version;
	tstr_v transport;
	tstr_v host; // sent-by host:port

	// parameters
	tstr_v branch; // token
	tstr_v maddr; // host
	tstr_v received; // IPv4address / IPv6address
	int ttl; // 0-255
	int rport; // 0-not found, -1-no-value, other-value
	struct sip_params_t params; // include branch/maddr/received/ttl/rport
};
SIP_VEC_DECLARE(sip_via);

struct sip_cseq_t
{
	uint32_t id;
	tstr_v method;
};

struct sip_substate_t
{
	tstr_v state;

	// parameters
	tstr_v reason;
	uint32_t expires; // expires
	uint32_t retry; // retry-after
	struct sip_params_t params; // include reason/expires/retry
};

struct sip_event_t
{
	tstr_v event;

	// parameters
	tstr_v id;
	struct sip_params_t params; // include id
};

int sip_header_param(const char* s, const char* end, struct sip_param_t* param);
int sip_header_params(char sep, const char* s, const char* end, struct sip_params_t* params);
int sip_param_write(const struct sip_param_t* param, char* data, const char* end);
int sip_params_write(const struct sip_params_t* params, char* data, const char* end, char sep);
const struct sip_param_t* sip_params_find(const struct sip_params_t* params, const char* name, int bytes);
const tstr_v* sip_params_find_string(const struct sip_params_t* params, const char* name, int bytes);
int sip_params_find_int(const struct sip_params_t* params, const char* name, int bytes, int* value);
int sip_params_find_int64(const struct sip_params_t* params, const char* name, int bytes, int64_t* value);
int sip_params_find_double(const struct sip_params_t* params, const char* name, int bytes, double* value);
int sip_params_add_or_update(struct sip_params_t* params, const char* name, int bytes, const tstr_v* value);

/// @return 0-ok, other-error
int sip_header_cseq(const char* s, const char* end, struct sip_cseq_t* cseq);
/// @return write length, >0-ok, <0-error
int sip_cseq_write(const struct sip_cseq_t* cseq, char* data, const char* end);

int sip_header_uri(const char* s, const char* end, struct sip_uri_t* uri);
int sip_uri_write(const struct sip_uri_t* uri, char* data, const char* end);
int sip_uri_equal(const struct sip_uri_t* l, const struct sip_uri_t* r);
int sip_uri_username(const struct sip_uri_t* uri, tstr_v* user);
int sip_request_uri_write(const struct sip_uri_t* uri, char* data, const char* end);

int sip_header_via(const char* s, const char* end, struct sip_via_t* via);
int sip_header_vias(const char* s, const char* end, struct sip_vias_t* vias);
int sip_via_write(const struct sip_via_t* via, char* data, const char* end);
const tstr_v* sip_vias_top_branch(const struct sip_vias_t* vias);

int sip_header_route(const char* s, const char* end, struct sip_uri_t* route);
int sip_header_routes(const char* s, const char* end, struct sip_uris_t* route);
int sip_route_write(const struct sip_uri_t* route, char* data, const char* end);

int sip_header_contact(const char* s, const char* end, struct sip_contact_t* contact);
int sip_header_contacts(const char* s, const char* end, struct sip_contacts_t* contacts);
int sip_contact_write(const struct sip_contact_t* contact, char* data, const char* end);
int sip_contacts_match_any(const struct sip_contacts_t* contacts);

char* sip_string_view_clone(char* ptr, const char* end, tstr_v* clone, const char* s, size_t n);
char* sip_uri_clone(char* ptr, const char* end, struct sip_uri_t* clone, const struct sip_uri_t* uri);
char* sip_via_clone(char* ptr, const char* end, struct sip_via_t* clone, const struct sip_via_t* via);
char* sip_contact_clone(char* ptr, const char* end, struct sip_contact_t* clone, const struct sip_contact_t* contact);

void sip_uri_free(struct sip_uri_t* uri);
void sip_via_free(struct sip_via_t* via);
void sip_contact_free(struct sip_contact_t* contact);
void sip_substate_free(struct sip_substate_t* substate);
void sip_event_free(struct sip_event_t* event);

/// @return 0-ok, other-error
int sip_header_substate(const char* s, const char* end, struct sip_substate_t* substate);
/// @return write length, >0-ok, <0-error
int sip_substate_write(const struct sip_substate_t* substate, char* data, const char* end);

/// @return 0-ok, other-error
int sip_header_event(const char* s, const char* end, struct sip_event_t* evt);
/// @return write length, >0-ok, <0-error
int sip_event_write(const struct sip_event_t* evt, char* data, const char* end);
/// @return 1-true, 0-false
int sip_event_equal(const struct sip_event_t* l, const struct sip_event_t* r);

#if defined(__cplusplus)
}
#endif
#endif /* !_sip_header_h_ */
