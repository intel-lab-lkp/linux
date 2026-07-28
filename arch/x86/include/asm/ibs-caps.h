/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IBS_CAPS_H
#define _ASM_X86_IBS_CAPS_H

/*
 * IBS cpuid feature detection
 */

#define IBS_CPUID_FEATURES		0x8000001b

/*
 * Same bit mask as for IBS cpuid feature flags (Fn8000_001B_EAX), but
 * bit 0 is used to indicate the existence of IBS.
 */
#define IBS_CAPS_AVAIL			(1U<<0)
#define IBS_CAPS_FETCHSAM		(1U<<1)
#define IBS_CAPS_OPSAM			(1U<<2)
#define IBS_CAPS_RDWROPCNT		(1U<<3)
#define IBS_CAPS_OPCNT			(1U<<4)
#define IBS_CAPS_BRNTRGT		(1U<<5)
#define IBS_CAPS_OPCNTEXT		(1U<<6)
#define IBS_CAPS_RIPINVALIDCHK		(1U<<7)
#define IBS_CAPS_OPBRNFUSE		(1U<<8)
#define IBS_CAPS_FETCHCTLEXTD		(1U<<9)
#define IBS_CAPS_OPDATA4		(1U<<10)
#define IBS_CAPS_ZEN4			(1U<<11)
#define IBS_CAPS_OPLDLAT		(1U<<12)
#define IBS_CAPS_DIS			(1U<<13)
#define IBS_CAPS_FETCHLAT		(1U<<14)
#define IBS_CAPS_BIT63_FILTER		(1U<<15)
#define IBS_CAPS_STRMST_RMTSOCKET	(1U<<16)
#define IBS_CAPS_MEM_PROFILER		(1U<<18)
#define IBS_CAPS_OPDTLBPGSIZE		(1U<<19)

#define IBS_CAPS_DEFAULT		(IBS_CAPS_AVAIL		\
					 | IBS_CAPS_FETCHSAM	\
					 | IBS_CAPS_OPSAM)

/*
 * IBS APIC setup
 */
#define IBSCTL				0x1cc
#define IBSCTL_LVT_OFFSET_VALID		(1ULL<<8)
#define IBSCTL_LVT_OFFSET_MASK		0x0F

/*
 * IBS Memprofiler setup
 */
#define IBSCTL_MPROF_LVT_OFFSET_VALID	(1ULL << 24)
#define IBSCTL_MPROF_LVT_OFFSET_SHIFT	16
#define IBSCTL_MPROF_LVT_OFFSET_MASK	(0xFULL << IBSCTL_MPROF_LVT_OFFSET_SHIFT)

/* IBS fetch bits/masks */
#define IBS_FETCH_L3MISSONLY		      (1ULL << 59)
#define IBS_FETCH_RAND_EN		      (1ULL << 57)
#define IBS_FETCH_VAL			      (1ULL << 49)
#define IBS_FETCH_ENABLE		      (1ULL << 48)
#define IBS_FETCH_CNT			     0xFFFF0000ULL
#define IBS_FETCH_MAX_CNT		     0x0000FFFFULL

#define IBS_FETCH_2_DIS			      (1ULL <<  0)
#define IBS_FETCH_2_FETCHLAT_FILTER	    (0xFULL <<  1)
#define IBS_FETCH_2_FETCHLAT_FILTER_SHIFT	       (1)
#define IBS_FETCH_2_EXCL_RIP_63_EQ_1	      (1ULL <<  5)
#define IBS_FETCH_2_EXCL_RIP_63_EQ_0	      (1ULL <<  6)

/*
 * IBS op bits/masks
 * The lower 7 bits of the current count are random bits
 * preloaded by hardware and ignored in software
 */
#define IBS_OP_LDLAT_EN			      (1ULL << 63)
#define IBS_OP_LDLAT_THRSH		    (0xFULL << 59)
#define IBS_OP_LDLAT_THRSH_SHIFT		      (59)
#define IBS_OP_CUR_CNT			(0xFFF80ULL << 32)
#define IBS_OP_CUR_CNT_RAND		(0x0007FULL << 32)
#define IBS_OP_CUR_CNT_EXT_MASK		   (0x7FULL << 52)
#define IBS_OP_CNT_CTL			      (1ULL << 19)
#define IBS_OP_VAL			      (1ULL << 18)
#define IBS_OP_ENABLE			      (1ULL << 17)
#define IBS_OP_L3MISSONLY		      (1ULL << 16)
#define IBS_OP_MAX_CNT			     0x0000FFFFULL
#define IBS_OP_MAX_CNT_EXT		     0x007FFFFFULL	/* not a register bit mask */
#define IBS_OP_MAX_CNT_EXT_MASK		   (0x7FULL << 20)	/* separate upper 7 bits */
#define IBS_RIP_INVALID			      (1ULL << 38)

#define IBS_OP_2_DIS			      (1ULL <<  0)
#define IBS_OP_2_EXCL_RIP_63_EQ_0	      (1ULL <<  1)
#define IBS_OP_2_EXCL_RIP_63_EQ_1	      (1ULL <<  2)
#define IBS_OP_2_STRM_ST_FILTER		      (1ULL <<  3)
#define IBS_OP_2_STRM_ST_FILTER_SHIFT		       (3)

#endif /*  _ASM_X86_IBS_CAPS_H */
