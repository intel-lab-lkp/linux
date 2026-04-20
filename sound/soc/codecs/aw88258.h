// SPDX-License-Identifier: GPL-2.0-only
//
// aw88258.h  --  AW88258 ALSA SoC Audio driver
//
// Copyright (c) 2023 awinic Technology CO., LTD
//
// Author: Jimmy Zhang <zhangjianming@awinic.com>
// Author: Weidong Wang <wangweidong.a@awinic.com>
//

#ifndef __AW88258_H__
#define __AW88258_H__

/* This file contains definitions for registers that differ between
 * the AW88261 (ID 0x2113) and the AW88258 (ID 0x1852). */

#define AW88258_PLLCTRL1_REG		(0x66)
#define AW88258_PLLCTRL2_REG		(0x67)
#define AW88258_PLLCTRL3_REG		(0x68)

#define AW88258_CCO_MUX_START_BIT	(14)
#define AW88258_CCO_MUX_BITS_LEN	(1)
#define AW88258_CCO_MUX_MASK		\
	(~(((1<<AW88258_CCO_MUX_BITS_LEN)-1) << AW88258_CCO_MUX_START_BIT))

#define AW88258_CCO_MUX_DIVIDED	(0)
#define AW88258_CCO_MUX_DIVIDED_VALUE	\
	(AW88258_CCO_MUX_DIVIDED << AW88258_CCO_MUX_START_BIT)

#define AW88258_CCO_MUX_BYPASS		(1)
#define AW88258_CCO_MUX_BYPASS_VALUE	\
	(AW88258_CCO_MUX_BYPASS << AW88258_CCO_MUX_START_BIT)

#endif
