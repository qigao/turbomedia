/*
* 4.1.  Subscriber Behavior
* https://www.rfc-editor.org/rfc/rfc6665#section-4.1.2
					
						  +-------------+
						  |    init     |<-----------------------+
						  +-------------+                        |
								 |                           Retry-after
						   Send SUBSCRIBE                    expires
								 |                               |
								 V          Timer N Fires;       |
						  +-------------+   SUBSCRIBE failure    |
			 +------------| notify_wait |-- response; --------+  |
			 |            +-------------+   or NOTIFY,        |  |
			 |                   |          state=terminated  |  |
			 |                   |                            |  |
   ++========|===================|============================|==|====++
   ||        |                   |                            V  |    ||
   ||  Receive NOTIFY,    Receive NOTIFY,             +-------------+ ||
   ||  state=active       state=pending               | terminated  | ||
   ||        |                   |                    +-------------+ ||
   ||        |                   |          Re-subscription     A  A  ||
   ||        |                   V          times out;          |  |  ||
   ||        |            +-------------+   Receive NOTIFY,     |  |  ||
   ||        |            |   pending   |-- state=terminated; --+  |  ||
   ||        |            +-------------+   or 481 response        |  ||
   ||        |                   |          to SUBSCRIBE           |  ||
   ||        |            Receive NOTIFY,   refresh                |  ||
   ||        |            state=active                             |  ||
   ||        |                   |          Re-subscription        |  ||
   ||        |                   V          times out;             |  ||
   ||        |            +-------------+   Receive NOTIFY,        |  ||
   ||        +----------->|   active    |-- state=terminated; -----+  ||
   ||                     +-------------+   or 481 response           ||
   ||                                       to SUBSCRIBE              ||
   || Subscription                          refresh                   ||
   ++=================================================================++


* 4.2.  Notifier Behavior
* https://www.rfc-editor.org/rfc/rfc6665#section-4.2.2

						 +-------------+
						 |    init     |
						 +-------------+
								|
						  Receive SUBSCRIBE,
						  Send NOTIFY
								|
								V          NOTIFY failure,
						 +-------------+   subscription expires,
			+------------|  resp_wait  |-- or terminated ----+
			|            +-------------+   per local policy  |
			|                   |                            |
			|                   |                            |
			|                   |                            V
	  Policy grants       Policy needed              +-------------+
	  permission                |                    | terminated  |
			|                   |                    +-------------+
			|                   |                               A A
			|                   V          NOTIFY failure,      | |
			|            +-------------+   subscription expires,| |
			|            |   pending   |-- or terminated -------+ |
			|            +-------------+   per local policy       |
			|                   |                                 |
			|            Policy changed to                        |
			|            grant permission                         |
			|                   |                                 |
			|                   V          NOTIFY failure,        |
			|            +-------------+   subscription expires,  |
			+----------->|   active    |-- or terminated ---------+
						 +-------------+   per local policy

*/

#include "sip-subscribe.h"
#include "sip-internal.h"
#include "sip-message.h"
#include "sip-subscribe.h"
#include <stdlib.h>

#define N 512

// 12.1.2 UAC Behavior
// A UAC MUST be prepared to receive a response without a tag in the To
// field, in which case the tag is considered to have a value of null.
// This is to maintain backwards compatibility with RFC 2543, which
// did not mandate To tags.
static const tstr_v sc_null = { "", 0 };

struct sip_subscribe_t* sip_subscribe_create(const struct sip_event_t* event)
{
	char* end;
	struct sip_subscribe_t* s;
	s = (struct sip_subscribe_t*)calloc(1, sizeof(*s) + N);
	if (s)
	{
		s->ref = 1;
		s->state = SUBSCRIBE_INIT;
		s->ptr = (char*)(s + 1);

		end = s->ptr + N;
		s->ptr = sip_string_view_clone(s->ptr, end, &s->event.event, event->event.data, event->event.len);
		s->ptr = sip_string_view_clone(s->ptr, end, &s->event.id, event->id.data, event->id.len);
		sip_atomic_increment(&s_gc.subscribe);
	}
	return s;
}

int sip_subscribe_release(struct sip_subscribe_t* subscribe)
{
	if (!subscribe)
		return -1;

	assert(subscribe->ref > 0);
	if (0 != sip_atomic_decrement(&subscribe->ref))
		return 0;

	if (subscribe->dialog)
		sip_dialog_release(subscribe->dialog);

	//sip_event_free(&subscribe->event); // event->params don't init
	free(subscribe);
	sip_atomic_decrement(&s_gc.subscribe);
	return 0;
}

int sip_subscribe_addref(struct sip_subscribe_t* subscribe)
{
	assert(subscribe->ref > 0);
	return sip_atomic_increment(&subscribe->ref);
}

/// @return 1-match, 0-don't match
static int sip_subscribe_match(const struct sip_subscribe_t* subscribe, const tstr_v* callid, const tstr_v* local, const tstr_v* remote, const struct sip_event_t* event)
{
	assert(subscribe && local);
	if (!remote) remote = &sc_null;

	return sip_sv_equal(callid, &subscribe->dialog->callid) && sip_sv_equal(local, &subscribe->dialog->local.uri.tag) && sip_sv_equal(remote, &subscribe->dialog->remote.uri.tag) && sip_event_equal(event, &subscribe->event) ? 1 : 0;
}

struct sip_subscribe_t* sip_subscribe_internal_create(struct sip_agent_t* sip, const struct sip_message_t* msg, const struct sip_event_t* event, int uac)
{
	struct sip_subscribe_t* subscribe;
	subscribe = sip_subscribe_create(event);
	if (!subscribe)
	{
		turbo_mutex_unlock(&sip->locker);
		return NULL; // exist
	}

	subscribe->dialog = sip_dialog_create();
	if (!subscribe->dialog || 0 != (uac ? sip_dialog_init_uac(subscribe->dialog, msg) : sip_dialog_init_uas(subscribe->dialog, msg)))
	{
		sip_subscribe_release(subscribe);
		return NULL;
	}
	subscribe->dialog->state = DIALOG_CONFIRMED; // confirm dialog
	return subscribe;
}

int sip_subscribe_id(tstr_v* id, const struct sip_subscribe_t* subscribe, char* ptr, int len)
{
	int r;
	r = subscribe ? snprintf(ptr, len, "%.*s@%.*s@%.*s@%.*s@%.*s", (int)subscribe->dialog->callid.len, subscribe->dialog->callid.data, (int)subscribe->dialog->local.uri.tag.len, subscribe->dialog->local.uri.tag.data, (int)subscribe->dialog->remote.uri.tag.len, subscribe->dialog->remote.uri.tag.data, (int)subscribe->event.event.len, subscribe->event.event.data, (int)subscribe->event.id.len, subscribe->event.id.data) : 0;
	id->data = ptr;
	id->len = r > 0 && r < len ? r : 0;
	return r;
}

// @param[in] uas 1-local is uas
int sip_subscribe_id_with_message(tstr_v* id, const struct sip_message_t* msg, char* ptr, int len, int uas)
{
	int r;
	assert(msg->mode == SIP_MESSAGE_REQUEST);
	if (uas)
		r = snprintf(ptr, len, "%.*s@%.*s@%.*s@%.*s@%.*s", (int)msg->callid.len, msg->callid.data, (int)msg->to.tag.len, msg->to.tag.data, (int)msg->from.tag.len, msg->from.tag.data, (int)msg->event.event.len, msg->event.event.data, (int)msg->event.id.len, msg->event.id.data);
	else
		r = snprintf(ptr, len, "%.*s@%.*s@%.*s@%.*s@%.*s", (int)msg->callid.len, msg->callid.data, (int)msg->from.tag.len, msg->from.tag.data, (int)msg->to.tag.len, msg->to.tag.data, (int)msg->event.event.len, msg->event.event.data, (int)msg->event.id.len, msg->event.id.data);

	id->data = ptr;
	id->len = r > 0 && r < len ? r : 0;
	return r;
}
