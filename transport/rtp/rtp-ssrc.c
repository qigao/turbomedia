#include "platform.h"

#include <stdint.h>

int rtp_ssrc_generate(uint32_t *ssrc)
{
	if (!ssrc)
		return -1;

	return salts_secure_random(ssrc, sizeof(*ssrc));
}
