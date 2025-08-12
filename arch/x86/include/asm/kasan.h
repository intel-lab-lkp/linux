/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_KASAN_H
#define _ASM_X86_KASAN_H

#include <linux/const.h>
#include <linux/kasan-tags.h>
#include <linux/types.h>
#define KASAN_SHADOW_OFFSET _AC(CONFIG_KASAN_SHADOW_OFFSET, UL)
#ifdef CONFIG_KASAN_SW_TAGS

/*
 * LLVM ABI for reporting tag mismatches in inline KASAN mode.
 * On x86 the INT3 instruction is used to carry metadata in RAX
 * to the KASAN report.
 *
 * SIZE refers to how many bytes the faulty memory access
 * requested.
 * WRITE bit, when set, indicates the access was a write, otherwise
 * it was a read.
 * RECOVER bit, when set, should allow the kernel to carry on after
 * a tag mismatch. Otherwise die() is called.
 */
#define KASAN_RAX_RECOVER	0x20
#define KASAN_RAX_WRITE		0x10
#define KASAN_RAX_SIZE_MASK	0x0f
#define KASAN_RAX_SIZE(rax)	(1 << ((rax) & KASAN_RAX_SIZE_MASK))

#else
#define KASAN_SHADOW_SCALE_SHIFT 3
#endif

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

#define __tag_shifted(tag)		FIELD_PREP(GENMASK_ULL(60, 57), tag)
#define __tag_reset(addr)		(sign_extend64((u64)(addr), 56))
#define __tag_get(addr)			((u8)FIELD_GET(GENMASK_ULL(60, 57), (u64)addr))
bool kasan_inline_handler(struct pt_regs *regs);
#else
#define __tag_shifted(tag)		0UL
#define __tag_reset(addr)		(addr)
#define __tag_get(addr)			0
static inline bool kasan_inline_handler(struct pt_regs *regs)
{
	return false;
}
#endif /* CONFIG_KASAN_SW_TAGS */

static inline void *__tag_set(const void *__addr, u8 tag)
{
	u64 addr = (u64)__addr;

	addr &= ~__tag_shifted(KASAN_TAG_MASK);
	addr |= __tag_shifted(tag);

	return (void *)addr;
}

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
