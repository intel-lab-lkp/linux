// SPDX-License-Identifier: GPL-2.0
#include "pre.h"
#include "drivers/cxl/core/hdm.c"
#include "post.h"
#include "../stub.c"

#include <CUnit/Basic.h>
#include <stdlib.h>

static void
add_hdm_decoder_test(void)
{
	struct cxl_port port;
	struct cxl_decoder decoder;
	int targets[8];

	CU_ASSERT(add_hdm_decoder(&port, &decoder, targets) == 0);
}

static void
cxl_settle_decoders_test(void)
{
	struct cxl_hdm hdm;
	char _regs[0x200];
	void *regs = _regs;
	unsigned int msecs;

	*(long *)&_regs[CXL_HDM_DECODER0_CTRL_OFFSET(0)] = CXL_HDM_DECODER0_CTRL_COMMITTED;
	*(long *)&_regs[CXL_HDM_DECODER0_CTRL_OFFSET(1)] = CXL_HDM_DECODER0_CTRL_COMMITTED;
	*(long *)&_regs[CXL_HDM_DECODER0_CTRL_OFFSET(2)] = 0;

	hdm.regs.hdm_decoder = regs;

	/*
	 * With just one decoder, we should not see a delay, since all decoders report they
	 * are committed.
	 */
	hdm.decoder_count = 1;
	msecs = g_msecs;
	cxl_settle_decoders(&hdm);
	CU_ASSERT(g_msecs == msecs);

	/*
	 * With two decoders, we should not see a delay, since all decoders report they
	 * are committed.
	 */
	hdm.decoder_count = 2;
	msecs = g_msecs;
	cxl_settle_decoders(&hdm);
	CU_ASSERT(g_msecs == msecs);

	/*
	 * With three decoders, we should see a delay, since not all decoders report they
	 * are committed. Check that the delay is at least as big as the spec defined
	 * 10ms commit timeout (CXL 2.0 8.2.5.12.20)..
	 */
	hdm.decoder_count = 3;
	msecs = g_msecs;
	cxl_settle_decoders(&hdm);
	CU_ASSERT(g_msecs >= msecs + 10);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("app_suite", NULL, NULL);
	CU_ADD_TEST(suite, add_hdm_decoder_test);
	CU_ADD_TEST(suite, cxl_settle_decoders_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
