/*
                                    |INVITE from TU
                  Timer A fires     |INVITE sent      Timer B fires
                  Reset A,          V                 or Transport Err.
                  INVITE sent +-----------+           inform TU
                    +---------|           |--------------------------+
                    |         |  Calling  |                          |
                    +-------->|           |-----------+              |
   300-699                    +-----------+ 2xx       |              |
   ACK sent                      |  |       2xx to TU |              |
   resp. to TU                   |  |1xx              |              |
   +-----------------------------+  |1xx to TU        |              |
   |                                |                 |              |
   |                1xx             V                 |              |
   |                1xx to TU +-----------+           |              |
   |                +---------|           |           |              |
   |                |         |Proceeding |           |              |
   |                +-------->|           |           |              |
   |                          +-----------+ 2xx       |              |
   |         300-699             |    |     2xx to TU |              |
   |         ACK sent,  +--------+    +---------------+              |
   |         resp. to TU|                             |              |
   |                    |                             |              |
   |                    V                             V              |
   |              +-----------+                   +----------+       |
   +------------->|           |Transport Err.     |          |       |
                  | Completed |Inform TU          | Accepted |       |
               +--|           |-------+           |          |-+     |
       300-699 |  +-----------+       |           +----------+ |     |
       ACK sent|    ^  |              |               |  ^     |     |
               |    |  |              |               |  |     |     |
               +----+  |              |               |  +-----+     |
                       |Timer D fires |  Timer M fires|    2xx       |
                       |-             |             - |    2xx to TU |
                       +--------+     |   +-----------+              |
      NOTE:                     V     V   V                          |
   Transitions                 +------------+                        |
   are labeled                 |            |                        |
   with the event              | Terminated |<-----------------------+
   over the action             |            |
   to take.                    +------------+

					Figure 5: INVITE client transaction
*/

#include "sip-uac-transaction.h"
#include "sip-uac.h"
#include "sip-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

static int sip_uac_transaction_invite_proceeding(struct sip_uac_transaction_t* t, const struct sip_message_t* reply)
{
	int r;
	char ptr[256];
	vstr id;

	// TODO: add dialog locker here
	if (!t->dialog && sip_sv_valid(&reply->to.tag))
	{
		// create early dialog
		t->dialog = sip_dialog_create();
		if (!t->dialog) return -1;
		if (0 != sip_dialog_init_uac(t->dialog, reply) || 0 != sip_dialog_set_local_target(t->dialog, t->req))
		{
			assert(0);
			sip_dialog_release(t->dialog);
			t->dialog = NULL;
			return 0; // ignore
		}
	}

	sip_dialog_id(&id, t->dialog, ptr, sizeof(ptr));
	// 17.1.1.2 Formal Description (p126)
	// the provisional response MUST be passed to the TU. 
	// Any further provisional responses MUST be passed up to the TU while in the "Proceeding" state.
	r = t->oninvite(t->param, reply, t, t->dialog, &id, reply->u.s.code); // ignore session

	// reset timer b for proceeding too long
    sip_uac_transaction_timeout(t, TIMER_B);
    return r;
}

static int sip_uac_transaction_invite_completed(struct sip_uac_transaction_t* t, const struct sip_message_t* reply, int retransmissions)
{
	int r;
	char ptr[256];
	vstr id;
	
	r = 0;

	if (!retransmissions)
	{
		sip_dialog_id(&id, t->dialog, ptr, sizeof(ptr));

		// Any retransmissions of the final response that are received while in
		// the "Completed" state MUST cause the ACK to be re-passed to the
		// transport layer for retransmission, but the newly received response
		// MUST NOT be passed up to the TU
		r = t->oninvite(t->param, reply, t, t->dialog, &id, reply->u.s.code); // ignore session
	}
	else
	{
		// 1. miss order / in-flight
		// 2. fork invite
	}

	// 17.1.1.3 Construction of the ACK Request
	// The ACK MUST be sent to the same address, port, 
	// and transport to which the original request was sent.
	sip_uac_ack_3456xx(t, reply); // ignore ack transport layer error, retry send on retransmissions

	if (!retransmissions)
	{
		// The client transaction SHOULD start timer D when it enters the
		// "Completed" state, with a value of at least 32 seconds for unreliable
		// transports, and a value of zero seconds for reliable transports.

		// Timer D reflects the amount of time that the server transaction can
		// remain in the "Completed" state when unreliable transports are used.
		// This is equal to Timer H in the INVITE server transaction, whose
		// default is 64*T1. However, the client transaction does not know the
		// value of T1 in use by the server transaction, so an absolute minimum
		// of 32s is used instead of basing Timer D on T1.

		// transfer to terminated
		sip_uac_transaction_timewait(t, t->reliable ? 1 : TIMER_D);
	}

	return r;
}

static int sip_uac_transaction_invite_accepted(struct sip_uac_transaction_t* t, const struct sip_message_t* reply)
{
    if (!sip_sv_valid(&reply->to.tag) || !sip_sv_valid(&reply->from.tag))
        return -EINVAL;
    for (unsigned i = 0; i < t->accepted_count; ++i) {
        struct sip_uac_accepted_dialog* branch = &t->accepted[i];
        if (sip_sv_equal(&branch->dialog->remote.uri.tag, &reply->to.tag))
            return sip_uac_resend_ack(t, branch);
    }
    if (t->accepted_count == SIP_UAC_ACCEPTED_LIMIT)
        return -ENOSPC;

    struct sip_dialog_t* dialog = t->dialog;
    int result;
    if (dialog && sip_sv_equal(&dialog->remote.uri.tag, &reply->to.tag)) {
        sip_dialog_addref(dialog);
        result = sip_dialog_confirm_uac(dialog, reply);
    } else {
        dialog = sip_dialog_create();
        if (!dialog) return -ENOMEM;
        result = sip_dialog_init_uac(dialog, reply);
    }
    if (result == 0) result = sip_dialog_set_local_target(dialog, t->req);
    if (result != 0 || !sip_sv_valid(&dialog->remote.target.host)) {
        sip_dialog_release(dialog);
        return -EPROTO;
    }
    dialog->state = DIALOG_CONFIRMED;
    t->accepted[t->accepted_count++].dialog = dialog;

    if (t->status != SIP_UAC_TRANSACTION_ACCEPTED) {
        t->status = SIP_UAC_TRANSACTION_ACCEPTED;
        sip_uac_stop_timer(t->agent, t, &t->timerd);
        result = sip_uac_transaction_timewait(t, TIMER_M);
        if (result != 0) return result;
    }

    char text[256];
    vstr id;
    sip_dialog_id(&id, dialog, text, sizeof(text));
    return t->oninvite ? t->oninvite(t->param, reply, t, dialog, &id, reply->u.s.code) : 0;
}

int sip_uac_transaction_invite_input(struct sip_uac_transaction_t* t, const struct sip_message_t* reply)
{
    int code = reply->u.s.code;
    if (t->status == SIP_UAC_TRANSACTION_TERMINATED)
        return 0;
    sip_uac_stop_timer(t->agent, t, &t->timera);

    if (code >= 200 && code < 300)
        return sip_uac_transaction_invite_accepted(t, reply);
    if (t->status == SIP_UAC_TRANSACTION_ACCEPTED)
        return 0;
    if (t->status == SIP_UAC_TRANSACTION_COMPLETED)
        return code >= 300 ? sip_uac_transaction_invite_completed(t, reply, 1) : 0;
    if (code < 200) {
        t->status = SIP_UAC_TRANSACTION_PROCEEDING;
        return sip_uac_transaction_invite_proceeding(t, reply);
    }

    t->status = SIP_UAC_TRANSACTION_COMPLETED;
    return sip_uac_transaction_invite_completed(t, reply, 0);
}
