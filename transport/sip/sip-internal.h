#ifndef _sip_internal_h_
#define _sip_internal_h_

#include "sip-agent.h"
#include "sip-atomic.h"
#include "sip-message.h"
#include "platform.h"
#include "turbo_thread.h"
#include "turbo_vec.h"
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

struct sip_agent_t
{
	sip_atomic_i32_t ref;
	turbo_mutex_t locker;

	//struct sip_timer_t timer;
	//void* timerptr;

	turbo_vec_t uac; // sip_uac_transaction_t pointers
	turbo_vec_t uas; // sip_uas_transaction_t pointers
	struct sip_uas_handler_t handler;
};

int sip_uac_input(struct sip_agent_t* sip, struct sip_message_t* reply);
int sip_uas_input(struct sip_agent_t* sip, const struct sip_message_t* request, void* param);

static inline int sip_transport_isreliable(const tstr_v* c)
{
	return (0 == sip_sv_compare_cstr_ci(c, "TCP") || 0 == sip_sv_compare_cstr_ci(c, "TLS") || 0 == sip_sv_compare_cstr_ci(c, "SCTP")) ? 1 : 0;
}
static inline int sip_transport_isreliable2(const char* protocol)
{
	tstr_v c;
	c.data = protocol;
	c.len = strlen(protocol);
	return sip_transport_isreliable(&c);
}

extern struct sip_gc_t s_gc;

static inline int sip_random_u32(uint32_t* value)
{
	if (!value)
		return -1;
	return turbo_secure_random(value, sizeof(*value));
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
	return turbo_secure_random(value, sizeof(*value));
}

#endif /* !_sip_internal_h_ */
