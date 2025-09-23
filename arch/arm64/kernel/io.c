// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/io.c
 *
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/export.h>
#include <linux/types.h>
#include <linux/io.h>

noinstr void alt_cb_patch_ldr_to_ldar(struct alt_instr *alt,
			       __le32 *origptr, __le32 *updptr, int nr_inst)
{
	u32 rt, rn, size, orinst, altinst;

	BUG_ON(nr_inst != 1);

	orinst = le32_to_cpu(origptr[0]);

	rt = aarch64_insn_decode_register(AARCH64_INSN_REGTYPE_RT, orinst);
	rn = aarch64_insn_decode_register(AARCH64_INSN_REGTYPE_RN, orinst);
	/* The size field (31,30) matches the enum used in gen_load_acq below. */
	size = orinst >> 30;

	altinst = aarch64_insn_gen_load_acq_store_rel(rt, rn, size,
		AARCH64_INSN_LDST_LOAD_ACQ);

	updptr[0] = cpu_to_le32(altinst);
}
EXPORT_SYMBOL(alt_cb_patch_ldr_to_ldar);

/*
 * This generates a memcpy that works on a from/to address which is aligned to
 * bits. Count is in terms of the number of bits sized quantities to copy. It
 * optimizes to use the STR groupings when possible so that it is WC friendly.
 */
#define memcpy_toio_aligned(to, from, count, bits)                        \
	({                                                                \
		volatile u##bits __iomem *_to = to;                       \
		const u##bits *_from = from;                              \
		size_t _count = count;                                    \
		const u##bits *_end_from = _from + ALIGN_DOWN(_count, 8); \
                                                                          \
		for (; _from < _end_from; _from += 8, _to += 8)           \
			__const_memcpy_toio_aligned##bits(_to, _from, 8); \
		if ((_count % 8) >= 4) {                                  \
			__const_memcpy_toio_aligned##bits(_to, _from, 4); \
			_from += 4;                                       \
			_to += 4;                                         \
		}                                                         \
		if ((_count % 4) >= 2) {                                  \
			__const_memcpy_toio_aligned##bits(_to, _from, 2); \
			_from += 2;                                       \
			_to += 2;                                         \
		}                                                         \
		if (_count % 2)                                           \
			__const_memcpy_toio_aligned##bits(_to, _from, 1); \
	})

void __iowrite64_copy_full(void __iomem *to, const void *from, size_t count)
{
	memcpy_toio_aligned(to, from, count, 64);
	dgh();
}
EXPORT_SYMBOL(__iowrite64_copy_full);

void __iowrite32_copy_full(void __iomem *to, const void *from, size_t count)
{
	memcpy_toio_aligned(to, from, count, 32);
	dgh();
}
EXPORT_SYMBOL(__iowrite32_copy_full);
