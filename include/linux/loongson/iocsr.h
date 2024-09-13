/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 * Copyright (C) 2024, Jiaxun Yang <jiaxun.yang@flygoat.com>
 */

#ifndef _LOONGSON_IOCSR_H
#define _LOONGSON_IOCSR_H

#include <linux/bits.h>
#include <linux/types.h>

#ifdef CONFIG_LOONGARCH
#include <asm/loongarch.h>
#endif

#define LOONGSON_IOCSR_FEATURES	0x8
#define  IOCSRF_TEMP			BIT_ULL(0)
#define  IOCSRF_NODECNT			BIT_ULL(1)
#define  IOCSRF_MSI			BIT_ULL(2)
#define  IOCSRF_EXTIOI			BIT_ULL(3)
#define  IOCSRF_CSRIPI			BIT_ULL(4)
#define  IOCSRF_FREQCSR			BIT_ULL(5)
#define  IOCSRF_FREQSCALE		BIT_ULL(6)
#define  IOCSRF_DVFSV1			BIT_ULL(7)
#define  IOCSRF_EIODECODE		BIT_ULL(9)
#define  IOCSRF_FLATMODE		BIT_ULL(10)
#define  IOCSRF_VM			BIT_ULL(11)
#define  IOCSRF_AVEC			BIT_ULL(15)

#define LOONGSON_IOCSR_VENDOR		0x10

#define LOONGSON_IOCSR_CPUNAME		0x20

#define LOONGSON_IOCSR_NODECNT		0x408

#define LOONGSON_IOCSR_MISC_FUNC	0x420
#define  IOCSR_MISC_FUNC_SOFT_INT	BIT_ULL(10)
#define  IOCSR_MISC_FUNC_TIMER_RESET	BIT_ULL(21)
#define  IOCSR_MISC_FUNC_EXT_IOI_EN	BIT_ULL(48)
#define  IOCSR_MISC_FUNC_AVEC_EN	BIT_ULL(51)

#define LOONGSON_IOCSR_CPUTEMP		0x428

#define LOONGSON_IOCSR_SMCMBX		0x51c

/* PerCore CSR, only accessible by local cores */
#define LOONGSON_IOCSR_IPI_STATUS	0x1000
#define LOONGSON_IOCSR_IPI_EN		0x1004
#define LOONGSON_IOCSR_IPI_SET		0x1008
#define LOONGSON_IOCSR_IPI_CLEAR	0x100c
#define LOONGSON_IOCSR_MBUF0		0x1020
#define LOONGSON_IOCSR_MBUF1		0x1028
#define LOONGSON_IOCSR_MBUF2		0x1030
#define LOONGSON_IOCSR_MBUF3		0x1038

#define LOONGSON_IOCSR_IPI_SEND	0x1040
#define  IOCSR_IPI_SEND_IP_SHIFT	0
#define  IOCSR_IPI_SEND_CPU_SHIFT	16
#define  IOCSR_IPI_SEND_BLOCKING	BIT(31)

#define LOONGSON_IOCSR_MBUF_SEND	0x1048
#define  IOCSR_MBUF_SEND_BLOCKING	BIT_ULL(31)
#define  IOCSR_MBUF_SEND_BOX_SHIFT	2
#define  IOCSR_MBUF_SEND_BOX_LO(box)	(box << 1)
#define  IOCSR_MBUF_SEND_BOX_HI(box)	((box << 1) + 1)
#define  IOCSR_MBUF_SEND_CPU_SHIFT	16
#define  IOCSR_MBUF_SEND_BUF_SHIFT	32
#define  IOCSR_MBUF_SEND_H32_MASK	0xFFFFFFFF00000000ULL

#define LOONGSON_IOCSR_ANY_SEND	0x1158
#define  IOCSR_ANY_SEND_BLOCKING	BIT_ULL(31)
#define  IOCSR_ANY_SEND_CPU_SHIFT	16
#define  IOCSR_ANY_SEND_MASK_SHIFT	27
#define  IOCSR_ANY_SEND_BUF_SHIFT	32
#define  IOCSR_ANY_SEND_H32_MASK	0xFFFFFFFF00000000ULL

/* Register offset and bit definition for CSR access */
#define LOONGSON_IOCSR_TIMER_CFG       0x1060
#define LOONGSON_IOCSR_TIMER_TICK      0x1070
#define  IOCSR_TIMER_CFG_RESERVED       (_ULCAST_(1) << 63)
#define  IOCSR_TIMER_CFG_PERIODIC       (_ULCAST_(1) << 62)
#define  IOCSR_TIMER_CFG_EN             (_ULCAST_(1) << 61)
#define  IOCSR_TIMER_MASK		0x0ffffffffffffULL
#define  IOCSR_TIMER_INITVAL_RST        (_ULCAST_(0xffff) << 48)

#define LOONGSON_IOCSR_EXTIOI_NODEMAP_BASE	0x14a0
#define LOONGSON_IOCSR_EXTIOI_IPMAP_BASE	0x14c0
#define LOONGSON_IOCSR_EXTIOI_EN_BASE		0x1600
#define LOONGSON_IOCSR_EXTIOI_BOUNCE_BASE	0x1680
#define LOONGSON_IOCSR_EXTIOI_ISR_BASE		0x1800
#define LOONGSON_IOCSR_EXTIOI_ROUTE_BASE	0x1c00
#define IOCSR_EXTIOI_VECTOR_NUM			256

#ifndef __ASSEMBLY__
static inline void csr_any_send(unsigned int addr, unsigned int data,
				unsigned int data_mask, unsigned int cpu)
{
	uint64_t val = 0;

	val = IOCSR_ANY_SEND_BLOCKING | addr;
	val |= (cpu << IOCSR_ANY_SEND_CPU_SHIFT);
	val |= (data_mask << IOCSR_ANY_SEND_MASK_SHIFT);
	val |= ((uint64_t)data << IOCSR_ANY_SEND_BUF_SHIFT);
	iocsr_write64(val, LOONGSON_IOCSR_ANY_SEND);
}
#endif

#endif

