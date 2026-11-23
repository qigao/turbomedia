#ifndef _sip_internal_h_
#define _sip_internal_h_

#include "sip-agent.h"
#include "sip-atomic.h"
#include "sip-message.h"
#include "sip-timer.h"
#include "core/platform.h"
#include "core/error.h"
#include "threads/threads.h"
#include <cstl/vec.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

struct sip_gc_t
{
	sip_atomic_i32_t uac; // uac transaction
	sip_atomic_i32_t uas; // uas transaction
	sip_atomic_i32_t dialog; // dialog
	sip_atomic_i32_t message; // dialog
	sip_atomic_i32_t subscribe;
};

struct sip_uac_transaction_t;
struct sip_uas_transaction_t;
struct sip_owner_timer;
enum { SIP_OWNER_MAX_TRANSACTIONS = 64 };

struct sip_agent_t
{
	sip_atomic_i32_t ref;
	sync_mutex_t locker;
	int owner_timers;
	uint64_t now_ms;
	struct sip_owner_timer* deadlines;

	//struct sip_timer_t timer;
	//void* timerptr;

	vec_t uac; // sip_uac_transaction_t pointers
	vec_t uas; // sip_uas_transaction_t pointers
	struct sip_uas_handler_t handler;
};

int sip_uac_input(struct sip_agent_t* sip, struct sip_message_t* reply);
int sip_uas_input(struct sip_agent_t* sip, const struct sip_message_t* request, void* param);
int sip_message_write_target(const struct sip_message_t* msg, char* p, const char* end);
int sip_dialog_confirm_uac(struct sip_dialog_t* dialog, const struct sip_message_t* reply);
struct sip_agent_t* sip_agent_create_owner(struct sip_uas_handler_t* handler);
void sip_agent_poll_timers(struct sip_agent_t* sip, uint64_t now_ms);
void sip_agent_abort_transactions(struct sip_agent_t* sip);
sip_timer_t sip_agent_timer_start(struct sip_agent_t* sip, int timeout, sip_timer_handle handler, void* context);
int sip_agent_timer_stop(struct sip_agent_t* sip, sip_timer_t* id);

static inline int sip_transport_isreliable(const vstr* c)
{
	return (0 == sip_sv_compare_cstr_ci(c, "TCP") || 0 == sip_sv_compare_cstr_ci(c, "TLS") || 0 == sip_sv_compare_cstr_ci(c, "SCTP")) ? 1 : 0;
}
static inline int sip_transport_isreliable2(const char* protocol)
{
	vstr c;
	c.data = protocol;
	c.len = strlen(protocol);
	return sip_transport_isreliable(&c);
}

extern struct sip_gc_t s_gc;

static inline int sip_random_u32(uint32_t* value)
{
	if (!value)
		return -1;
	return secure_random(value, sizeof(*value));
}

static inline int sip_random_u31(uint32_t* value)
{
	uint32_t random_value;
	if (!value || 0 != sip_random_u32(&random_value))
		return -1;
	*value = random_value & UINT32_C(0x7FFFFFFF);
	return 0;
}

static inline int sip_int_min(int left, int right)
{
	return left < right ? left : right;
}

static inline int sip_int_max(int left, int right)
{
	return left > right ? left : right;
}

static inline int sip_random_u64(uint64_t* value)
{
	if (!value)
		return -1;
	return secure_random(value, sizeof(*value));
}

#endif /* !_sip_internal_h_ */
