# SPDX-License-Identifier: GPL-2.0
obj-y				:= dma-mapping.o extable.o fault.o init.o \
				   cache.o copypage.o flush.o \
				   ioremap.o mmap.o pgd.o mmu.o \
				   context.o proc.o pageattr.o fixmap.o
obj-$(CONFIG_HUGETLB_PAGE)	+= hugetlbpage.o
obj-$(CONFIG_PTDUMP_CORE)	+= ptdump.o
obj-$(CONFIG_PTDUMP_DEBUGFS)	+= ptdump_debugfs.o
obj-$(CONFIG_TRANS_TABLE)	+= trans_pgd.o
obj-$(CONFIG_TRANS_TABLE)	+= trans_pgd-asm.o
obj-$(CONFIG_DEBUG_VIRTUAL)	+= physaddr.o
obj-$(CONFIG_ARM64_MTE)		+= mteswap.o
obj-$(CONFIG_ARM64_MTE_COMP)	+= mtecomp.o
ifdef CONFIG_ARM64_MTE_COMP
obj-$(CONFIG_ARM64_MTE)		+= mteswap_comp.o
else
obj-$(CONFIG_ARM64_MTE)		+= mteswap_nocomp.o
endif
obj-$(CONFIG_ARM64_MTE_COMP_KUNIT_TEST) += test_mtecomp.o
KASAN_SANITIZE_physaddr.o	+= n

obj-$(CONFIG_KASAN)		+= kasan_init.o
KASAN_SANITIZE_kasan_init.o	:= n
