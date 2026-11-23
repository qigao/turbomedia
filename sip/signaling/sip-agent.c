#include "sip-agent.h"
#include "sip-internal.h"
#include "uac/sip-uac-transaction.h"
#include "uas/sip-uas-transaction.h"

struct sip_agent_t* sip_agent_create(struct sip_uas_handler_t* handler)
{
	struct sip_agent_t* sip;
	sip = (struct sip_agent_t*)calloc(1, sizeof(*sip));
	if (NULL == sip)
		return NULL;

	sip->ref = 1;
	sync_mutex_init(&sip->locker);
	if (vec_init_bytes(&sip->uac, sizeof(struct sip_uac_transaction_t *), CMETA_ALIGNOF(struct sip_uac_transaction_t *), SIZE_MAX / sizeof(struct sip_uac_transaction_t *)) != STL_OK ||
		vec_init_bytes(&sip->uas, sizeof(struct sip_uas_transaction_t *), CMETA_ALIGNOF(struct sip_uas_transaction_t *), SIZE_MAX / sizeof(struct sip_uas_transaction_t *)) != STL_OK)
	{
		vec_destroy(&sip->uac);
		vec_destroy(&sip->uas);
		sync_mutex_destroy(&sip->locker);
		free(sip);
		return NULL;
	}
	memcpy(&sip->handler, handler, sizeof(sip->handler));
	return sip;
}

int sip_agent_destroy(struct sip_agent_t* sip)
{
    int32_t ref;

	assert(sip->ref > 0);
    ref = sip_atomic_decrement(&sip->ref);
	if (0 != ref)
		return ref;

	assert(vec_empty(&sip->uac));
	assert(vec_empty(&sip->uas));
	
	vec_destroy(&sip->uac);
	vec_destroy(&sip->uas);
	sync_mutex_destroy(&sip->locker);
	free(sip);
	return 0;
}

int sip_agent_input(struct sip_agent_t* sip, struct sip_message_t* msg, void* param)
{
	return SIP_MESSAGE_REPLY == msg->mode ? sip_uac_input(sip, msg) : sip_uas_input(sip, msg, param);
}

int sip_agent_input_reply_datagram(struct sip_agent_t* sip, const void* data, size_t bytes)
{
	struct sip_message_t* msg;
	int result;
	if (!sip)
		return -EINVAL;
	msg = sip_message_parse_datagram(data, bytes);
	if (!msg)
		return -EINVAL;
	result = msg->mode == SIP_MESSAGE_REPLY && sip_sv_valid(&msg->from.tag)
		? sip_agent_input(sip, msg, NULL) : -EINVAL;
	sip_message_destroy(msg);
	return result;
}

int sip_agent_set_rport(struct sip_message_t* msg, const char* peer, int port)
{
	return sip_message_set_rport(msg, peer, port);
}

struct sip_agent_t* sip_agent_create_owner(struct sip_uas_handler_t* handler)
{
	struct sip_agent_t* sip = sip_agent_create(handler);
	if (sip)
		sip->owner_timers = 1;
	return sip;
}

void sip_agent_abort_transactions(struct sip_agent_t* sip)
{
	assert(sip->owner_timers);
	while (!vec_empty(&sip->uac))
	{
		struct sip_uac_transaction_t* transaction = *(struct sip_uac_transaction_t**)vec_at(&sip->uac, 0);
		sip_uac_transaction_addref(transaction);
		sip_uac_transaction_abort(transaction);
		sip_uac_transaction_release(transaction);
	}
	while (!vec_empty(&sip->uas))
	{
		struct sip_uas_transaction_t* transaction = *(struct sip_uas_transaction_t**)vec_at(&sip->uas, 0);
		sip_uas_transaction_addref(transaction);
		sip_uas_transaction_terminated(transaction);
		sip_uas_transaction_release(transaction);
	}
	assert(!sip->deadlines);
}

struct sip_gc_t s_gc;
void sip_gc_get(int32_t* uac, int32_t* uas, int32_t* dialog, int32_t* message, int32_t* subscribe)
{
	*uac = sip_atomic_load(&s_gc.uac);
	*uas = sip_atomic_load(&s_gc.uas);
	*dialog = sip_atomic_load(&s_gc.dialog);
	*message = sip_atomic_load(&s_gc.message);
	*subscribe = sip_atomic_load(&s_gc.subscribe);
}
