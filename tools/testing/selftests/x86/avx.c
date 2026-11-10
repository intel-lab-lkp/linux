// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE /* Required for inline xstate helpers */
#include "xstate.h"

static uint32_t avx_features[] = {
	XFEATURE_YMM,
	XFEATURE_OPMASK,
	XFEATURE_ZMM_Hi256,
	XFEATURE_Hi16_ZMM,
};

int main(void)
{
	unsigned int failed = 0, skipped = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(avx_features); i++) {
		switch (test_xstate(avx_features[i])) {
		case KSFT_FAIL:
			failed++;
			break;
		case KSFT_SKIP:
			skipped++;
			break;
		case KSFT_PASS:
		default:
		}
	}

	if (skipped == ARRAY_SIZE(avx_features))
		return KSFT_SKIP;
	else if (failed)
		return KSFT_FAIL;
	else
		return KSFT_PASS;
}
