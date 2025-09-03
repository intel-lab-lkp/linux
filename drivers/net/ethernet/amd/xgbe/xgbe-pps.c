// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-3-Clause)
/*
 * Copyright (c) 2014-2025, Advanced Micro Devices, Inc.
 * Copyright (c) 2014, Synopsys, Inc.
 * All rights reserved
 *
 * Author: Raju Rangoju <Raju.Rangoju@amd.com>
 */

#include "xgbe.h"
#include "xgbe-common.h"

static u32 get_pps_mask(unsigned int x)
{
	return GENMASK(PPS_MAXIDX(x), PPS_MINIDX(x));
}

static u32 get_pps_cmd(unsigned int x, u32 val)
{
	return (val & GENMASK(3, 0)) << PPS_MINIDX(x);
}

static u32 get_target_mode_sel(unsigned int x, u32 val)
{
	return (val & GENMASK(1, 0)) << (PPS_MAXIDX(x) - 2);
}

int xgbe_pps_config(struct xgbe_prv_data *pdata,
		    struct xgbe_pps_config *cfg, int index, bool on)
{
	unsigned int value = 0;
	unsigned int tnsec;
	u64 period;

	tnsec = XGMAC_IOREAD(pdata, MAC_PPSx_TTNSR(index));
	if (XGMAC_GET_BITS(tnsec, MAC_PPSx_TTNSR, TRGTBUSY0))
		return -EBUSY;

	value = XGMAC_IOREAD(pdata, MAC_PPSCR);
	value &= ~get_pps_mask(index);

	if (!on) {
		value |= get_pps_cmd(index, 0x5);
		value |= PPSEN0;
		XGMAC_IOWRITE(pdata, MAC_PPSCR, value);

		return 0;
	}

	XGMAC_IOWRITE(pdata, MAC_PPSx_TTSR(index), cfg->start.tv_sec);
	XGMAC_IOWRITE(pdata, MAC_PPSx_TTNSR(index), cfg->start.tv_nsec);

	period = cfg->period.tv_sec * NSEC_PER_SEC;
	period += cfg->period.tv_nsec;
	do_div(period, XGBE_V2_TSTAMP_SSINC);

	if (period <= 1)
		return -EINVAL;

	XGMAC_IOWRITE(pdata, MAC_PPSx_INTERVAL(index), period - 1);
	period >>= 1;
	if (period <= 1)
		return -EINVAL;

	XGMAC_IOWRITE(pdata, MAC_PPSx_WIDTH(index), period - 1);

	value |= get_pps_cmd(index, 0x2);
	value |= get_target_mode_sel(index, 0x2);
	value |= PPSEN0;

	XGMAC_IOWRITE(pdata, MAC_PPSCR, value);

	return 0;
}
