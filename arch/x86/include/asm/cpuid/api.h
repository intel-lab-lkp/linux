/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_API_H
#define _ASM_X86_CPUID_API_H

#include <asm/cpuid/types.h>

#include <linux/build_bug.h>
#include <linux/init.h>
#include <linux/types.h>

#include <asm/processor.h>
#include <asm/string.h>

/*
 * Raw CPUID accessors:
 */

#ifdef CONFIG_X86_32
bool cpuid_feature(void);
#else
static inline bool cpuid_feature(void)
{
	return true;
}
#endif

static inline void native_cpuid(u32 *eax, u32 *ebx,
				u32 *ecx, u32 *edx)
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

#define NATIVE_CPUID_REG(reg)					\
static inline u32 native_cpuid_##reg(u32 op)			\
{								\
	u32 eax = op, ebx, ecx = 0, edx;			\
								\
	native_cpuid(&eax, &ebx, &ecx, &edx);			\
								\
	return reg;						\
}

/*
 * Native CPUID functions returning a single datum:
 */
NATIVE_CPUID_REG(eax)
NATIVE_CPUID_REG(ebx)
NATIVE_CPUID_REG(ecx)
NATIVE_CPUID_REG(edx)

#ifdef CONFIG_PARAVIRT_XXL
# include <asm/paravirt.h>
#else
# define __cpuid native_cpuid
#endif

/*
 * Generic CPUID function
 *
 * Clear ECX since some CPUs (Cyrix MII) do not set or clear ECX
 * resulting in stale register contents being returned.
 */
static inline void cpuid(u32 op,
			 u32 *eax, u32 *ebx,
			 u32 *ecx, u32 *edx)
{
	*eax = op;
	*ecx = 0;
	__cpuid(eax, ebx, ecx, edx);
}

/* Some CPUID calls want 'count' to be placed in ECX */
static inline void cpuid_count(u32 op, int count,
			       u32 *eax, u32 *ebx,
			       u32 *ecx, u32 *edx)
{
	*eax = op;
	*ecx = count;
	__cpuid(eax, ebx, ecx, edx);
}

/*
 * CPUID functions returning a single datum:
 */

static inline u32 cpuid_eax(u32 op)
{
	u32 eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return eax;
}

static inline u32 cpuid_ebx(u32 op)
{
	u32 eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return ebx;
}

static inline u32 cpuid_ecx(u32 op)
{
	u32 eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return ecx;
}

static inline u32 cpuid_edx(u32 op)
{
	u32 eax, ebx, ecx, edx;

	cpuid(op, &eax, &ebx, &ecx, &edx);

	return edx;
}

static inline void __cpuid_read(u32 leaf, u32 subleaf, u32 *regs)
{
	regs[CPUID_EAX] = leaf;
	regs[CPUID_ECX] = subleaf;
	__cpuid(regs + CPUID_EAX, regs + CPUID_EBX, regs + CPUID_ECX, regs + CPUID_EDX);
}

#define cpuid_read_subleaf(leaf, subleaf, regs) {	\
	static_assert(sizeof(*(regs)) == 16);		\
	__cpuid_read(leaf, subleaf, (u32 *)(regs));	\
}

#define cpuid_read(leaf, regs) {			\
	static_assert(sizeof(*(regs)) == 16);		\
	__cpuid_read(leaf, 0, (u32 *)(regs));		\
}

static inline void __cpuid_read_reg(u32 leaf, u32 subleaf,
				    enum cpuid_regs_idx regidx, u32 *reg)
{
	u32 regs[4];

	__cpuid_read(leaf, subleaf, regs);
	*reg = regs[regidx];
}

#define cpuid_subleaf_reg(leaf, subleaf, regidx, reg) {		\
	static_assert(sizeof(*(reg)) == 4);			\
	__cpuid_read_reg(leaf, subleaf, regidx, (u32 *)(reg));	\
}

#define cpuid_leaf_reg(leaf, regidx, reg) {			\
	static_assert(sizeof(*(reg)) == 4);			\
	__cpuid_read_reg(leaf, 0, regidx, (u32 *)(reg));	\
}

/*
 * Hypervisor-related APIs:
 */

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
	case 0x24:
	case 0x8000001d:
		return true;
	}

	return false;
}

#define for_each_possible_cpuid_base_hypervisor(function) \
	for (function = 0x40000000; function < 0x40010000; function += 0x100)

static inline u32 cpuid_base_hypervisor(const char *sig, u32 leaves)
{
	u32 base, eax, signature[3];

	for_each_possible_cpuid_base_hypervisor(base) {
		cpuid(base, &eax, &signature[0], &signature[1], &signature[2]);

		/*
		 * This must not compile to "call memcmp" because it's called
		 * from PVH early boot code before instrumentation is set up
		 * and memcmp() itself may be instrumented.
		 */
		if (!__builtin_memcmp(sig, signature, 12) &&
		    (leaves == 0 || ((eax - base) >= leaves)))
			return base;
	}

	return 0;
}

/*
 * CPUID(0x2) parsing:
 */

/**
 * cpuid_leaf_0x2() - Return sanitized CPUID(0x2) register output
 * @regs:	Output parameter
 *
 * Query CPUID(0x2) and store its output in @regs.  Force set any
 * invalid 1-byte descriptor returned by the hardware to zero (the NULL
 * cache/TLB descriptor) before returning it to the caller.
 *
 * Use for_each_cpuid_0x2_desc() to iterate over the register output in
 * parsed form.
 */
static inline void cpuid_leaf_0x2(union leaf_0x2_regs *regs)
{
	cpuid_read(0x2, regs);

	/*
	 * All Intel CPUs must report an iteration count of 1.	In case
	 * of bogus hardware, treat all returned descriptors as NULL.
	 */
	if (regs->desc[0] != 0x01) {
		for (int i = 0; i < 4; i++)
			regs->regv[i] = 0;
		return;
	}

	/*
	 * The most significant bit (MSB) of each register must be clear.
	 * If a register is invalid, replace its descriptors with NULL.
	 */
	for (int i = 0; i < 4; i++) {
		if (regs->reg[i].invalid)
			regs->regv[i] = 0;
	}
}

/**
 * for_each_cpuid_0x2_desc() - Iterator for parsed CPUID(0x2) descriptors
 * @_regs:	CPUID(0x2) register output, as returned by cpuid_leaf_0x2()
 * @_ptr:	u8 pointer, for macro internal use only
 * @_desc:	Pointer to the parsed CPUID(0x2) descriptor at each iteration
 *
 * Loop over the 1-byte descriptors in the passed CPUID(0x2) output registers
 * @_regs.  Provide the parsed information for each descriptor through @_desc.
 *
 * To handle cache-specific descriptors, switch on @_desc->c_type.  For TLB
 * descriptors, switch on @_desc->t_type.
 *
 * Example usage for cache descriptors::
 *
 *	const struct leaf_0x2_table *desc;
 *	union leaf_0x2_regs regs;
 *	u8 *ptr;
 *
 *	cpuid_leaf_0x2(&regs);
 *	for_each_cpuid_0x2_desc(regs, ptr, desc) {
 *		switch (desc->c_type) {
 *			...
 *		}
 *	}
 */
#define for_each_cpuid_0x2_desc(_regs, _ptr, _desc)				\
	for (_ptr = &(_regs).desc[1];						\
	     _ptr < &(_regs).desc[16] && (_desc = &cpuid_0x2_table[*_ptr]);	\
	     _ptr++)

/*
 * CPUID(0x80000006) parsing:
 */

static inline bool cpuid_amd_hygon_has_l3_cache(void)
{
	return cpuid_edx(0x80000006);
}

/*
 * 'struct cpuid_leaves' accessors:
 *
 * For internal-use by the CPUID parser.  These macros do not perform any
 * sanity checks.
 */

/**
 * __cpuid_leaves_subleaf_idx() - Get parsed CPUID output (without sanity checks)
 * @_leaves:	&struct cpuid_leaves instance
 * @_leaf:	CPUID leaf, in compile-time 0xN format
 * @_subleaf:	CPUID subleaf, in compile-time decimal format
 * @_idx:	@_leaf/@_subleaf CPUID output's storage array index.  Check
 *		__CPUID_LEAF() for info on CPUID output storage arrays indexing.
 *
 * Returns the parsed CPUID output at @_leaves as a <cpuid/leaf_types.h> data
 * type: 'struct leaf_0xN_M', where 0xN is the token provided at @_leaf, and M
 * is token provided at @_subleaf.
 */
#define __cpuid_leaves_subleaf_idx(_leaves, _leaf, _subleaf, _idx)	\
	((_leaves)->leaf_ ## _leaf ## _ ## _subleaf)[_idx]

/**
 * __cpuid_leaves_subleaf_0() - Get parsed CPUID output (without sanity checks)
 * @_leaves:	&struct cpuid_leaves instance
 * @_leaf:	CPUID leaf, in compile-time 0xN format
 *
 * Like __cpuid_leaves_subleaf_idx(), but with subleaf = 0 and index = 0.
 */
#define __cpuid_leaves_subleaf_0(_leaves, _leaf)			\
	__cpuid_leaves_subleaf_idx(_leaves, _leaf, 0, 0)

/**
 * __cpuid_leaves_subleaf_info() - Get CPUID query info for @_leaf/@_subleaf
 * @_leaves:	&struct cpuid_leaves instance
 * @_leaf:	CPUID leaf, in compile-time 0xN format
 * @_subleaf:	CPUID subleaf, in compile-time decimal format
 *
 * Returns a pointer to the &struct leaf_query_info instance associated with
 * the given @_leaf/@_subleaf pair at the CPUID @_leaves data repository. See
 * __CPUID_LEAF().
 */
#define __cpuid_leaves_subleaf_info(_leaves, _leaf, _subleaf)		\
	((_leaves)->leaf_ ## _leaf ## _ ## _subleaf ## _ ## info)

/*
 * 'struct cpuid_table' accessors:
 *
 * For internal-use by the CPUID parser.  These macros perform the necessary
 * sanity checks by default.
 */

/**
 * __cpuid_table_subleaf_idx() - Get parsed CPUID output (with sanity checks)
 * @_table:	&struct cpuid_table instance
 * @_leaf:	CPUID leaf, in compile-time 0xN format
 * @_subleaf:	CPUID subleaf, in compile-time decimal format
 * @_idx:	@_leaf/@_subleaf CPUID query output's storage array index.
 *		See __CPUID_LEAF().
 *
 * Return a pointer to the requested parsed CPUID output at @_table, as a
 * <cpuid/leaf_types.h> data type: 'struct leaf_0xN_M', where 0xN is the token
 * provided at @_leaf, and M is the token provided at @_subleaf; e.g. 'struct
 * leaf_0x7_0'.
 *
 * Returns NULL if the requested CPUID @_leaf/@_subleaf/@_idx query output is
 * not present at @_table.
 */
#define __cpuid_table_subleaf_idx(_table, _leaf, _subleaf, _idx)	\
	(((_idx) >= __cpuid_leaves_subleaf_info(&((_table)->leaves), _leaf, _subleaf).nr_entries) ? \
	 NULL : &__cpuid_leaves_subleaf_idx(&((_table)->leaves), _leaf, _subleaf, _idx))

/**
 * __cpuid_table_subleaf() - Get parsed CPUID output (with sanity checks)
 * @_table:	&struct cpuid_table instance
 * @_leaf:	CPUID leaf, in compile-time 0xN format
 * @_subleaf:	CPUID subleaf, in compile-time decimal format
 *
 * Like __cpuid_table_subleaf_idx(), but with CPUID output storage index = 0.
 */
#define __cpuid_table_subleaf(_table, _leaf, _subleaf)			\
	__cpuid_table_subleaf_idx(_table, _leaf, _subleaf, 0)

/*
 * External APIs for accessing parsed CPUID data:
 *
 * Call sites should use below APIs instead of invoking direct CPUID queries.
 *
 * Benefits include:
 *
 * - Return CPUID output as typed C structures that are auto-generated from a
 *   centralized database (see <cpuid/leaf_types.h).  Such data types have a
 *   full C99 bitfield layout per CPUID leaf/subleaf combination.  Call sites
 *   can thus avoid doing ugly and cryptic bitwise operations on raw CPUID data.
 *
 * - Return cached, per-CPU, CPUID output.  Below APIs do not invoke any CPUID
 *   queries, thus avoiding their side effects like serialization and VM exits.
 *   Call-site-specific hard coded constants and macros for caching CPUID query
 *   outputs can also be avoided.
 *
 * - Return sanitized CPUID data.  Below APIs return NULL if the given CPUID
 *   leaf/subleaf input is not supported by hardware, or if the hardware CPUID
 *   output was deemed invalid by the CPUID parser.  This centralizes all CPUID
 *   data sanitization in one place (the kernel's CPUID parser.)
 *
 * - A centralized global view of system CPUID data.  Below APIs will reflect
 *   any kernel-enforced feature masking or overrides, unlike ad hoc parsing of
 *   raw CPUID output by drivers and individual call sites.
 */

/**
 * cpuid_subleaf() - Access parsed CPUID data
 * @_cpuinfo:	CPU capability structure reference ('struct cpuinfo_x86')
 * @_leaf:	CPUID leaf, in compile-time 0xN format; e.g. 0x7, 0xf
 * @_subleaf:	CPUID subleaf, in compile-time decimal format; e.g. 0, 1, 3
 *
 * Returns a pointer to parsed CPUID output, from the CPUID table inside
 * @_cpuinfo, as a <cpuid/leaf_types.h> data type: 'struct leaf_0xN_M', where
 * 0xN is the token provided at @_leaf, and M is the token provided at
 * @_subleaf; e.g. struct leaf_0x7_0.
 *
 * Returns NULL if the requested CPUID @_leaf/@_subleaf query output is not
 * present at the parsed CPUID table inside @_cpuinfo.  This can happen if:
 *
 * - The CPUID table inside @_cpuinfo has not yet been populated.
 * - The CPUID table inside @_cpuinfo was populated, but the CPU does not
 *   implement the requested CPUID @_leaf/@_subleaf combination.
 * - The CPUID table inside @_cpuinfo was populated, but the kernel's CPUID
 *   parser has predetermined that the requested CPUID @_leaf/@_subleaf
 *   hardware output is invalid or unsupported.
 *
 * Example usage::
 *
 *	const struct leaf_0x7_0 *l7_0 = cpuid_subleaf(c, 0x7, 0);
 *	if (!l7_0) {
 *		// Handle error
 *	}
 *
 *	const struct leaf_0x7_1 *l7_1 = cpuid_subleaf(c, 0x7, 1);
 *	if (!l7_1) {
 *		// Handle error
 *	}
 */
#define cpuid_subleaf(_cpuinfo, _leaf, _subleaf)			\
	__cpuid_table_subleaf(&(_cpuinfo)->cpuid, _leaf, _subleaf)

/**
 * cpuid_leaf() - Access parsed CPUID data
 * @_cpuinfo:	CPU capability structure reference ('struct cpuinfo_x86')
 * @_leaf:	CPUID leaf, in compile-time 0xN format; e.g. 0x0, 0x2, 0x80000000
 *
 * Similar to cpuid_subleaf(), but with a CPUID subleaf = 0.
 *
 * Example usage::
 *
 *	const struct leaf_0x0_0 *l0 = cpuid_leaf(c, 0x0);
 *	if (!l0) {
 *		// Handle error
 *	}
 *
 *	const struct leaf_0x80000000_0 *el0 = cpuid_leaf(c, 0x80000000);
 *	if (!el0) {
 *		// Handle error
 *	}
 */
#define cpuid_leaf(_cpuinfo, _leaf)					\
	cpuid_subleaf(_cpuinfo, _leaf, 0)

/**
 * cpuid_leaf_regs() - Access parsed CPUID data in raw format
 * @_cpuinfo:	CPU capability structure reference ('struct cpuinfo_x86')
 * @_leaf:	CPUID leaf, in compile-time 0xN format
 *
 * Similar to cpuid_leaf(), but returns a raw 'struct cpuid_regs' pointer to
 * the parsed CPUID data instead of a "typed" <cpuid/leaf_types.h> pointer.
 */
#define cpuid_leaf_regs(_cpuinfo, _leaf)				\
	((struct cpuid_regs *)(cpuid_leaf(_cpuinfo, _leaf)))

#define __cpuid_assert_leaf_has_dynamic_subleaves(_cpuinfo, _leaf)	\
	static_assert(ARRAY_SIZE((_cpuinfo)->cpuid.leaves.leaf_ ## _leaf ## _0) > 1);

/**
 * cpuid_subleaf_index() - Access parsed CPUID data at runtime subleaf index
 * @_cpuinfo:	CPU capability structure reference ('struct cpuinfo_x86')
 * @_leaf:	CPUID leaf, in compile-time 0xN format; e.g. 0x4, 0x8000001d
 * @_idx:	Index within CPUID(@_leaf) output storage array.  It must be
 *		smaller than "cpuid_subleaf_count(@_cpuinfo, @_leaf)".  Unlike
 *		@_leaf, this value can be provided dynamically.
 *
 * For a given leaf/subleaf combination, the CPUID table inside @_cpuinfo
 * contains an array of CPUID output storage entries.  An array of storage
 * entries is used to accommodate CPUID leaves which produce the same output
 * format for a large subleaf range.  This is common for CPUID hierarchical
 * objects enumeration; e.g., CPUID(0x4) and CPUID(0xd).  Check CPUID_LEAF().
 *
 * CPUID leaves that are to be accessed using this macro are specified at
 * <cpuid/types.h>, 'struct cpuid_leaves', with a CPUID_LEAF() count field
 * bigger than 1.  A build-time error will be generated otherwise.
 *
 * Example usage::
 *
 *	const struct leaf_0x4_0 *l4;
 *
 *	for (int i = 0; i < cpuid_subleaf_count(c, 0x4); i++) {
 *		l4 = cpuid_subleaf_index(c, 0x4, i);
 *		if (!l4) {
 *			// Handle error
 *		}
 *
 *		// Access CPUID(0x4, i) data; e.g. l4->cache_type
 *	}
 *
 * Beside the standard error situations detailed at cpuid_subleaf(), this
 * macro will return NULL if @_idx is out of range.
 */
#define cpuid_subleaf_index(_cpuinfo, _leaf, _idx)			\
({									\
	__cpuid_assert_leaf_has_dynamic_subleaves(_cpuinfo, _leaf);	\
	__cpuid_table_subleaf_idx(&(_cpuinfo)->cpuid, _leaf, 0, _idx);	\
})

/**
 * cpuid_subleaf_index_regs() - Access parsed CPUID data at runtime subleaf index
 * @_cpuinfo:	CPU capability structure reference ('struct cpuinfo_x86')
 * @_leaf:	CPUID leaf, in compile-time 0xN format; e.g. 0x4, 0x8000001d
 * @_idx:	Index within CPUID(@_leaf) output storage array.  It must be
 *		smaller than "cpuid_subleaf_count(@_cpuinfo, @_leaf)".
 *
 * Similar to cpuid_subleaf_index(), but returns a raw 'struct cpuid_regs'
 * pointer to the parsed CPUID data, instead of a "typed" <cpuid/leaf_types.h>
 * pointer.
 */
#define cpuid_subleaf_index_regs(_cpuinfo, _leaf, _idx)			\
	((struct cpuid_regs *)cpuid_subleaf_index(_cpuinfo, _leaf, _idx))

/**
 * cpuid_subleaf_count() - Number of valid (filled) subleaves for @_leaf
 * @_cpuinfo:	CPU capability structure reference ('struct cpuinfo_x86')
 * @_leaf:	CPUID leaf, in compile-time 0xN format; e.g. 0x4, 0x8000001d
 *
 * Return the number of subleaves filled by the CPUID parser for @_leaf. Check
 * cpuid_subleaf_index().
 *
 * CPUID leaves that are to be accessed using this macro are specified at
 * <cpuid/types.h>, 'struct cpuid_leaves', with a CPUID_LEAF() count field
 * bigger than 1.  A build-time error will be generated otherwise.
 */
#define cpuid_subleaf_count(_cpuinfo, _leaf)				\
({									\
	__cpuid_assert_leaf_has_dynamic_subleaves(_cpuinfo, _leaf);	\
	__cpuid_leaves_subleaf_info(&(_cpuinfo)->cpuid.leaves, _leaf, 0).nr_entries; \
})

/*
 * Convenience leaf-specific functions (using parsed CPUID data):
 */

/*
 * CPUID(0x2)
 */

/**
 * for_each_parsed_cpuid_0x2_desc() - Iterator for parsed CPUID(0x2) descriptors
 * @_regs:   Leaf 0x2 register output, as returned by cpuid_leaf_regs()
 * @_ptr:  u8 pointer, for macro internal use only
 * @_desc:  Pointer to parsed descriptor information at each iteration
 *
 * Loop over the 1-byte descriptors in the passed CPUID(0x2) output registers
 * @_regs.  Provide the parsed information for each descriptor through @_desc.
 *
 * To handle cache-specific descriptors, switch on @_desc->c_type.  For TLB
 * descriptors, switch on @_desc->t_type.
 *
 * Example usage for cache descriptors::
 *
 *	const struct leaf_0x2_table *desc;
 *	struct cpuid_regs *regs;
 *	u8 *ptr;
 *
 *	regs = cpuid_leaf_regs(c, 0x2);
 *	if (!regs) {
 *		// Handle error
 *	}
 *
 *	for_each_parsed_cpuid_0x2_desc(regs, ptr, desc) {
 *		switch (desc->c_type) {
 *			...
 *		}
 *	}
 */
#define for_each_parsed_cpuid_0x2_desc(_regs, _ptr, _desc)				\
	for (({ static_assert(sizeof(*_regs) == sizeof(union leaf_0x2_regs)); }),	\
	     _ptr = &((union leaf_0x2_regs *)(_regs))->desc[1];				\
	     _ptr < &((union leaf_0x2_regs *)(_regs))->desc[16] && (_desc = &cpuid_0x2_table[*_ptr]);\
	     _ptr++)

/*
 * CPUID parser exported APIs:
 */

void __init cpuid_parser_early_scan_cpu(struct cpuinfo_x86 *c);
void cpuid_parser_scan_cpu(struct cpuinfo_x86 *c);

#endif /* _ASM_X86_CPUID_API_H */
