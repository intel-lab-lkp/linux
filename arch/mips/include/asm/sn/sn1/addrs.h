/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SN1 (Bedrock) address space layout.
 *
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 */
#ifndef _ASM_SN_SN1_ADDRS_H
#define _ASM_SN_SN1_ADDRS_H

#ifdef CONFIG_SGI_SN_N_MODE

#define SN1_NODE_SIZE_BITS	32
#define SN1_NASID_SHFT		32

#else /* !defined(CONFIG_SGI_SN_N_MODE), assume that M-mode is desired */

#define SN1_NODE_SIZE_BITS	33
#define SN1_NASID_SHFT		33

#endif /* !defined(CONFIG_SGI_SN_N_MODE) */

#endif /* _ASM_SN_SN1_ADDRS_H */
