#include "platform.h"

#include <assert.h>
#include <stdint.h>
#include <time.h>

/// same as system_time except ms -> us
/// @return microseconds since the Epoch(1970-01-01 00:00:00 +0000 (UTC))
uint64_t rtpclock()
{
	turbo_timeval_t tv;

	if (turbo_gettimeofday(&tv, NULL) != 0)
		abort();

	return (uint64_t)tv.tv_sec * 1000000ULL + (uint32_t)tv.tv_usec;
}

/// us(microsecond) -> ntp
uint64_t clock2ntp(uint64_t clock)
{
	uint64_t ntp;

	// high 32 bits in seconds
	ntp = ((clock/1000000)+0x83AA7E80) << 32; // 1/1/1970 -> 1/1/1900

	// low 32 bits in picosecond
	// us * 2^32 / 10^6
	// 10^6 = 2^6 * 15625
	// => us * 2^26 / 15625
	ntp |= (uint32_t)(((clock % 1000000) << 26) / 15625);

	return ntp;
}

/// ntp -> us(microsecond)
uint64_t ntp2clock(uint64_t ntp)
{
	uint64_t clock;

	// high 32 bits in seconds
	clock = ((uint64_t)((unsigned int)(ntp >> 32) - 0x83AA7E80)) * 1000000; // 1/1/1900 -> 1/1/1970

	// low 32 bits in picosecond
	clock += ((ntp & 0xFFFFFFFF) * 15625) >> 26;

	return clock;
}

#if defined(_DEBUG) || defined(DEBUG)
void rtp_time_test(void)
{
	const uint64_t ntp = 0xe2e1d897e9c38b05ULL;
	uint64_t clock;
	struct tm* tm;
	time_t t;

	clock = ntp2clock(ntp);
	t = (time_t)(clock / 1000000);
	tm = gmtime(&t);
	assert(tm->tm_year + 1900 == 2020 && tm->tm_mon == 7 && tm->tm_mday == 15 && tm->tm_hour == 3 && tm->tm_min == 44 && tm->tm_sec == 23);
	assert(clock2ntp(clock) == 0xe2e1d897e9c38b04ULL);
}
#endif
