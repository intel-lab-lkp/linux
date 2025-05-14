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

#define define_rust_io_read_helper(rust_name, name, type) \
	type rust_name(void __iomem *addr)                \
	{                                                 \
		return name(addr);                        \
	}

#define define_rust_io_write_helper(rust_name, name, type) \
	void rust_name(type value, void __iomem *addr)     \
	{                                                  \
		name(value, addr);                         \
	}

define_rust_io_read_helper(rust_helper_readb, readb, u8);
define_rust_io_read_helper(rust_helper_readw, readw, u16);
define_rust_io_read_helper(rust_helper_readl, readl, u32);
#ifdef CONFIG_64BIT
define_rust_io_read_helper(rust_helper_readq, readq, u64);
#endif

define_rust_io_write_helper(rust_helper_writeb, writeb, u8);
define_rust_io_write_helper(rust_helper_writew, writew, u16);
define_rust_io_write_helper(rust_helper_writel, writel, u32);
#ifdef CONFIG_64BIT
define_rust_io_write_helper(rust_helper_writeq, writeq, u64);
#endif

define_rust_io_read_helper(rust_helper_readb_relaxed, readb_relaxed, u8);
define_rust_io_read_helper(rust_helper_readw_relaxed, readw_relaxed, u16);
define_rust_io_read_helper(rust_helper_readl_relaxed, readl_relaxed, u32);
#ifdef CONFIG_64BIT
define_rust_io_read_helper(rust_helper_readq_relaxed, readq_relaxed, u64);
#endif

define_rust_io_write_helper(rust_helper_writeb_relaxed, writeb_relaxed, u8);
define_rust_io_write_helper(rust_helper_writew_relaxed, writew_relaxed, u16);
define_rust_io_write_helper(rust_helper_writel_relaxed, writel_relaxed, u32);
#ifdef CONFIG_64BIT
define_rust_io_write_helper(rust_helper_writeq_relaxed, writeq_relaxed, u64);
#endif

define_rust_io_read_helper(rust_helper_ioread8, ioread8, u8);
define_rust_io_read_helper(rust_helper_ioread16, ioread16, u16);
define_rust_io_read_helper(rust_helper_ioread32, ioread32, u32);

define_rust_io_write_helper(rust_helper_iowrite8, iowrite8, u8);
define_rust_io_write_helper(rust_helper_iowrite16, iowrite16, u16);
define_rust_io_write_helper(rust_helper_iowrite32, iowrite32, u32);
