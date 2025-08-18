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

int xgbe_pps_config(struct xgbe_prv_data *pdata,
		    struct xgbe_pps_config *cfg, int index, int on)
{
	unsigned int value = 0;
	unsigned int tnsec;
	u64 period;

	tnsec = XGMAC_IOREAD(pdata, MAC_PPSx_TTNSR(index));
	if (XGMAC_GET_BITS(tnsec, MAC_PPSx_TTNSR, TRGTBUSY0))
		return -EBUSY;

	value = XGMAC_IOREAD(pdata, MAC_PPSCR);

	value &= ~PPSx_MASK(index);

	if (!on) {
		value |= PPSCMDx(index, 0x5);
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

	value |= PPSCMDx(index, 0x2);
	value |= TRGTMODSELx(index, 0x2);
	value |= PPSEN0;

	XGMAC_IOWRITE(pdata, MAC_PPSCR, value);
	return 0;
}
