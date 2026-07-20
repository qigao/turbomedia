// RFC3261 19.1.1 SIP and SIPS URI Components (p148)
// RFC3261 19.1.2 Character Escaping Requirements (p152)
// sip:user:password@host:port;uri-parameters?headers
// sip:alice@atlanta.com
// sip:alice:secretword@atlanta.com;transport=tcp
// sips:alice@atlanta.com?subject=project%20x&priority=urgent
// sip:+1-212-555-1212:1234@gateway.com;user=phone
// sips:1212@gateway.com
// sip:alice@192.0.2.4
// sip:atlanta.com;method=REGISTER?to=alice%40atlanta.com
// sip:alice;day=tuesday@atlanta.com

#include "sip-header.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void sip_uri_free(struct sip_uri_t* uri)
{
	sip_params_free(&uri->headers);
	sip_params_free(&uri->parameters);
}

int sip_header_uri(const char* s, const char* end, struct sip_uri_t* uri)
{
	int i;
	const char* p;
	const struct sip_param_t* param;
	tstr_v parameters;
	memset(uri, 0, sizeof(*uri));
	sip_params_init(&uri->headers);
	sip_params_init(&uri->parameters);

	p = (s && s < end) ? strchr(s, ':') : NULL;
	if (!p || p >= end)
		return -1;
	
	uri->scheme.data = s;
	uri->scheme.len = p - s;
	uri->host.data = p + 1;

	s = p + 1;
	p = strpbrk(s, ";?");
	if (!p || p >= end)
	{
		uri->host.len = end - s;
	}
	else
	{
		uri->host.len = p - s;
		
		if (p && p < end && ';' == *p)
		{
			parameters.data = p + 1;
			p = strpbrk(p + 1, "?");
			if(p && p < end)
				parameters.len = p - parameters.data;
			else
				parameters.len = end - parameters.data;
			sip_header_params(';', parameters.data, parameters.data + parameters.len, &uri->parameters);
		}

		if (p && p < end && '?' == *p)
		{
			sip_header_params('&', p + 1, end, &uri->headers);
		}
	}

	for(i = 0; i < sip_params_count(&uri->parameters); i++)
	{
		param = sip_params_get(&uri->parameters, i);
		if (0 == sip_sv_compare_cstr_ci(&param->name, "transport"))
		{
			uri->transport.data = param->value.data;
			uri->transport.len = param->value.len;
		}
		else if (0 == sip_sv_compare_cstr_ci(&param->name, "method"))
		{
			uri->method.data = param->value.data;
			uri->method.len = param->value.len;
		}
		else if (0 == sip_sv_compare_cstr_ci(&param->name, "maddr"))
		{
			uri->maddr.data = param->value.data;
			uri->maddr.len = param->value.len;
		}
		else if (0 == sip_sv_compare_cstr_ci(&param->name, "user"))
		{
			uri->user.data = param->value.data;
			uri->user.len = param->value.len;
		}
		else if (0 == sip_sv_compare_cstr_ci(&param->name, "ttl"))
		{
			uri->ttl = (int)sip_sv_to_long(&param->value, NULL, 10);
		}
		else if (0 == sip_sv_compare_cstr_ci(&param->name, "lr"))
		{
			uri->lr = 1;
		}
		else if (0 == sip_sv_compare_cstr_ci(&param->name, "rport"))
		{
			uri->rport = sip_sv_valid(&param->value) ? (int)sip_sv_to_long(&param->value, NULL, 10) : -1;
		}
	}

	return 0;
}

// sip:user:password@host:port;uri-parameters?headers
int sip_uri_write(const struct sip_uri_t* uri, char* data, const char* end)
{
	char* p;
	if (!sip_sv_valid(&uri->scheme) || !sip_sv_valid(&uri->host))
		return -1; // error

	p = data;
	if(p < end)
		p += snprintf(p, end - p, "%.*s:%.*s", (int)uri->scheme.len, uri->scheme.data, (int)uri->host.len, uri->host.data);

	if (sip_params_count(&uri->parameters) > 0)
	{
		*p++ = ';';
		p += sip_params_write(&uri->parameters, p, end, ';');
	}

	if (sip_params_count(&uri->headers) > 0)
	{
		*p++ = '?';
		p += sip_params_write(&uri->headers, p, end, '&');
	}

	if (p < end) *p = '\0';
	return (int)(p - data);
}

// 19.1.1 SIP and SIPS URI Components (p152)
/*
												 dialog
								   reg./redir.  Contact/
		  default Req.-URI	To From	 Contact	R-R/Route external
user		--		o		o	o		o			o		o
password	--		o		o	o		o			o		o
host		--		m		m	m		m			m		m
port		(1)		o		-	-		o			o		o
user-param	ip		o		o	o		o			o		o
method		INVITE	-		-	-		-			-		o
maddr-param --		o		-	-		o			o		o
ttl-param	1		o		-	-		o			-		o
trans-param (2)		o		-	-		o			o		o
lr-param	--		o		-	-		-			o		o
other-param --		o		o	o		o			o		o
headers		--		-		-	-		o			-		o
*/
int sip_request_uri_write(const struct sip_uri_t* uri, char* data, const char* end)
{
	int i, r;
	struct sip_uri_t v;
	struct sip_param_t* param;

	memcpy(&v, uri, sizeof(v));
	sip_params_init(&v.parameters);
	sip_params_init(&v.headers);

	for (i = 0; i < sip_params_count(&uri->parameters); i++)
	{
		param = sip_params_get(&uri->parameters, i);
		if (0 == sip_sv_compare_cstr_ci(&param->name, "method"))
			continue;
		sip_params_push(&v.parameters, param);
	}
	r = sip_uri_write(&v, data, end);

	sip_uri_free(&v);
	return r;
}

// 19.1.4 URI Comparison (p153)
int sip_uri_equal(const struct sip_uri_t* l, const struct sip_uri_t* r)
{
	static const char* uriparameters[] = { "user", "ttl", "method", "transport" };

	int i;
	const char* p1;
	const char* p2;
	const struct sip_param_t* param1;
	const struct sip_param_t* param2;

	if (!sip_sv_equal(&l->scheme, &r->scheme))
		return 0;

	p1 = sip_sv_find_char(&l->host, '@');
	p2 = sip_sv_find_char(&r->host, '@');
	p1 = p1 ? p1 : l->host.data;
	p2 = p2 ? p2 : r->host.data;

	// Comparison of the userinfo of SIP and SIPS URIs is case-sensitive
	if (p1 - l->host.data != p2 - r->host.data || 0 != strncmp(l->host.data, r->host.data, p1 - l->host.data))
		return 0;

	// Comparison of all other components of the URI is case-insensitive
	// A URI omitting any component with a default value will not match a 
	// URI explicitly containing that component with its default value.
	if (l->host.len != r->host.len ||
		0 != sip_mem_casecmp(p1, p2, (size_t)(l->host.data + l->host.len - p1)))
		return 0;

	// TODO:
	// Characters other than those in the "reserved" set (see RFC 2396 [5]) 
	// are equivalent to their ""%" HEX HEX" encoding.

	// Any uri-parameter appearing in both URIs must match.
	for (i = 0; i < sip_params_count(&l->parameters); i++)
	{
		param1 = sip_params_get(&l->parameters, i);
		if(0 == sip_sv_compare_cstr(&param1->name, "maddr"))
			continue;
		param2 = sip_params_find(&r->parameters, param1->name.data, (int)param1->name.len);
		if (param2 && !sip_sv_equal(&param1->value, &param2->value))
			return 0;
	}

	// A user, ttl, or method uri-parameter appearing in only one URI 
	// never matches, even if it contains the default value
	for (i = 0; i < sizeof(uriparameters) / sizeof(uriparameters[0]); i++)
	{
		param1 = sip_params_find(&l->parameters, uriparameters[i], (int)strlen(uriparameters[i]));
		param2 = sip_params_find(&r->parameters, uriparameters[i], (int)strlen(uriparameters[i]));
		if (param2 && !param1) // eq(param1, param2) already checked
			return 0;
	}

	// A URI that includes an maddr parameter will not match a URI
	// that contains no maddr parameter
	param1 = sip_params_find(&l->parameters, "maddr", 5);
	param2 = sip_params_find(&r->parameters, "maddr", 5);
	if ((param1 && !param2) || (!param1 && param2) || (param1 && param2 && 0==sip_sv_equal(&param1->value, &param2->value)))
		return 0;

	// URI header components are never ignored.
	if (sip_params_count(&l->headers) != sip_params_count(&r->headers))
		return 0;

	// The matching rules are defined for each header field in Section 20.
	for (i = 0; i < sip_params_count(&l->headers); i++)
	{
		param1 = sip_params_get(&l->headers, i);
		param2 = sip_params_find(&r->headers, param1->name.data, (int)param1->name.len);
		if (!param2 || !sip_sv_equal(&param1->value, &param2->value))
			return 0;
	}

	return 1;
}

int sip_uri_username(const struct sip_uri_t* uri, tstr_v* user)
{
	const char *p1, *p2;
	p1 = sip_sv_find_char(&uri->host, '@');
	if (!p1) return -1;
	p2 = sip_sv_find_char(&uri->host, ':');
	if (!p2 || p2 > p1)
		p2 = p1;

	user->data = uri->host.data;
	user->len = p2 - uri->host.data;
	return 0;
}


#if defined(DEBUG) || defined(_DEBUG)
void sip_uri_parse_test(void)
{
	char p[1024];
	const char* s;
	struct sip_uri_t uri;
	tstr_v usr;

	s = "sip:user:password@host:port;uri-parameters?headers";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "user:password@host:port") && 1 == sip_params_count(&uri.parameters) && 1 == sip_params_count(&uri.headers));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->name, "uri-parameters") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->value, ""));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.headers, 0)->name, "headers") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.headers, 0)->value, ""));
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "user"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sip:alice@atlanta.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "alice@atlanta.com") && 0 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "alice"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sips:alice@atlanta.com?subject=project%20x&priority=urgent";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sips") && 0 == sip_sv_compare_cstr(&uri.host, "alice@atlanta.com") && 0 == sip_params_count(&uri.parameters) && 2 == sip_params_count(&uri.headers));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.headers, 0)->name, "subject") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.headers, 0)->value, "project%20x"));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.headers, 1)->name, "priority") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.headers, 1)->value, "urgent"));
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "alice"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sip:alice:secretword@atlanta.com;transport=tcp";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "alice:secretword@atlanta.com") && 1 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->name, "transport") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->value, "tcp"));
	assert(0 == sip_sv_compare_cstr(&uri.transport, "tcp"));
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "alice"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sip:+1-212-555-1212:1234@gateway.com;user=phone";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "+1-212-555-1212:1234@gateway.com") && 1 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->name, "user") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->value, "phone"));
	assert(0 == sip_sv_compare_cstr(&uri.user, "phone"));
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "+1-212-555-1212"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sip:alice;day=tuesday@atlanta.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "alice") && 1 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->name, "day") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->value, "tuesday@atlanta.com"));
	assert(0 != sip_uri_username(&uri, &usr));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sip:p2.domain.com;lr";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "p2.domain.com") && 1 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->name, "lr") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->value, ""));
	assert(1 == uri.lr);
	assert(0 != sip_uri_username(&uri, &usr));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sip:alice@atlanta.com;maddr=239.255.255.1;ttl=15";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "alice@atlanta.com") && 2 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->name, "maddr") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 0)->value, "239.255.255.1"));
	assert(0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 1)->name, "ttl") && 0 == sip_sv_compare_cstr(&sip_params_get(&uri.parameters, 1)->value, "15"));
	assert(0 == sip_sv_compare_cstr(&uri.maddr, "239.255.255.1") && 15 == uri.ttl);
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "alice"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	// ipv6
	s = "sip:user@[::ffff:192.0.2.10]:19823";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "user@[::ffff:192.0.2.10]:19823") && 0 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "user"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);

	s = "sip:user@fe80::204:75ff:fe4d:19d9";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri));
	assert(0 == sip_sv_compare_cstr(&uri.scheme, "sip") && 0 == sip_sv_compare_cstr(&uri.host, "user@fe80::204:75ff:fe4d:19d9") && 0 == sip_params_count(&uri.parameters) && 0 == sip_params_count(&uri.headers));
	assert(0 == sip_uri_username(&uri, &usr) && 0 == sip_sv_compare_cstr(&usr, "user"));
	assert(sip_uri_write(&uri, p, p + sizeof(p)) < sizeof(p) && 0 == strcmp(s, p));
	sip_uri_free(&uri);
}

void sip_uri_equal_test(void)
{
	const char* s;
	struct sip_uri_t uri1;
	struct sip_uri_t uri2;

	// TODO: url decode
	//s = "sip:%61lice@atlanta.com;transport=TCP";
	//assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	//s = "sip:alice@AtLanTa.CoM;Transport=tcp";
	//assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	//assert(1 == sip_uri_equal(&uri1, &uri2));
	//sip_uri_free(&uri1);
	//sip_uri_free(&uri2);

	s = "sip:carol@chicago.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:carol@chicago.com;newparam=5";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(1 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	s = "sip:carol@chicago.com;security=on";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:carol@chicago.com;newparam=5";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(1 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	s = "sip:biloxi.com;transport=tcp;method=REGISTER?to=sip:bob%40biloxi.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:biloxi.com;method=REGISTER;transport=tcp?to=sip:bob%40biloxi.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(1 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	s = "sip:alice@atlanta.com?subject=project%20x&priority=urgent";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:alice@atlanta.com?priority=urgent&subject=project%20x";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(1 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	// different usernames
	s = "SIP:ALICE@AtLanTa.CoM;Transport=udp";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:alice@AtLanTa.CoM;Transport=UDP";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(0 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	// can resolve to different ports
	s = "sip:bob@biloxi.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:bob@biloxi.com:5060";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(0 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	// can resolve to different transports
	s = "sip:bob@biloxi.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:bob@biloxi.com;transport=udp";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(0 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	// different header component
	s = "sip:carol@chicago.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:carol@chicago.com?Subject=next%20meeting";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(0 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);

	// An IP address that is the result of a DNS lookup of a host name
	// does not match that host name.
	s = "sip:bob@phone21.boxesbybob.com";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri1));
	s = "sip:bob@192.0.2.4";
	assert(0 == sip_header_uri(s, s + strlen(s), &uri2));
	assert(0 == sip_uri_equal(&uri1, &uri2));
	sip_uri_free(&uri1);
	sip_uri_free(&uri2);
}
#endif
