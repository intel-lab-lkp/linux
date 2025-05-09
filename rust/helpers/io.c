// SPDX-License-Identifier: GPL-2.0

#include <linux/io.h>

void __iomem *rust_helper_ioremap(phys_addr_t offset, size_t size)
{
	return ioremap(offset, size);
}

void rust_helper_iounmap(void __iomem *addr)
{
	iounmap(addr);
}

#define define_rust_mmio_read_helper(name, type)    \
	type rust_helper_##name(void __iomem *addr) \
	{                                           \
		return name(addr);                  \
	}

#define define_rust_mmio_write_helper(name, type)               \
	void rust_helper_##name(type value, void __iomem *addr) \
	{                                                       \
		name(value, addr);                              \
	}

define_rust_mmio_read_helper(readb, u8);
define_rust_mmio_read_helper(readw, u16);
define_rust_mmio_read_helper(readl, u32);
#ifdef CONFIG_64BIT
define_rust_mmio_read_helper(readq, u64);
#endif

define_rust_mmio_write_helper(writeb, u8);
define_rust_mmio_write_helper(writew, u16);
define_rust_mmio_write_helper(writel, u32);
#ifdef CONFIG_64BIT
define_rust_mmio_write_helper(writeq, u64);
#endif

define_rust_mmio_read_helper(readb_relaxed, u8);
define_rust_mmio_read_helper(readw_relaxed, u16);
define_rust_mmio_read_helper(readl_relaxed, u32);
#ifdef CONFIG_64BIT
define_rust_mmio_read_helper(readq_relaxed, u64);
#endif

define_rust_mmio_write_helper(writeb_relaxed, u8);
define_rust_mmio_write_helper(writew_relaxed, u16);
define_rust_mmio_write_helper(writel_relaxed, u32);
#ifdef CONFIG_64BIT
define_rust_mmio_write_helper(writeq_relaxed, u64);
#endif
