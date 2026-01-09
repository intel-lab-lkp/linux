/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LINUX_FFS_VAL_H_
#define _ASM_LINUX_FFS_VAL_H_

/**
 * ffs_val - find the value of the first set bit
 * @x: the value to search
 *
 * Unlike ffs(), which returns a bit position, ffs_val() returns the bit
 * value itself.
 *
 * Returns:
 * least significant non-zero bit, 0 if all bits are zero
 */
#define ffs_val(x)			\
({					\
	const typeof(x) val__ = (x);	\
	val__ & -val__;			\
})

#endif /* _ASM_LINUX_FFS_VAL_H_ */
