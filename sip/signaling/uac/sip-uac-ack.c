#include "sip-uac.h"
#include "sip-dialog.h"
#include "sip-message.h"
#include "sip-uac-transaction.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

static int sip_uac_ack_credentials(struct sip_message_t* ack, const struct sip_message_t* request)
{
    const char* const names[] = {"Authorization", "Proxy-Authorization"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const vstr* value = sip_message_get_header_by_name(request, names[i]);
        if (value) {
            char text[UDP_PACKET_SIZE];
            if (value->len >= sizeof(text)) return -E2BIG;
            sip_sv_copy(value, text, sizeof(text));
            int result = sip_message_add_header(ack, names[i], text);
            if (result) return result;
        }
    }
    return 0;
}

int sip_uac_ack_3456xx(struct sip_uac_transaction_t* t, const struct sip_message_t* reply)
{
    struct sip_message_t* ack = sip_message_create(SIP_MESSAGE_REQUEST);
    if (!ack) return -ENOMEM;
    int result = sip_message_initack(ack, t->req);
    char to[UDP_PACKET_SIZE];
    int size = sip_contact_write(&reply->to, to, to + sizeof(to));
    if (result == 0 && (size < 0 || (size_t)size >= sizeof(to) - 1))
        result = -E2BIG;
    if (result == 0)
        result = sip_message_add_header(ack, "To", to);
    ack->u.c.method = vstr_from_cstr(SIP_METHOD_ACK);
    ack->cseq.method = ack->u.c.method;
    if (result == 0)
        result = sip_uac_ack_credentials(ack, t->req);
    if (result == 0) {
        t->size = sip_message_write(ack, t->data, sizeof(t->data));
        result = t->size <= 0 || (size_t)t->size >= sizeof(t->data)
            ? -E2BIG : sip_uac_select_hop(t, t->next_hop);
        if (result == 0)
            result = t->transport.send(t->transportptr, t->data, t->size);
    }
    sip_message_destroy(ack);
    return result;
}

int sip_uac_resend_ack(struct sip_uac_transaction_t* t, const struct sip_uac_accepted_dialog* branch)
{
    if (!branch->ack) return 0;
    int result = sip_uac_select_hop(t, branch->next_hop);
    if (result == 0)
        result = t->transport.send(t->transportptr, branch->ack, (size_t)branch->ack_size);
    return result;
}

static int sip_uac_write_ack(struct sip_uac_transaction_t* invite, struct sip_uac_accepted_dialog* branch,
                             const void* data, int bytes, const char* content_type)
{
    if (branch->ack)
        return sip_uac_resend_ack(invite, branch);
    if (!sip_sv_valid(&branch->dialog->remote.target.host))
        return -EPROTO;

    struct sip_message_t* ack = sip_message_create(SIP_MESSAGE_REQUEST);
    if (!ack) return -ENOMEM;
    int result = sip_message_init2(ack, SIP_METHOD_ACK, branch->dialog);
    ack->cseq.id = invite->req->cseq.id;
    if (result == 0 && content_type && *content_type)
        result = sip_message_add_header(ack, "Content-Type", content_type);
    if (result == 0)
        result = sip_uac_ack_credentials(ack, invite->req);

    char via[1024], contact[1024];
    if (result == 0)
        result = sip_uac_next_hop(ack, branch->next_hop);
    if (result == 0)
        result = sip_uac_transaction_via(invite, branch->next_hop, via, sizeof(via), contact, sizeof(contact));
    if (result == 0)
        result = sip_message_add_header(ack, "Via", via);

    if (result == 0) {
        uint8_t packet[UDP_PACKET_SIZE];
        ack->payload = data;
        ack->size = bytes;
        int size = sip_message_write(ack, packet, sizeof(packet));
        if (size <= 0 || (size_t)size >= sizeof(packet)) {
            result = -E2BIG;
        } else {
            branch->ack = malloc((size_t)size);
            if (branch->ack) {
                memcpy(branch->ack, packet, (size_t)size);
                branch->ack_size = size;
                result = invite->transport.send(invite->transportptr, branch->ack, (size_t)size);
            } else {
                result = -ENOMEM;
            }
        }
    }
    sip_message_destroy(ack);
    return result;
}

static int sip_uac_ack_branch(struct sip_uac_transaction_t* invite, const struct sip_dialog_t* dialog,
                              const void* data, int bytes, const char* content_type)
{
    if (!invite || bytes < 0 || (bytes && !data)) return -EINVAL;
    int result = -EINVAL;
    sync_mutex_lock(&invite->locker);
    if (invite->status == SIP_UAC_TRANSACTION_ACCEPTED) {
        for (unsigned i = 0; i < invite->accepted_count; ++i) {
            struct sip_uac_accepted_dialog* branch = &invite->accepted[i];
            if (!dialog || branch->dialog == dialog) {
                result = sip_uac_write_ack(invite, branch, data, bytes, content_type);
                break;
            }
        }
    }
    sync_mutex_unlock(&invite->locker);
    return result;
}

int sip_uac_ack(struct sip_uac_transaction_t* invite, const void* data, int bytes, const char* content_type)
{
    return sip_uac_ack_branch(invite, NULL, data, bytes, content_type);
}

int sip_uac_ack_dialog(struct sip_uac_transaction_t* invite, const struct sip_dialog_t* dialog,
                       const void* data, int bytes, const char* content_type)
{
    return dialog ? sip_uac_ack_branch(invite, dialog, data, bytes, content_type) : -EINVAL;
}

struct sip_uac_transaction_t* sip_uac_prack(struct sip_agent_t* sip, const struct sip_message_t* req100rel, struct sip_dialog_t* dialog, sip_uac_onreply onreply, void* param)
{
	char rack[64];
	struct sip_message_t* req;
	struct sip_uac_transaction_t* t;
	const vstr* rseq;

	rseq = sip_message_get_header_by_name(req100rel, SIP_HEADER_RSEQ);
	if (!sip || !dialog || !rseq)
		return NULL;

	snprintf(rack, sizeof(rack), "%u %u %.*s", (unsigned int)req100rel->rseq, (unsigned int)req100rel->cseq.id, (int)req100rel->cseq.method.len, req100rel->cseq.method.data);

	++dialog->local.id;
	req = sip_message_create(SIP_MESSAGE_REQUEST);
	if (0 != sip_message_init2(req, SIP_METHOD_INFO, dialog)
		|| 0 != sip_message_add_header(req, SIP_HEADER_RACK, rack))
	{
		--dialog->local.id;
		sip_message_destroy(req);
		return NULL;
	}

	t = sip_uac_transaction_create(sip, req);
	t->onreply = onreply;
	t->param = param;
	return t;
}

struct sip_uac_transaction_t* sip_uac_update(struct sip_agent_t* sip, struct sip_dialog_t* dialog, sip_uac_onreply onreply, void* param)
{
	struct sip_message_t* req;
	struct sip_uac_transaction_t* t;

	if (!sip || !dialog)
		return NULL;

	++dialog->local.id;
	req = sip_message_create(SIP_MESSAGE_REQUEST);
	if (0 != sip_message_init2(req, SIP_METHOD_UPDATE, dialog))
	{
		--dialog->local.id;
		sip_message_destroy(req);
		return NULL;
	}

	t = sip_uac_transaction_create(sip, req);
	t->onreply = onreply;
	t->param = param;
	return t;
}
