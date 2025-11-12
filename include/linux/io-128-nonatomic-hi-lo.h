/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IO_128_NONATOMIC_HI_LO_H_
#define _LINUX_IO_128_NONATOMIC_HI_LO_H_

#include <linux/io.h>
#include <asm-generic/int-ll64.h>

static inline u128 ioread128_hi_lo(const void __iomem *addr)
{
	u32 low, high;

	high = ioread64(addr + sizeof(u64));
	low = ioread64(addr);

	return low + ((u128)high << 64);
}

static inline void iowrite128_hi_lo(u128 val, void __iomem *addr)
{
	iowrite64(val >> 64, addr + sizeof(u64));
	iowrite64(val, addr);
}

#ifndef ioread128
#define ioread128_is_nonatomic
#define ioread128 ioread128_hi_lo
#endif

#ifndef iowrite128
#define iowrite128_is_nonatomic
#define iowrite128 iowrite128_hi_lo
#endif

#endif	/* _LINUX_IO_128_NONATOMIC_HI_LO_H_ */

