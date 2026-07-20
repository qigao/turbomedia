#include "sip-dialog.h"
#include "sip-message.h"
#include "sip-internal.h"
#include <stdlib.h>

#define N 2048

// 12.1.2 UAC Behavior
// A UAC MUST be prepared to receive a response without a tag in the To
// field, in which case the tag is considered to have a value of null.
// This is to maintain backwards compatibility with RFC 2543, which
// did not mandate To tags.
static const tstr_v sc_null = { "", 0 };

struct sip_dialog_t* sip_dialog_create(void)
{
    struct sip_dialog_t* dialog;
    
    dialog = (struct sip_dialog_t*)calloc(1, sizeof(*dialog)+ N);
    if (dialog)
    {
        dialog->ref = 1;
        dialog->state = DIALOG_ERALY;
        dialog->ptr = (char*)(dialog + 1);
		sip_atomic_increment(&s_gc.dialog);
    }
    return dialog;
}

int sip_dialog_init_uac(struct sip_dialog_t* dialog, const struct sip_message_t* msg)
{
	int i;
	char *end;
	struct sip_uri_t uri;
	const struct sip_contact_t* contact;

    assert(SIP_MESSAGE_REPLY == msg->mode);
	assert(sip_sv_valid(&msg->from.tag) && sip_sv_valid(&msg->to.tag));
	end = dialog->ptr + N;

	dialog->ptr = sip_string_view_clone(dialog->ptr, end, &dialog->callid, msg->callid.data, msg->callid.len);
	dialog->ptr = sip_contact_clone(dialog->ptr, end, &dialog->local.uri, &msg->from);
	dialog->ptr = sip_contact_clone(dialog->ptr, end, &dialog->remote.uri, &msg->to);
	dialog->local.id = msg->cseq.id;
	if (0 != sip_random_u31(&dialog->local.rseq) ||
		0 != sip_random_u31(&dialog->remote.id) ||
		0 != sip_random_u31(&dialog->remote.rseq))
		return -1;

	//assert(1 == sip_contacts_count(&msg->contacts));
	contact = sip_contacts_get(&msg->contacts, 0);
	if (contact && sip_sv_valid(&contact->uri.host))
		dialog->ptr = sip_uri_clone(dialog->ptr, end, &dialog->remote.target, &contact->uri);

	// 12.1.2 UAC Behavior (p71)
	// The route set MUST be set to the list of URIs in the Record-Route
	// header field from the response, taken in reverse order and preserving
	// all URI parameters.
	sip_uris_init(&dialog->routers);
	for (i = sip_uris_count(&msg->record_routers); i > 0; i--)
	{
		dialog->ptr = sip_uri_clone(dialog->ptr, end, &uri, sip_uris_get(&msg->record_routers, i-1));
		sip_uris_push(&dialog->routers, &uri);
	}

	dialog->secure = sip_sv_starts_with(&dialog->remote.target.host, "sips");
	return 0;
}

int sip_dialog_init_uas(struct sip_dialog_t* dialog, const struct sip_message_t* msg)
{
    int i;
    char *end;
    struct sip_uri_t uri;
    const struct sip_contact_t* contact;
    
    assert(SIP_MESSAGE_REQUEST == msg->mode);
    assert(sip_sv_valid(&msg->from.tag));
    end = dialog->ptr + N;

	dialog->ptr = sip_string_view_clone(dialog->ptr, end, &dialog->callid, msg->callid.data, msg->callid.len);
    dialog->ptr = sip_contact_clone(dialog->ptr, end, &dialog->local.uri, &msg->to);
    dialog->ptr = sip_contact_clone(dialog->ptr, end, &dialog->remote.uri, &msg->from);
    if (0 != sip_random_u31(&dialog->local.id) ||
		0 != sip_random_u31(&dialog->local.rseq))
		return -1;
    dialog->remote.id = msg->cseq.id;
	if (0 == msg->rseq) {
		if (0 != sip_random_u31(&dialog->remote.rseq))
			return -1;
	} else {
		dialog->remote.rseq = msg->rseq;
	}
    
    //assert(1 == sip_contacts_count(&msg->contacts));
    contact = sip_contacts_get(&msg->contacts, 0);
    if (contact && sip_sv_valid(&contact->uri.host))
        dialog->ptr = sip_uri_clone(dialog->ptr, end, &dialog->remote.target, &contact->uri);
    
	// 12.1.1 UAS behavior (p70)
	// The route set MUST be set to the list of URIs in the Record-Route
	// header field from the request, taken in order and preserving all URI
	// parameters. If no Record-Route header field is present in the
	// request, the route set MUST be set to the empty set.
    sip_uris_init(&dialog->routers);
    for (i = 0; i < sip_uris_count(&msg->record_routers); i++)
    {
        dialog->ptr = sip_uri_clone(dialog->ptr, end, &uri, sip_uris_get(&msg->record_routers, i));
        sip_uris_push(&dialog->routers, &uri);
    }
    
    dialog->secure = sip_sv_starts_with(&dialog->remote.target.host, "sips");
    return 0;
}

int sip_dialog_release(struct sip_dialog_t* dialog)
{
	if (!dialog)
		return -1;

	assert(dialog->ref > 0);
	if (0 != sip_atomic_decrement(&dialog->ref))
		return 0;

	sip_uri_free(&dialog->local.target);
	sip_contact_free(&dialog->local.uri);
	sip_uri_free(&dialog->remote.target);
	sip_contact_free(&dialog->remote.uri);
	sip_uris_free(&dialog->routers);
	free(dialog);
	sip_atomic_decrement(&s_gc.dialog);
	return 0;
}

int sip_dialog_addref(struct sip_dialog_t* dialog)
{
	int r;
	r = sip_atomic_increment(&dialog->ref);
	assert(r > 1);
	return r;
}

int sip_dialog_setlocaltag(struct sip_dialog_t* dialog, const tstr_v* tag)
{
	const char* end;
	end = (char*)(dialog + 1) + N;
	dialog->ptr = sip_string_view_clone(dialog->ptr, end, &dialog->local.uri.tag, tag->data, tag->len);
	sip_params_add_or_update(&dialog->local.uri.params, "tag", 3, &dialog->local.uri.tag);
	return dialog->ptr < end ? 0 : -1;
}

int sip_dialog_set_local_target(struct sip_dialog_t* dialog, const struct sip_message_t* msg)
{
	const char* end;
	struct sip_contact_t* contact;
	end = (char*)(dialog + 1) + N;

	contact = sip_contacts_get(&msg->contacts, 0);
	if (contact && sip_sv_valid(&contact->uri.host) && !sip_uri_equal(&dialog->local.target, &contact->uri))
	{
		sip_uri_free(&dialog->local.target);
		dialog->ptr = sip_uri_clone(dialog->ptr, end, &dialog->local.target, &contact->uri);
	}
	return dialog->ptr < end ? 0 : -1;
}

int sip_dialog_target_refresh(struct sip_dialog_t* dialog, const struct sip_message_t* msg)
{
    const char* end;
    struct sip_contact_t* contact;
    end = (char*)(dialog + 1) + N;
    
    contact = sip_contacts_get(&msg->contacts, 0);
	if (contact && sip_sv_valid(&contact->uri.host) && !sip_uri_equal(&dialog->remote.target, &contact->uri))
	{
		sip_uri_free(&dialog->remote.target);
		dialog->ptr = sip_uri_clone(dialog->ptr, end, &dialog->remote.target, &contact->uri);
	}
    return dialog->ptr < end ? 0 : -1;
}

/// @return 1-match, 0-don't match
static int sip_dialog_match(const struct sip_dialog_t* dialog, const tstr_v* callid, const tstr_v* local, const tstr_v* remote)
{
	assert(dialog && local);
	if (!remote) remote = &sc_null;

	return sip_sv_equal(callid, &dialog->callid) && sip_sv_equal(local, &dialog->local.uri.tag) && sip_sv_equal(remote, &dialog->remote.uri.tag) ? 1 : 0;
}

int sip_dialog_id(tstr_v* id, const struct sip_dialog_t* dialog, char* ptr, int len)
{
	int r;
	r = dialog ? snprintf(ptr, len, "%.*s@%.*s@%.*s", (int)dialog->callid.len, dialog->callid.data, (int)dialog->local.uri.tag.len, dialog->local.uri.tag.data, (int)dialog->remote.uri.tag.len, dialog->remote.uri.tag.data) : 0;
	id->data = ptr;
	id->len = r > 0 && r < len ? r : 0;
	return r;
}

// @param[in] uas 1-local is uas
int sip_dialog_id_with_message(tstr_v *id, const struct sip_message_t* msg, char* ptr, int len, int uas)
{
	int r;
	assert(msg->mode == SIP_MESSAGE_REQUEST);
	if (uas)
		r = snprintf(ptr, len, "%.*s@%.*s@%.*s", (int)msg->callid.len, msg->callid.data, (int)msg->to.tag.len, msg->to.tag.data, (int)msg->from.tag.len, msg->from.tag.data);
	else
		r = snprintf(ptr, len, "%.*s@%.*s@%.*s", (int)msg->callid.len, msg->callid.data, (int)msg->from.tag.len, msg->from.tag.data, (int)msg->to.tag.len, msg->to.tag.data);
	
	id->data = ptr;
	id->len = r > 0 && r < len ? r : 0;
	return r;
}
