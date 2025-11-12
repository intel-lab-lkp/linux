/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IO_128_NONATOMIC_LO_HI_H_
#define _LINUX_IO_128_NONATOMIC_LO_HI_H_

#include <linux/io.h>
#include <asm-generic/int-ll64.h>

static inline u128 ioread128_lo_hi(const void __iomem *addr)
{
	u64 low, high;

	low = ioread64(addr);
	high = ioread64(addr + sizeof(u64));

	return low + ((u128)high << 64);
}

static inline void iowrite128_lo_hi(u128 val, void __iomem *addr)
{
	iowrite64(val, addr);
	iowrite64(val >> 64, addr + sizeof(u64));
}

#ifndef ioread128
#define ioread128_is_nonatomic
#define ioread128 ioread128_lo_hi
#endif

#ifndef iowrite128
#define iowrite128_is_nonatomic
#define iowrite128 iowrite128_lo_hi
#endif

#endif	/* _LINUX_IO_128_NONATOMIC_LO_HI_H_ */
