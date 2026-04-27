// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/mm.h>
#include <asm/rsi.h>
#include <asm/rhi.h>

/* we need an aligned rhicall for rsi_host_call. slab is not yet ready */
static struct rsi_host_call hyp_pagesize_rhicall;
unsigned long rhi_get_ipa_change_alignment(void)
{
	long ret;
	unsigned long ipa_change_align;

	hyp_pagesize_rhicall.imm = 0;
	hyp_pagesize_rhicall.gprs[0] = RHI_HOSTCONF_VERSION;
	ret = rsi_host_call(lm_alias(&hyp_pagesize_rhicall));
	if (ret != RSI_SUCCESS)
		goto err_out;

	if (hyp_pagesize_rhicall.gprs[0] != RHI_HOSTCONF_VER_1_0)
		goto err_out;

	hyp_pagesize_rhicall.imm = 0;
	hyp_pagesize_rhicall.gprs[0] = RHI_HOSTCONF_FEATURES;
	ret = rsi_host_call(lm_alias(&hyp_pagesize_rhicall));
	if (ret != RSI_SUCCESS)
		goto err_out;

	if (!(hyp_pagesize_rhicall.gprs[0] & __RHI_HOSTCONF_GET_IPA_CHANGE_ALIGNMENT))
		goto err_out;

	hyp_pagesize_rhicall.imm = 0;
	hyp_pagesize_rhicall.gprs[0] = RHI_HOSTCONF_GET_IPA_CHANGE_ALIGNMENT;
	ret = rsi_host_call(lm_alias(&hyp_pagesize_rhicall));
	if (ret != RSI_SUCCESS)
		goto err_out;

	ipa_change_align = hyp_pagesize_rhicall.gprs[0];
	/* This error needs special handling in the caller */
	if (ipa_change_align & (SZ_4K - 1))
		return 0;

	return ipa_change_align;

err_out:
	/*
	 * For failure condition assume host is built with 4K page size
	 * and hence ipa change alignment can be guest PAGE_SIZE.
	 */
	return PAGE_SIZE;
}
