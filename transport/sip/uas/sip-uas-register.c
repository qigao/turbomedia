#include "sip-uas-transaction.h"
#include "fmt.h"
#include "salts_error.h"
#include "salts_str.h"

/*
REGISTER sip:registrar.biloxi.com SIP/2.0
Via: SIP/2.0/UDP bobspc.biloxi.com:5060;branch=z9hG4bKnashds7
Max-Forwards: 70
To: Bob <sip:bob@biloxi.com>
From: Bob <sip:bob@biloxi.com>;tag=456248
Call-ID: 843817637684230@998sdasdh09
CSeq: 1826 REGISTER
Contact: <sip:bob@192.0.2.4>
Expires: 7200
Content-Length: 0

SIP/2.0 200 OK
Via: SIP/2.0/UDP bobspc.biloxi.com:5060;branch=z9hG4bKnashds7;received=192.0.2.4
To: Bob <sip:bob@biloxi.com>;tag=2493k59kd
From: Bob <sip:bob@biloxi.com>;tag=456248
Call-ID: 843817637684230@998sdasdh09
CSeq: 1826 REGISTER
Contact: <sip:bob@192.0.2.4>
Expires: 7200
Content-Length: 0
*/

struct sip_register_endpoint {
	vstr userinfo;
	vstr host;
	int port;
};

static int sip_register_endpoint_parse(vstr value, struct sip_register_endpoint *endpoint)
{
	size_t separator;
	vstr authority;
	vstr port;
	long parsed_port;

	if (!endpoint || !value.data || value.len == 0U)
		return SALTS_EINVAL;

	memset(endpoint, 0, sizeof(*endpoint));
	authority = value;
	separator = vstr_rfind_char(authority, '@');
	if (separator != VSTR_NPOS)
	{
		endpoint->userinfo = vstr_sub(authority, 0U, separator);
		authority = vstr_sub(authority, separator + 1U, SIZE_MAX);
	}
	if (authority.len == 0U)
		return SALTS_EINVAL;

	if (authority.data[0] == '[')
	{
		separator = vstr_find_char(authority, ']');
		if (separator == VSTR_NPOS || separator == 1U)
			return SALTS_EINVAL;
		endpoint->host = vstr_sub(authority, 1U, separator - 1U);
		if (separator + 1U < authority.len)
		{
			if (authority.data[separator + 1U] != ':')
				return SALTS_EINVAL;
			port = vstr_sub(authority, separator + 2U, SIZE_MAX);
		}
		else
			port = vstr_from_buf(NULL, 0U);
	}
	else
	{
		separator = vstr_rfind_char(authority, ':');
		if (separator != VSTR_NPOS &&
			vstr_find_char(authority, ':') == separator)
		{
			endpoint->host = vstr_sub(authority, 0U, separator);
			port = vstr_sub(authority, separator + 1U, SIZE_MAX);
		}
		else
		{
			endpoint->host = authority;
			port = vstr_from_buf(NULL, 0U);
		}
	}

	if (endpoint->host.len == 0U)
		return SALTS_EINVAL;
	if (port.len == 0U)
	{
		endpoint->port = SIP_PORT;
		return SALTS_OK;
	}

	parsed_port = sip_sv_to_long(&port, NULL, 10);
	if (parsed_port < 1L || parsed_port > 65535L)
		return SALTS_EINVAL;
	endpoint->port = (int)parsed_port;
	return SALTS_OK;
}

// 10.3 Processing REGISTER Requests(p63)
int sip_uas_onregister(struct sip_uas_transaction_t* t, const struct sip_message_t* req, void* param)
{
	int r, expires;
	char *from_user;
	tstr location;
	struct sip_register_endpoint uri;
	struct sip_register_endpoint from;
	const vstr* header;
	const struct sip_contact_t* contact;
	int have_contact;

	// If contact.expire is not provided, default equal to 60
	header = sip_message_get_header_by_name(req, "Expires");
	expires = header ? (unsigned int)sip_sv_to_long(header, NULL, 10) : 60;

	// 1. Request-URI

	// Request-URI: The "userinfo" and "@" components of the SIP URI MUST NOT be present
	if (sip_register_endpoint_parse(req->u.c.uri.host, &uri) != SALTS_OK ||
		uri.userinfo.len != 0U)
	{
		return sip_uas_transaction_noninvite_reply(t, 400/*Invalid Request*/, NULL, 0, param);
	}

	// TODO: check domain and proxy to another host
	//if (0 != strcasecmp(uri->host, t->uas->domain))
	//{
	//}

	// 2. the registrar MUST process the Require header field values
	header = sip_message_get_header_by_name(req, "require");
	if (!header)
	{
		// TODO: check require
	}

	// 3. authentication
	// 4. authorized modify registrations(403 Forbidden)

	// 5. To domain check (404 Not Found)
	if (sip_register_endpoint_parse(req->from.uri.host, &from) != SALTS_OK ||
		from.userinfo.len == 0U)
	{
		// all URI parameters MUST be removed (including the user-param), and
		// any escaped characters MUST be converted to their unescaped form.
		return sip_uas_transaction_noninvite_reply(t, 404/*Not Found*/, NULL, 0, param);
	}

	// 6. Contact
	//    * - multi-contacts, expires != 0 (400 Invalid Request)
	//    call-id
	//    cseq
	if (sip_contacts_match_any(&req->contacts) && (1 != sip_contacts_count(&req->contacts) || 0 < expires) )
	{
		return sip_uas_transaction_noninvite_reply(t, 400/*Invalid Request*/, NULL, 0, param);
	}

	// TODO:
	// Typically, a UA that uses the REGISTER method to bind its address-of-record 
	// to a specific contact address will see requests whose Request-URI equals 
	// that contact address

	// All registrations from a UAC SHOULD use the same Call-ID header 
	// field value for registrations sent to a particular registrar.
	//req->callid;

	// A UA MUST increment the CSeq value by one for each
	// REGISTER request with the same Call-ID.
	assert(0 == sip_sv_compare_cstr_ci(&req->cseq.method, "REGISTER"));
	//req->cseq.id;

	// zero or more values containing address bindings
	contact = sip_contacts_get(&req->contacts, 0);
	have_contact = contact &&
		sip_register_endpoint_parse(contact->uri.host, &uri) == SALTS_OK;
	if (contact && !have_contact)
		return sip_uas_transaction_noninvite_reply(t, 400/*Invalid Request*/, NULL, 0, param);
	if (contact && contact->expires > 0)
	{
		// https://datatracker.ietf.org/doc/html/rfc3261#section-10.3
		// 7. The registrar now processes each contact address in the Contact
        // header field in turn.  For each address, it determines the
		// expiration interval as follows:
		// -  If the field value has an "expires" parameter, that value
		//    MUST be taken as the requested expiration.
		// -  If there is no such parameter, but the request has an
		//    Expires header field, that value MUST be taken as the
		//    requested expiration.
		// -  If there is neither, a locally-configured default value MUST
		//    be taken as the requested expiration.
		expires = (int)contact->expires;
	}

	// The Record-Route header field has no meaning in REGISTER 
	// requests or responses, and MUST be ignored if present.

	from_user = vstr_to_cstr(from.userinfo);
	if (!from_user)
		return sip_uas_transaction_noninvite_reply(t, 500/*Server Internal Error*/, NULL, 0, param);
	location = have_contact ? tstr_format("{}:{}", uri.host, uri.port) : NULL;
	if (have_contact && !location)
	{
		free(from_user);
		return sip_uas_transaction_noninvite_reply(t, 500/*Server Internal Error*/, NULL, 0, param);
	}
	r = t->handler->onregister
		? t->handler->onregister(param, req, t, from_user, location, expires)
		: 0;
	
	//if (423/*Interval Too Brief*/ == r)
	//{
	//	sip_uas_add_header_int(t, "Min-Expires", t->uas->min_expires_seconds);
	//}

	//// The Record-Route header field has no meaning in REGISTER requests or responses, 
	//// and MUST be ignored if present.
	//return sip_uas_transaction_noninvite_reply(t, r, NULL, 0);
	tstr_free(location);
	free(from_user);
	return r;
}
