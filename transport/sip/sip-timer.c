#include "sip-timer.h"

#include "platform.h"
#include "turbo_thread.h"

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
	turbo_timer_t* native_timer;
	sip_timer_handle handler;
	void* usrptr;
	atomic_int state;
};

static void sip_timer_destroy_after_callback(void* param)
{
	struct sip_timer_impl_t* timer;
	timer = (struct sip_timer_impl_t*)param;
	turbo_timer_destroy(timer->native_timer);
	free(timer);
}

static void sip_timer_on_timeout(turbo_timer_t* native_timer)
{
	int expected;
	turbo_thread_t cleanup_thread;
	struct sip_timer_impl_t* timer;

	timer = (struct sip_timer_impl_t*)turbo_timer_get_data(native_timer);
	if (NULL == timer)
		return;

	expected = SIP_TIMER_ACTIVE;
	if (!atomic_compare_exchange_strong_explicit(&timer->state, &expected, SIP_TIMER_FIRING,
		memory_order_acq_rel, memory_order_acquire))
		return;

	/* turbo_timer_destroy waits for an in-flight callback on POSIX. Destroying
	 * from a detached worker preserves the SIP timer callback lifecycle on all
	 * TurboUtils platforms without a platform-specific branch here. */
	cleanup_thread = NULL;
	if (0 != turbo_thread_create(&cleanup_thread, sip_timer_destroy_after_callback, timer))
		abort();
	turbo_thread_destroy(&cleanup_thread);

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

	timer->native_timer = turbo_timer_create(NULL);
	if (NULL == timer->native_timer)
	{
		free(timer);
		return NULL;
	}

	timer->handler = handler;
	timer->usrptr = usrptr;
	atomic_init(&timer->state, SIP_TIMER_ACTIVE);
	turbo_timer_set_data(timer->native_timer, timer);
	if (0 != turbo_timer_start(timer->native_timer, sip_timer_on_timeout, (uint64_t)timeout, 0U))
	{
		turbo_timer_destroy(timer->native_timer);
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

	turbo_timer_destroy(timer->native_timer);
	free(timer);
	return 0;
}
