// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: util.c
 *
 * Abstract: This Include file used to implement platform independent APIs
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 8/25/2014		Creation	Peter.Guo
 */

#include "../include/basic.h"
#include "../include/debug.h"

/*
 *
 * Function Name: swapu32
 *
 * Abstract:
 *
 *			swap the u32 type byte order
 *
 * Input:
 *
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 */
u32 swapu32(u32 value)
{
	u32 ret = ((value & 0xFF000000) >> 24) | ((value & 0x00FF0000) >> 8) |
	    ((value & 0x0000FF00) << 8) | ((value & 0x000000FF) << 24);
	return ret;
}

static void *va_offset_va(void *va, u32 offset)
{
	va = (byte *) va + offset;
	return va;
}

void pa_offset_pa(phy_addr_t *pa, u32 offset)
{
	u32 pal = 0, pah = 0;
	u64 pa64 = 0;

	pal = os_get_phy_addr32l(*pa);
	pah = os_get_phy_addr32h(*pa);
	pa64 = pah;
	pa64 <<= 32;
	pa64 += pal;
	pa64 += offset;
	os_set_phy_add64(pa, pa64);
}

bool resize_dma_buf(dma_desc_buf_t *p, u32 sz)
{
	if (sz > p->len) {
		DbgErr("try to put over size to buf\n");
		return FALSE;
	}
	p->va = va_offset_va(p->va, sz);
	pa_offset_pa(&p->pa, sz);
	p->len = p->len - sz;
	return TRUE;
}

void dump_dma_buf(char *str, const dma_desc_buf_t *pdma)
{
}

u32 fls32(u32 x)
{
	s32 i;

	for (i = 31; i >= 0; i--) {
		if (x & (1 << i))
			return i;
	}

	return 0;
}

/*
 * This function will generate percetage of specified value
 */
bool random_percent_check(u32 percent)
{
	u32 val = 0;

	val = os_random_get(100);

	if (val >= (100 - percent))
		return TRUE;
	else
		return FALSE;
}

void util_init_waitloop(void *pdx, u32 timeout_ms, u32 per_us,
			loop_wait_t *wait)
{
	if (((bht_dev_ext_t *) pdx)->dump_mode == FALSE) {
		wait->dump_mode = FALSE;
		wait->tick = os_get_cur_tick();
		wait->timeout = timeout_ms;
	} else {
		wait->dump_mode = TRUE;
		wait->tick = per_us;
		wait->timeout = timeout_ms * 1000;
	}
}

bool util_is_timeout(loop_wait_t *wait)
{
	bool ret = FALSE;

	if (wait->dump_mode == FALSE) {
		ret = os_is_timeout(wait->tick, wait->timeout);
	} else {
		if (wait->timeout == 0)
			ret = TRUE;
		wait->timeout -= os_min(wait->timeout, wait->tick);
	}

	return ret;
}
