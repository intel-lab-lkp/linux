// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Debug helper to dump the current kernel pagetables of the system
 * so that we can see what the various memory ranges are set to.
 *
 * Derived from x86 and arm implementation:
 * (C) Copyright 2008 Intel Corporation
 *
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 */
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/ptdump.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/ptdump.h>
#include <asm/pgalloc.h>


#define pt_dump_seq_printf(m, fmt, args...)	\
({						\
	if (m)					\
		seq_printf(m, fmt, ##args);	\
})

#define pt_dump_seq_puts(m, fmt)	\
({					\
	if (m)				\
		seq_printf(m, fmt);	\
})

/*
 * The page dumper groups page table entries of the same type into a single
 * description. It uses pg_state to track the range information while
 * iterating over the pte entries. When the continuity is broken it then
 * dumps out a description of the range.
 */
struct pg_state {
	struct ptdump_state ptdump;
	struct seq_file *seq;
	const struct addr_marker *marker;
	const struct mm_struct *mm;
	unsigned long start_address;
	int level;
	u64 current_prot;
	bool check_wx;
	unsigned long wx_pages;
	unsigned long uxn_pages;
};

struct prot_bits {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
};

static const struct prot_bits blk_bits[] = {
	{
		.mask	= PTE_VALID,
		.val	= PTE_VALID,
		.set	= " ",
		.clear	= "F",
	}, {
		.mask	= PTE_USER,
		.val	= PTE_USER,
		.set	= "USR",
		.clear	= "   ",
	}, {
		.mask	= PTE_RDONLY,
		.val	= PTE_RDONLY,
		.set	= "RO",
		.clear	= "RW",
	}, {
		.mask	= PTE_PXN,
		.val	= PTE_PXN,
		.set	= "NX",
		.clear	= "X ",
	}, {
		.mask	= PTE_SHARED,
		.val	= PTE_SHARED,
		.set	= "SHD",
		.clear	= "   ",
	}, {
		.mask	= PTE_AF,
		.val	= PTE_AF,
		.set	= "AF",
		.clear	= "  ",
	}, {
		.mask	= PTE_NG,
		.val	= PTE_NG,
		.set	= "NG",
		.clear	= "  ",
	}, {
		.mask	= PTE_CONT,
		.val	= PTE_CONT,
		.set	= "CON",
		.clear	= "   ",
	}, {
		.mask	= PTE_TABLE_BIT,
		.val	= PTE_TABLE_BIT,
		.set	= "   ",
		.clear	= "BLK",
	}, {
		.mask	= PTE_UXN,
		.val	= PTE_UXN,
		.set	= "UXN",
		.clear	= "   ",
	}, {
		.mask	= PTE_GP,
		.val	= PTE_GP,
		.set	= "GP",
		.clear	= "  ",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_DEVICE_nGnRnE),
		.set	= "DEVICE/nGnRnE",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_DEVICE_nGnRE),
		.set	= "DEVICE/nGnRE",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL_NC),
		.set	= "MEM/NORMAL-NC",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL),
		.set	= "MEM/NORMAL",
	}, {
		.mask	= PTE_ATTRINDX_MASK,
		.val	= PTE_ATTRINDX(MT_NORMAL_TAGGED),
		.set	= "MEM/NORMAL-TAGGED",
	}
};
static const size_t num_blk_bits = ARRAY_SIZE(blk_bits);

static const struct prot_bits tbl_bits[] = {
	{
		.mask	= PTE_VALID,
		.val	= PTE_VALID,
		.set	= " ",
		.clear	= "F",
	}, {
		.mask	= PMD_TABLE_BIT,
		.val	= PMD_TABLE_BIT,
		.set	= "TBL",
		.clear	= "   ",
	}, {
		.mask	= PTE_AF,
		.val	= PTE_AF,
		.set	= "AF",
		.clear	= "  ",
	}, {
		.mask	= PMD_TABLE_PXN,
		.val	= PMD_TABLE_PXN,
		.set	= "NX",
		.clear	= "     ",
	}, {
		.mask	= PMD_TABLE_UXN,
		.val	= PMD_TABLE_UXN,
		.set	= "UXN",
		.clear	= "      ",
	}, {
		.mask	= PMD_TABLE_KERN,
		.val	= PMD_TABLE_KERN,
		.set	= "KRN",
		.clear	= "    "
	}, {
		.mask	= PMD_TABLE_PRDONLY,
		.val	= PMD_TABLE_PRDONLY,
		.set	= "RO",
		.clear	= "RW"
	}
};
static const size_t num_tbl_bits = ARRAY_SIZE(tbl_bits);

struct pg_level {
	const struct prot_bits *blk_bits;
	const struct prot_bits *tbl_bits;
	char name[4];
	u64 mask;
	unsigned long size;
};

static struct pg_level pg_level[] __ro_after_init = {
	{ /* pgd */
		.name		= "PGD",
		.blk_bits	= blk_bits,
		.size		= PGDIR_SIZE,
		.tbl_bits	= tbl_bits
	}, { /* p4d */
		.name		= "P4D",
		.blk_bits	= blk_bits,
		.size		= P4D_SIZE,
		.tbl_bits	= tbl_bits
	}, { /* pud */
		.name		= "PUD",
		.blk_bits	= blk_bits,
		.size		= PUD_SIZE,
		.tbl_bits	= tbl_bits
	}, { /* pmd */
		.name		= "PMD",
		.blk_bits	= blk_bits,
		.size		= PMD_SIZE,
		.tbl_bits	= tbl_bits
	}, { /* pte */
		.name		= "PTE",
		.blk_bits	= blk_bits,
		.size		= PAGE_SIZE,
		.tbl_bits	= NULL
	},
};

static void dump_prot(struct pg_state *st, struct pg_level level)
{
	unsigned i;
	const struct prot_bits *bits;
	int num_bits;

	if ((st->current_prot & PTE_TABLE_BIT) == PTE_TABLE_BIT &&
	    level.tbl_bits) {
		bits = level.tbl_bits;
		num_bits = num_tbl_bits;
	} else {
		bits = level.blk_bits;
		num_bits = num_blk_bits;
	}

	for (i = 0; i < num_bits; i++, bits++) {
		const char *s;

		if ((st->current_prot & bits->mask) == bits->val)
			s = bits->set;
		else
			s = bits->clear;

		if (s)
			pt_dump_seq_printf(st->seq, " %s", s);
	}
}

static void note_prot_uxn(struct pg_state *st, unsigned long addr)
{
	if (!st->check_wx)
		return;

	if ((st->current_prot & PTE_UXN) == PTE_UXN)
		return;

	WARN_ONCE(1, "arm64/mm: Found non-UXN mapping at address %p/%pS\n",
		  (void *)st->start_address, (void *)st->start_address);

	st->uxn_pages += (addr - st->start_address) / PAGE_SIZE;
}

static void note_prot_wx(struct pg_state *st, unsigned long addr)
{
	if (!st->check_wx)
		return;
	if ((st->current_prot & PTE_RDONLY) == PTE_RDONLY)
		return;
	if ((st->current_prot & PTE_PXN) == PTE_PXN)
		return;

	WARN_ONCE(1, "arm64/mm: Found insecure W+X mapping at address %p/%pS\n",
		  (void *)st->start_address, (void *)st->start_address);

	st->wx_pages += (addr - st->start_address) / PAGE_SIZE;
}

static void note_page(struct ptdump_state *pt_st, unsigned long addr, int level,
		      u64 val)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);
	static const char units[] = "KMGTPE";
	u64 prot = 0;

	/* check if the current level has been folded dynamically */
	if ((level == 1 && mm_p4d_folded(st->mm)) ||
	    (level == 2 && mm_pud_folded(st->mm)))
		level = 0;

	if (level >= 0)
		prot = val & pg_level[level].mask;

	if (st->level == -1) {
		st->level = level;
		st->current_prot = prot;
		st->start_address = addr;
		pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	} else if (prot != st->current_prot || level != st->level ||
		   addr >= st->marker[1].start_address) {
		const char *unit = units;
		unsigned long delta;
		unsigned int i;

		for (i = 0; i < st->level; i++)
			pt_dump_seq_printf(st->seq, "  ");

		if (st->current_prot) {
			note_prot_uxn(st, addr);
			note_prot_wx(st, addr);
		}

		if (st->start_address == addr) {
			if (check_add_overflow(addr, pg_level[st->level].size,
					       &delta))
				delta = ULONG_MAX - addr + 1;
			else
				delta = pg_level[st->level].size;
			pt_dump_seq_printf(st->seq, "0x%016lx-0x%016lx   ",
					   addr, addr + delta);
		} else {
			delta = (addr - st->start_address);
			pt_dump_seq_printf(st->seq, "0x%016lx-0x%016lx   ",
					   st->start_address, addr);
		}

		/* Align region information regardlesss of level */
		for (i = st->level; i < 4; i++)
			pt_dump_seq_printf(st->seq, "  ");

		delta >>= 10;
		while (!(delta & 1023) && unit[1]) {
			delta >>= 10;
			unit++;
		}
		pt_dump_seq_printf(st->seq, "%9lu%c %s", delta, *unit,
				   pg_level[st->level].name);
		if (st->current_prot && pg_level[st->level].blk_bits)
			dump_prot(st, pg_level[st->level]);
		pt_dump_seq_puts(st->seq, "\n");
		if (addr >= st->marker[1].start_address) {
			st->marker++;
			pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
		}

		st->start_address = addr;
		st->current_prot = prot;
		st->level = level;
	}

	if (addr >= st->marker[1].start_address) {
		st->marker++;
		pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	}

}

void ptdump_walk(struct seq_file *s, struct ptdump_info *info)
{
	unsigned long end = ~0UL;
	struct pg_state st;

	if (info->base_addr < TASK_SIZE_64)
		end = TASK_SIZE_64;

	st = (struct pg_state){
		.seq = s,
		.marker = info->markers,
		.mm = info->mm,
		.level = -1,
		.ptdump = {
			.note_page = note_page,
			.range = (struct ptdump_range[]){
				{info->base_addr, end},
				{0, 0}
			}
		}
	};

	ptdump_walk_pgd(&st.ptdump, info->mm, NULL);
}

static void __init ptdump_initialize(void)
{
	unsigned i, j;
	struct pg_level *level = pg_level;

	for (i = 0; i < ARRAY_SIZE(pg_level); i++, level++) {
		if (level->blk_bits)
			for (j = 0; j < num_blk_bits; j++)
				level->mask |= level->blk_bits[j].mask;
		if (level->tbl_bits)
			for (j = 0; j < num_tbl_bits; j++)
				level->mask |= level->tbl_bits[j].mask;
	}
}

static struct ptdump_info kernel_ptdump_info __ro_after_init = {
	.mm		= &init_mm,
};

bool ptdump_check_wx(void)
{
	struct pg_state st = {
		.seq = NULL,
		.marker = (struct addr_marker[]) {
			{ 0, NULL},
			{ -1, NULL},
		},
		.level = -1,
		.check_wx = true,
		.ptdump = {
			.note_page = note_page,
			.range = (struct ptdump_range[]) {
				{_PAGE_OFFSET(vabits_actual), ~0UL},
				{0, 0}
			}
		}
	};

	ptdump_walk_pgd(&st.ptdump, &init_mm, NULL);

	if (st.wx_pages || st.uxn_pages) {
		pr_warn("Checked W+X mappings: FAILED, %lu W+X pages found, %lu non-UXN pages found\n",
			st.wx_pages, st.uxn_pages);

		return false;
	} else {
		pr_info("Checked W+X mappings: passed, no W+X pages found\n");

		return true;
	}
}

static int __init ptdump_init(void)
{
	u64 page_offset = _PAGE_OFFSET(vabits_actual);
	u64 vmemmap_start = (u64)virt_to_page((void *)page_offset);
	struct addr_marker m[] = {
		{ PAGE_OFFSET,		"Linear Mapping start" },
		{ PAGE_END,		"Linear Mapping end" },
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
		{ KASAN_SHADOW_START,   "Kasan shadow start" },
		{ KASAN_SHADOW_END,     "Kasan shadow end" },
#endif
		{ MODULES_VADDR,	"Modules start" },
		{ MODULES_END,		"Modules end" },
		{ VMALLOC_START,	"vmalloc() area" },
		{ VMALLOC_END,		"vmalloc() end" },
		{ vmemmap_start,	"vmemmap start" },
		{ VMEMMAP_END,		"vmemmap end" },
		{ PCI_IO_START,		"PCI I/O start" },
		{ PCI_IO_END,		"PCI I/O end" },
		{ FIXADDR_TOT_START,    "Fixmap start" },
		{ FIXADDR_TOP,	        "Fixmap end" },
		{ -1,			NULL },
	};
	static struct addr_marker address_markers[ARRAY_SIZE(m)] __ro_after_init;

	kernel_ptdump_info.markers = memcpy(address_markers, m, sizeof(m));
	kernel_ptdump_info.base_addr = page_offset;

	ptdump_initialize();
	ptdump_debugfs_register(&kernel_ptdump_info, "kernel_page_tables");
	return 0;
}
device_initcall(ptdump_init);
