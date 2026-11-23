#include "sip-uas-transaction.h"
#include "sip-internal.h"

int sip_uas_oncancel(struct sip_uas_transaction_t* t, const struct sip_message_t* req, void* param)
{
	int r;
	char ptr[256];
	vstr id;
	struct sip_uas_transaction_t* origin;

	sip_dialog_id_with_message(&id, req, ptr, sizeof(ptr), 1);
	
	// 487 Request Terminated
	// CANCEL has no effect on a request to which a UAS has already given a final response

	sync_mutex_lock(&t->agent->locker);
	origin = sip_uas_find_transaction(t->agent, req, 0);
	sync_mutex_unlock(&t->agent->locker);
	if (!origin)
	{
		// 481 Call Leg/Transaction Does Not Exist
		return sip_uas_transaction_noninvite_reply(t, 481, NULL, 0, param);
	}

	sync_mutex_lock(&origin->locker);
	if (origin->status > SIP_UAS_TRANSACTION_PROCEEDING)
	{
		sync_mutex_unlock(&origin->locker);
		// If it has already sent a final response for the original request, 
		// the CANCEL request has no effect on the processing of the original 
		// request, no effect on any session state, and no effect on the 
		// responses generated for the original request.

		r = sip_uas_transaction_noninvite_reply(t, 200, NULL, 0, param);
	}
	else
	{
		// the To tag of the response to the CANCEL and the To tag
		// in the response to the original request SHOULD be the same.
		t->reply->ptr.ptr = sip_string_view_clone(t->reply->ptr.ptr, t->reply->ptr.end, &t->reply->to.tag, origin->reply->to.tag.data, origin->reply->to.tag.len);
		sync_mutex_unlock(&origin->locker);
		r = t->reply->ptr.ptr == t->reply->ptr.end ? -E2BIG
			: sip_params_add_or_update(&t->reply->to.params, "tag", 3, &t->reply->to.tag);
		if (r == 0)
			r = t->handler->oncancel ? t->handler->oncancel(param, req, t, &id)
				: sip_uas_transaction_noninvite_reply(t, 501, NULL, 0, param);
	}

	sip_uas_transaction_release(origin);
	return r;
}
