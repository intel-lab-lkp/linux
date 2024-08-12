// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 ARM Limited
 *
 * Common helper functions for SVE and SME functionality.
 */

#include <stdbool.h>
#include <kselftest.h>
#include <asm/sigcontext.h>
#include <sys/prctl.h>

unsigned int vls[SVE_VQ_MAX];
unsigned int nvls;

int sve_fill_vls(bool use_sme, int min_vls)
{
	int vq, vl;
	int option = use_sme ? PR_SME_SET_VL : PR_SVE_SET_VL;
	int len_mask = use_sme ? PR_SME_VL_LEN_MASK : PR_SVE_VL_LEN_MASK;

	/*
	 * Enumerate up to SVE_VQ_MAX vector lengths
	 */
	for (vq = SVE_VQ_MAX; vq > 0; --vq) {
		vl = prctl(option, vq * 16);
		if (vl == -1)
			return KSFT_FAIL;

		vl &= len_mask;

		/* Did we find the lowest supported VL? */
		if (use_sme && vq < sve_vq_from_vl(vl))
			break;

		/* Skip missing VLs */
		vq = sve_vq_from_vl(vl);

		vls[nvls++] = vl;
	}

	if (nvls < min_vls) {
		fprintf(stderr, "Only %d VL supported\n", nvls);
		return KSFT_SKIP;
	}

	return KSFT_PASS;
}
