#include "sip-header.h"
#include <stdio.h>

/*
CSeq = "CSeq" HCOLON 1*DIGIT LWS Method
*/

int sip_header_cseq(const char* s, const char* end, struct sip_cseq_t* cseq)
{
	char* p;

	p = (char*)s;
	cseq->id = (s && s < end) ? (uint32_t)strtoul(s, &p, 10) : 0;

	cseq->method.data = p;
	cseq->method.len = end - p;
	sip_sv_trim(&cseq->method, " \t\r\n");

	return 0;
}

int sip_cseq_write(const struct sip_cseq_t* cseq, char* data, const char* end)
{
	if (!sip_sv_valid(&cseq->method))
		return -1;
	return snprintf(data, end-data, "%u %.*s", (unsigned int)cseq->id, (int)cseq->method.len, cseq->method.data);
}

#if defined(DEBUG) || defined(_DEBUG)
void sip_header_cseq_test(void)
{
	const char* s;
	struct sip_cseq_t cseq;

	s = "314159 INVITE";
	assert(0 == sip_header_cseq(s, s + strlen(s), &cseq));
	assert(0 == sip_sv_compare_cstr(&cseq.method, "INVITE") && cseq.id == 314159);
}
#endif
