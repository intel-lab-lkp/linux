/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CPUID-related helpers/definitions
 */

#ifndef _ASM_X86_CPUID_H
#define _ASM_X86_CPUID_H

#include <asm/string.h>

struct cpuid_regs {
	u32 eax, ebx, ecx, edx;
};

enum cpuid_regs_idx {
	CPUID_EAX = 0,
	CPUID_EBX,
	CPUID_ECX,
	CPUID_EDX,
};

#ifdef CONFIG_X86_32
extern int have_cpuid_p(void);
#else
static inline int have_cpuid_p(void)
{
	return 1;
}
#endif
static inline void native_cpuid(unsigned int *eax, unsigned int *ebx,
				unsigned int *ecx, unsigned int *edx)
{
	/* ecx is often an input as well as an output. */
	asm volatile("cpuid"
	    : "=a" (*eax),
	      "=b" (*ebx),
	      "=c" (*ecx),
	      "=d" (*edx)
	    : "0" (*eax), "2" (*ecx)
	    : "memory");
}

#define native_cpuid_reg(reg)					\
static inline unsigned int native_cpuid_##reg(unsigned int op)	\
{								\
	unsigned int eax = op, ebx, ecx = 0, edx;		\
								\
	native_cpuid(&eax, &ebx, &ecx, &edx);			\
								\
	return reg;						\
}

/*
 * Native CPUID functions returning a single datum.
 */
native_cpuid_reg(eax)
native_cpuid_reg(ebx)
native_cpuid_reg(ecx)
native_cpuid_reg(edx)

#ifdef CONFIG_PARAVIRT_XXL
#include <asm/paravirt.h>
#else
#define __cpuid			native_cpuid
#endif

/*
 * Generic CPUID function
 * clear %ecx since some cpus (Cyrix MII) do not set or clear %ecx
 * resulting in stale register contents being returned.
 */
static inline void cpuid(unsigned int op,
			 unsigned int *eax, unsigned int *ebx,
			 unsigned int *ecx, unsigned int *edx)
{
	*eax = op;
	*ecx = 0;
	__cpuid(eax, ebx, ecx, edx);
}

/* Some CPUID calls want 'count' to be placed in ecx */
static inline void cpuid_count(unsigned int op, int count,
			       unsigned int *eax, unsigned int *ebx,
			       unsigned int *ecx, unsigned int *edx)
{
	*eax = op;
	*ecx = count;
	__cpuid(eax, ebx, ecx, edx);
}

/*
 * CPUID functions returning a single datum
 */
static inline unsigned int cpuid_eax(unsigned int op)
{
	unsigned int eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return eax;
}

static inline unsigned int cpuid_ebx(unsigned int op)
{
	unsigned int eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return ebx;
}

static inline unsigned int cpuid_ecx(unsigned int op)
{
	unsigned int eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return ecx;
}

static inline unsigned int cpuid_edx(unsigned int op)
{
	unsigned int eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return edx;
}

static __always_inline bool cpuid_function_is_indexed(u32 function)
{
	switch (function) {
	case 4:
	case 7:
	case 0xb:
	case 0xd:
	case 0xf:
	case 0x10:
	case 0x12:
	case 0x14:
	case 0x17:
	case 0x18:
	case 0x1d:
	case 0x1e:
	case 0x1f:
	case 0x8000001d:
		return true;
	}

	return false;
}

#define for_each_possible_hypervisor_cpuid_base(function) \
	for (function = 0x40000000; function < 0x40010000; function += 0x100)

static inline uint32_t hypervisor_cpuid_base(const char *sig, uint32_t leaves)
{
	uint32_t base, eax, signature[3];

	for_each_possible_hypervisor_cpuid_base(base) {
		cpuid(base, &eax, &signature[0], &signature[1], &signature[2]);

		if (!memcmp(sig, signature, 12) &&
		    (leaves == 0 || ((eax - base) >= leaves)))
			return base;
	}

	return 0;
}

/*
 * By convention, CPUID is broken up into regions which each
 * have 2^16 leaves.  EAX in the first leaf of each valid
 * region returns the maximum valid leaf in that region.
 *
 * The regions can be thought of as being vendor-specific
 * areas of CPUID, but that's imprecise because everybody
 * implements the "Intel" region and Intel implements the
 * AMD region.  There are a few well-known regions:
 *  - Intel	(0x0000)
 *  - AMD	(0x8000)
 *  - Transmeta	(0x8086)
 *  - Centaur	(0xC000)
 *
 * Consider a CPU where the maximum leaf in the Transmeta
 * region is 2.  On such a CPU, leaf 0x80860000 would contain:
 * EAX==0x80860002.
 * region-^^^^
 *   max leaf-^^^^
 */
static inline u32 cpuid_region_max_leaf(u16 region)
{
	u32 eax = cpuid_eax(region << 16);

	/*
	 * An unsupported region may return data from the last
	 * "basic" leaf, which is essentially garbage.  Avoid
	 * mistaking basic leaf data for region data.
	 *
	 * Note: this is not perfect.  It is theoretically
	 * possible for the last basic leaf to _resemble_ a
	 * valid first leaf from a region that doesn't exist.
	 * But Intel at least seems to pad out the basic region
	 * with 0's, possibly to avoid this.
	 */
	if ((eax >> 16) != region)
		return 0;

	return eax;
}

/* Returns true if the leaf exists and @value was populated */
static inline bool get_cpuid_region_leaf(u32 leaf, enum cpuid_regs_idx reg,
					 u32 *value)
{
	u16 region = leaf >> 16;
	u32 regs[4];

	if (cpuid_region_max_leaf(region) < leaf)
		return false;

	cpuid(leaf, &regs[CPUID_EAX], &regs[CPUID_EBX],
	            &regs[CPUID_ECX], &regs[CPUID_EDX]);

	*value = regs[reg];

	return true;
}

#endif /* _ASM_X86_CPUID_H */
