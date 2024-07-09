/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Furong Xu <0x1207@gmail.com>
 * stmmac FPE(802.3 Qbu) handling
 */

#define FPE_CTRL_STS_TRSP		BIT(19)
#define FPE_CTRL_STS_TVER		BIT(18)
#define FPE_CTRL_STS_RRSP		BIT(17)
#define FPE_CTRL_STS_RVER		BIT(16)
#define FPE_CTRL_STS_SRSP		BIT(2)
#define FPE_CTRL_STS_SVER		BIT(1)
#define FPE_CTRL_STS_EFPE		BIT(0)
