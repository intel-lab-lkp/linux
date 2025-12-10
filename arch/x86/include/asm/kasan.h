/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KASAN_H
#define _ASM_X86_KASAN_H

#include <linux/const.h>
#include <linux/kasan-tags.h>
#include <linux/types.h>
#define KASAN_SHADOW_OFFSET _AC(CONFIG_KASAN_SHADOW_OFFSET, UL)

/*
 * LLVM ABI for reporting tag mismatches in inline KASAN mode.
 * On x86 the UD1 instruction is used to carry metadata in the ECX register
 * to the KASAN report. ECX is used to differentiate KASAN from UBSan when
 * decoding the UD1 instruction.
 *
 * SIZE refers to how many bytes the faulty memory access
 * requested.
 * WRITE bit, when set, indicates the access was a write, otherwise
 * it was a read.
 * RECOVER bit, when set, should allow the kernel to carry on after
 * a tag mismatch. Otherwise die() is called.
 */
#define KASAN_ECX_RECOVER	0x20
#define KASAN_ECX_WRITE		0x10
#define KASAN_ECX_SIZE_MASK	0x0f
#define KASAN_ECX_SIZE(ecx)	(1 << ((ecx) & KASAN_ECX_SIZE_MASK))
#define KASAN_SHADOW_SCALE_SHIFT 3

/*
 * Compiler uses shadow offset assuming that addresses start
 * from 0. Kernel addresses don't start from 0, so shadow
 * for kernel really starts from compiler's shadow offset +
 * 'kernel address space start' >> KASAN_SHADOW_SCALE_SHIFT
 */
#define KASAN_SHADOW_START      (KASAN_SHADOW_OFFSET + \
					((-1UL << __VIRTUAL_MASK_SHIFT) >> \
						KASAN_SHADOW_SCALE_SHIFT))
/*
 * 47 bits for kernel address -> (47 - KASAN_SHADOW_SCALE_SHIFT) bits for shadow
 * 56 bits for kernel address -> (56 - KASAN_SHADOW_SCALE_SHIFT) bits for shadow
 */
#define KASAN_SHADOW_END        (KASAN_SHADOW_START + \
					(1ULL << (__VIRTUAL_MASK_SHIFT - \
						  KASAN_SHADOW_SCALE_SHIFT)))

#ifndef __ASSEMBLER__
#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/bits.h>

#ifdef CONFIG_KASAN_SW_TAGS
/*
 * Using the non-arch specific implementation of __kasan_mem_to_shadow() with a
 * arithmetic bit shift can cause high code complexity in KASAN's non-canonical
 * hook for x86 or might not work for some paging level and KASAN mode
 * combinations. The inline mode compiler support could also suffer from higher
 * complexity for no specific benefit. Therefore the generic mode's logical
 * shift implementation is used.
 */
static inline void *__kasan_mem_to_shadow(const void *addr)
{
	return (void *)((unsigned long)addr >> KASAN_SHADOW_SCALE_SHIFT)
		+ KASAN_SHADOW_OFFSET;
}

#define kasan_mem_to_shadow(addr)	__kasan_mem_to_shadow(addr)
#define __tag_shifted(tag)		FIELD_PREP(GENMASK_ULL(60, 57), tag)
#define __tag_reset(addr)		(sign_extend64((u64)(addr), 56))
#define __tag_get(addr)			((u8)FIELD_GET(GENMASK_ULL(60, 57), (u64)addr))
void kasan_inline_handler(struct pt_regs *regs, unsigned int metadata, u64 addr);
#else
#define __tag_shifted(tag)		0UL
#define __tag_reset(addr)		(addr)
#define __tag_get(addr)			0
static inline void kasan_inline_handler(struct pt_regs *regs, unsigned int metadata, u64 addr) { }
#endif /* CONFIG_KASAN_SW_TAGS */

#ifdef CONFIG_64BIT
static inline void *__tag_set(const void *__addr, u8 tag)
{
	u64 addr = (u64)__addr;

	addr &= ~__tag_shifted(KASAN_TAG_BYTE_MASK);
	addr |= __tag_shifted(tag & KASAN_TAG_BYTE_MASK);

	return (void *)addr;
}
#else
static inline void *__tag_set(void *__addr, u8 tag)
{
	return __addr;
}
#endif

#define arch_kasan_set_tag(addr, tag)	__tag_set(addr, tag)
#define arch_kasan_reset_tag(addr)	__tag_reset(addr)
#define arch_kasan_get_tag(addr)	__tag_get(addr)

#ifdef CONFIG_KASAN

void __init kasan_early_init(void);
void __init kasan_init(void);
void __init kasan_populate_shadow_for_vaddr(void *va, size_t size, int nid);
#else
static inline void kasan_early_init(void) { }
static inline void kasan_init(void) { }
static inline void kasan_populate_shadow_for_vaddr(void *va, size_t size,
						   int nid) { }

#endif /* CONFIG_KASAN */

#endif /* __ASSEMBLER__ */

#endif
