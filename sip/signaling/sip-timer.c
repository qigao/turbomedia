#include "sip-timer.h"
#include "sip-internal.h"

#include "core/platform.h"
#include "threads/threads.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

enum sip_timer_state_t
{
	SIP_TIMER_ACTIVE,
	SIP_TIMER_FIRING,
	SIP_TIMER_STOPPED
};

struct sip_timer_impl_t
{
	native_timer_t* native_timer;
	sip_timer_handle handler;
	void* usrptr;
	atomic_int state;
};

static void sip_timer_destroy_after_callback(void* param)
{
	struct sip_timer_impl_t* timer;
	timer = (struct sip_timer_impl_t*)param;
	native_timer_destroy(timer->native_timer);
	free(timer);
}

static void sip_timer_on_timeout(native_timer_t* native_timer)
{
	int expected;
	sync_thread_t cleanup_thread;
	struct sip_timer_impl_t* timer;

	timer = (struct sip_timer_impl_t*)native_timer_get_data(native_timer);
	if (NULL == timer)
		return;

	expected = SIP_TIMER_ACTIVE;
	if (!atomic_compare_exchange_strong_explicit(&timer->state, &expected, SIP_TIMER_FIRING,
		memory_order_acq_rel, memory_order_acquire))
		return;

	/* native_timer_destroy waits for an in-flight callback on POSIX. Destroying
	 * from a detached worker preserves the SIP timer callback lifecycle on all
	 * Salts platforms without a platform-specific branch here. */
	cleanup_thread = NULL;
	if (0 != sync_thread_create(&cleanup_thread, sip_timer_destroy_after_callback, timer))
		abort();
	sync_thread_destroy(&cleanup_thread);

	timer->handler(timer->usrptr);
}

sip_timer_t sip_timer_start(int timeout, sip_timer_handle handler, void* usrptr)
{
	struct sip_timer_impl_t* timer;

	if (timeout < 0 || NULL == handler)
		return NULL;

	timer = (struct sip_timer_impl_t*)calloc(1, sizeof(*timer));
	if (NULL == timer)
		return NULL;

	timer->native_timer = native_timer_create(NULL);
	if (NULL == timer->native_timer)
	{
		free(timer);
		return NULL;
	}

	timer->handler = handler;
	timer->usrptr = usrptr;
	atomic_init(&timer->state, SIP_TIMER_ACTIVE);
	native_timer_set_data(timer->native_timer, timer);
	if (0 != native_timer_start(timer->native_timer, sip_timer_on_timeout, (uint64_t)timeout, 0U))
	{
		native_timer_destroy(timer->native_timer);
		free(timer);
		return NULL;
	}

	return timer;
}

int sip_timer_stop(sip_timer_t* id)
{
	int previous;
	struct sip_timer_impl_t* timer;

	if (NULL == id || NULL == *id)
		return -1;

	timer = (struct sip_timer_impl_t*)*id;
	*id = NULL;
	previous = atomic_exchange_explicit(&timer->state, SIP_TIMER_STOPPED, memory_order_acq_rel);
	if (SIP_TIMER_ACTIVE != previous)
		return -1;

	native_timer_destroy(timer->native_timer);
	free(timer);
	return 0;
}

struct sip_owner_timer
{
	struct sip_owner_timer* next;
	uint64_t deadline;
	sip_timer_handle handler;
	void* context;
	int firing;
};

sip_timer_t sip_agent_timer_start(struct sip_agent_t* sip, int timeout, sip_timer_handle handler, void* context)
{
	if (!sip->owner_timers)
		return sip_timer_start(timeout, handler, context);
	if (timeout < 0 || (uint64_t)timeout > UINT64_MAX - sip->now_ms)
		return NULL;
	struct sip_owner_timer* timer = calloc(1, sizeof(*timer));
	if (!timer)
		return NULL;
	timer->deadline = sip->now_ms + (uint64_t)timeout;
	timer->handler = handler;
	timer->context = context;
	struct sip_owner_timer** next = &sip->deadlines;
	while (*next && (*next)->deadline <= timer->deadline)
		next = &(*next)->next;
	timer->next = *next;
	*next = timer;
	return timer;
}

int sip_agent_timer_stop(struct sip_agent_t* sip, sip_timer_t* id)
{
	if (!sip->owner_timers)
		return sip_timer_stop(id);
	struct sip_owner_timer* timer = *id;
	*id = NULL;
	if (!timer || timer->firing)
		return -1;
	struct sip_owner_timer** next = &sip->deadlines;
	while (*next != timer)
		next = &(*next)->next;
	*next = timer->next;
	free(timer);
	return 0;
}

void sip_agent_poll_timers(struct sip_agent_t* sip, uint64_t now_ms)
{
	assert(sip->owner_timers && now_ms >= sip->now_ms);
	sip->now_ms = now_ms;
	while (sip->deadlines && sip->deadlines->deadline <= now_ms)
	{
		struct sip_owner_timer* timer = sip->deadlines;
		sip->deadlines = timer->next;
		timer->firing = 1;
		timer->handler(timer->context);
		free(timer);
	}
}
