#include "sip-uac.h"
#include "sip-dialog.h"
#include "sip-message.h"
#include "sip-uac-transaction.h"
#include <limits.h>

struct sip_uac_transaction_t* sip_uac_invite(struct sip_agent_t* sip, const char* name, const char* to, sip_uac_oninvite oninvite, void* param)
{
	struct sip_message_t* req;
	struct sip_uac_transaction_t* t;

	req = sip_message_create(SIP_MESSAGE_REQUEST);
	if (!req)
		return NULL;
	if (0 != sip_message_init(req, SIP_METHOD_INVITE, to, name, to))
	{
		sip_message_destroy(req);
		return NULL;
	}

	t = sip_uac_transaction_create(sip, req);
	if (!t)
	{
		sip_message_destroy(req);
		return NULL;
	}
	t->oninvite = oninvite;
	t->param = param;
	return t;
}

struct sip_uac_transaction_t* sip_uac_reinvite(struct sip_agent_t* sip, struct sip_dialog_t* dialog, sip_uac_oninvite oninvite, void* param)
{
	struct sip_message_t* req;
	struct sip_uac_transaction_t* t;

	if (!dialog || dialog->local.id >= INT_MAX)
		return NULL;
	req = sip_message_create(SIP_MESSAGE_REQUEST);
	if (!req)
		return NULL;
	++dialog->local.id;
	if (0 != sip_message_init2(req, SIP_METHOD_INVITE, dialog))
	{
		--dialog->local.id;
		sip_message_destroy(req);
		return NULL;
	}

	t = sip_uac_transaction_create(sip, req);
	if (!t)
	{
		--dialog->local.id;
		sip_message_destroy(req);
		return NULL;
	}
	sip_dialog_addref(dialog);
	t->dialog = dialog;
	t->oninvite = oninvite;
	t->param = param;
	return t;
}
