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
#include <asm/kvm_pkvm.h>
#include <asm/kvm_pgtable.h>
#include <asm/kvm_host.h>


enum address_markers_idx {
	PAGE_OFFSET_NR = 0,
	PAGE_END_NR,
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	KASAN_START_NR,
#endif
};

static struct addr_marker address_markers[] = {
	{ PAGE_OFFSET,			"Linear Mapping start" },
	{ 0 /* PAGE_END */,		"Linear Mapping end" },
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	{ 0 /* KASAN_SHADOW_START */,	"Kasan shadow start" },
	{ KASAN_SHADOW_END,		"Kasan shadow end" },
#endif
	{ MODULES_VADDR,		"Modules start" },
	{ MODULES_END,			"Modules end" },
	{ VMALLOC_START,		"vmalloc() area" },
	{ VMALLOC_END,			"vmalloc() end" },
	{ FIXADDR_TOT_START,		"Fixmap start" },
	{ FIXADDR_TOP,			"Fixmap end" },
	{ PCI_IO_START,			"PCI I/O start" },
	{ PCI_IO_END,			"PCI I/O end" },
	{ VMEMMAP_START,		"vmemmap start" },
	{ VMEMMAP_START + VMEMMAP_SIZE,	"vmemmap end" },
	{ -1,				NULL },
};

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
	struct pg_level *pg_level;
	const struct addr_marker *marker;
	unsigned long start_address;
	int level;
	u64 current_prot;
	bool check_wx;
	unsigned long wx_pages;
	unsigned long uxn_pages;
	struct ptdump_info_file_priv *f_priv;
};

struct prot_bits {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
};

static const struct prot_bits pte_bits[] = {
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
		.set	= "ro",
		.clear	= "RW",
	}, {
		.mask	= PTE_PXN,
		.val	= PTE_PXN,
		.set	= "NX",
		.clear	= "x ",
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

static const struct prot_bits stage2_pte_bits[] = {
	{
		.mask	= PTE_VALID,
		.val	= PTE_VALID,
		.set	= " ",
		.clear	= "F",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_HI_S2_XN,
		.val	= KVM_PTE_LEAF_ATTR_HI_S2_XN,
		.set	= "XN",
		.clear	= "  ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_R,
		.set	= "R",
		.clear	= " ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_S2AP_W,
		.set	= "W",
		.clear	= " ",
	}, {
		.mask	= KVM_PTE_LEAF_ATTR_LO_S2_AF,
		.val	= KVM_PTE_LEAF_ATTR_LO_S2_AF,
		.set	= "AF",
		.clear	= "  ",
	}, {
		.mask	= PTE_NG,
		.val	= PTE_NG,
		.set	= "FnXS",
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
		.mask	= KVM_PGTABLE_PROT_SW0,
		.val	= KVM_PGTABLE_PROT_SW0,
		.set	= "SW0", /* PKVM_PAGE_SHARED_OWNED */
	}, {
		.mask   = KVM_PGTABLE_PROT_SW1,
		.val	= KVM_PGTABLE_PROT_SW1,
		.set	= "SW1", /* PKVM_PAGE_SHARED_BORROWED */
	}, {
		.mask	= KVM_PGTABLE_PROT_SW2,
		.val	= KVM_PGTABLE_PROT_SW2,
		.set	= "SW2",
	}, {
		.mask   = KVM_PGTABLE_PROT_SW3,
		.val	= KVM_PGTABLE_PROT_SW3,
		.set	= "SW3",
	},
};

struct pg_level {
	const struct prot_bits *bits;
	const char *name;
	size_t num;
	u64 mask;
};

static struct pg_level pg_level[] = {
	{ /* pgd */
		.name	= "PGD",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* p4d */
		.name	= "P4D",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pud */
		.name	= (CONFIG_PGTABLE_LEVELS > 3) ? "PUD" : "PGD",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pmd */
		.name	= (CONFIG_PGTABLE_LEVELS > 2) ? "PMD" : "PGD",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pte */
		.name	= "PTE",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	},
};

static void dump_prot(struct pg_state *st, const struct prot_bits *bits,
			size_t num)
{
	unsigned i;

	for (i = 0; i < num; i++, bits++) {
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
	struct pg_level *pg_info = st->pg_level;
	static const char units[] = "KMGTPE";
	u64 prot = 0;

	if (level >= 0)
		prot = val & pg_info[level].mask;

	if (st->level == -1) {
		st->level = level;
		st->current_prot = prot;
		st->start_address = addr;
		pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	} else if (prot != st->current_prot || level != st->level ||
		   addr >= st->marker[1].start_address) {
		const char *unit = units;
		unsigned long delta;

		if (st->current_prot) {
			note_prot_uxn(st, addr);
			note_prot_wx(st, addr);
		}

		pt_dump_seq_printf(st->seq, "0x%016lx-0x%016lx   ",
				   st->start_address, addr);

		delta = (addr - st->start_address) >> 10;
		while (!(delta & 1023) && unit[1]) {
			delta >>= 10;
			unit++;
		}

		pt_dump_seq_printf(st->seq, "%9lu%c %s", delta, *unit,
				   pg_info[st->level].name);
		if (st->current_prot && pg_info[st->level].bits)
			dump_prot(st, pg_info[st->level].bits,
				  pg_info[st->level].num);
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
		.pg_level = &pg_level[0],
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

	for (i = 0; i < ARRAY_SIZE(pg_level); i++)
		if (pg_level[i].bits)
			for (j = 0; j < pg_level[i].num; j++)
				pg_level[i].mask |= pg_level[i].bits[j].mask;
}

static struct ptdump_info kernel_ptdump_info = {
	.mm		= &init_mm,
	.markers	= address_markers,
	.base_addr	= PAGE_OFFSET,
	.ptdump_walk	= &ptdump_walk,
};

void ptdump_check_wx(void)
{
	struct pg_state st = {
		.seq = NULL,
		.marker = (struct addr_marker[]) {
			{ 0, NULL},
			{ -1, NULL},
		},
		.pg_level = &pg_level[0],
		.level = -1,
		.check_wx = true,
		.ptdump = {
			.note_page = note_page,
			.range = (struct ptdump_range[]) {
				{PAGE_OFFSET, ~0UL},
				{0, 0}
			}
		}
	};

	ptdump_walk_pgd(&st.ptdump, &init_mm, NULL);

	if (st.wx_pages || st.uxn_pages)
		pr_warn("Checked W+X mappings: FAILED, %lu W+X pages found, %lu non-UXN pages found\n",
			st.wx_pages, st.uxn_pages);
	else
		pr_info("Checked W+X mappings: passed, no W+X pages found\n");
}

#ifdef CONFIG_PTDUMP_STAGE2_DEBUGFS
static struct ptdump_info stage2_kernel_ptdump_info;

static phys_addr_t ptdump_host_pa(void *addr)
{
	return __pa(addr);
}

static void *ptdump_host_va(phys_addr_t phys)
{
	return __va(phys);
}

static struct kvm_pgtable_mm_ops host_mmops = {
	.phys_to_virt	=	ptdump_host_va,
	.virt_to_phys	=	ptdump_host_pa,
};

static size_t stage2_get_pgd_len(void)
{
	u64 mmfr0, mmfr1, vtcr;
	u32 phys_shift = get_kvm_ipa_limit();

	mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
	mmfr1 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR1_EL1);
	vtcr = kvm_get_vtcr(mmfr0, mmfr1, phys_shift);

	return kvm_pgtable_stage2_pgd_size(vtcr);
}

static int stage2_ptdump_prepare_walk(void *file_priv)
{
	struct ptdump_info_file_priv *f_priv = file_priv;
	struct ptdump_info *info = &f_priv->info;
	struct kvm_pgtable_snapshot *snapshot;
	int ret, pgd_index, mc_index, pgd_pages_sz;
	void *page_hva;
	phys_addr_t pgd;

	snapshot = alloc_pages_exact(PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (!snapshot)
		return -ENOMEM;

	memset(snapshot, 0, PAGE_SIZE);
	ret = kvm_call_hyp_nvhe(__pkvm_host_share_hyp, virt_to_pfn(snapshot));
	if (ret)
		goto free_snapshot;

	snapshot->pgd_len = stage2_get_pgd_len();
	pgd_pages_sz = snapshot->pgd_len / PAGE_SIZE;
	snapshot->pgd_hva = alloc_pages_exact(snapshot->pgd_len,
					      GFP_KERNEL_ACCOUNT);
	if (!snapshot->pgd_hva) {
		ret = -ENOMEM;
		goto unshare_snapshot;
	}

	for (pgd_index = 0; pgd_index < pgd_pages_sz; pgd_index++) {
		page_hva = snapshot->pgd_hva + pgd_index * PAGE_SIZE;
		ret = kvm_call_hyp_nvhe(__pkvm_host_share_hyp,
					virt_to_pfn(page_hva));
		if (ret)
			goto unshare_pgd_pages;
	}

	for (mc_index = 0; mc_index < info->mc_len; mc_index++) {
		page_hva = alloc_pages_exact(PAGE_SIZE, GFP_KERNEL_ACCOUNT);
		if (!page_hva) {
			ret = -ENOMEM;
			goto free_memcache_pages;
		}

		push_hyp_memcache(&snapshot->mc, page_hva, ptdump_host_pa);
		ret = kvm_call_hyp_nvhe(__pkvm_host_share_hyp,
					virt_to_pfn(page_hva));
		if (ret) {
			pop_hyp_memcache(&snapshot->mc, ptdump_host_va);
			free_pages_exact(page_hva, PAGE_SIZE);
			goto free_memcache_pages;
		}
	}

	ret = kvm_call_hyp_nvhe(__pkvm_copy_host_stage2, snapshot);
	if (ret)
		goto free_memcache_pages;

	pgd = (phys_addr_t)snapshot->pgtable.pgd;
	snapshot->pgtable.pgd = phys_to_virt(pgd);
	f_priv->file_priv = snapshot;
	return 0;

free_memcache_pages:
	page_hva = pop_hyp_memcache(&snapshot->mc, ptdump_host_va);
	while (page_hva) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp,
					virt_to_pfn(page_hva));
		WARN_ON(ret);
		free_pages_exact(page_hva, PAGE_SIZE);
		page_hva = pop_hyp_memcache(&snapshot->mc, ptdump_host_va);
	}
unshare_pgd_pages:
	pgd_index = pgd_index - 1;
	for (; pgd_index >= 0; pgd_index--) {
		page_hva = snapshot->pgd_hva + pgd_index * PAGE_SIZE;
		ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp,
					virt_to_pfn(page_hva));
		WARN_ON(ret);
	}
	free_pages_exact(snapshot->pgd_hva, snapshot->pgd_len);
unshare_snapshot:
	WARN_ON(kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp,
				  virt_to_pfn(snapshot)));
free_snapshot:
	free_pages_exact(snapshot, PAGE_SIZE);
	f_priv->file_priv = NULL;
	return ret;
}

static void stage2_ptdump_end_walk(void *file_priv)
{
	struct ptdump_info_file_priv *f_priv = file_priv;
	struct kvm_pgtable_snapshot *snapshot = f_priv->file_priv;
	void *page_hva;
	int pgd_index, ret, pgd_pages_sz;

	if (!snapshot)
		return;

	page_hva = pop_hyp_memcache(&snapshot->mc, ptdump_host_va);
	while (page_hva) {
		ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp,
					virt_to_pfn(page_hva));
		WARN_ON(ret);
		free_pages_exact(page_hva, PAGE_SIZE);
		page_hva = pop_hyp_memcache(&snapshot->mc, ptdump_host_va);
	}

	pgd_pages_sz = snapshot->pgd_len / PAGE_SIZE;
	for (pgd_index = 0; pgd_index < pgd_pages_sz; pgd_index++) {
		page_hva = snapshot->pgd_hva + pgd_index * PAGE_SIZE;
		ret = kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp,
					virt_to_pfn(page_hva));
		WARN_ON(ret);
	}

	free_pages_exact(snapshot->pgd_hva, snapshot->pgd_len);
	WARN_ON(kvm_call_hyp_nvhe(__pkvm_host_unshare_hyp,
				  virt_to_pfn(snapshot)));
	free_pages_exact(snapshot, PAGE_SIZE);
	f_priv->file_priv = NULL;
}

static int stage2_ptdump_visitor(const struct kvm_pgtable_visit_ctx *ctx,
				 enum kvm_pgtable_walk_flags visit)
{
	struct pg_state *st = ctx->arg;
	struct ptdump_state *pt_st = &st->ptdump;

	pt_st->note_page(pt_st, ctx->addr, ctx->level, ctx->old);

	return 0;
}

static void stage2_ptdump_build_levels(struct pg_level *level,
				       size_t num_levels,
				       unsigned int start_level)
{
	static const char * const lvl_names[] = {"PGD", "PUD", "PMD", "PTE"};
	int i, j, name_index;

	if (num_levels > KVM_PGTABLE_MAX_LEVELS && start_level > 2) {
		pr_warn("invalid configuration %lu levels start_lvl %u\n",
			num_levels, start_level);
		return;
	}

	for (i = start_level; i < num_levels; i++) {
		name_index = i - start_level;
		name_index = name_index * start_level + name_index;

		level[i].name	= lvl_names[name_index];
		level[i].num	= ARRAY_SIZE(stage2_pte_bits);
		level[i].bits	= stage2_pte_bits;

		for (j = 0; j < level[i].num; j++)
			level[i].mask |= level[i].bits[j].mask;
	}
}

static void stage2_ptdump_walk(struct seq_file *s, struct ptdump_info *info)
{
	struct ptdump_info_file_priv *f_priv =
		container_of(info, struct ptdump_info_file_priv, info);
	struct kvm_pgtable_snapshot *snapshot = f_priv->file_priv;
	struct pg_state st;
	struct kvm_pgtable *pgtable;
	u64 start_ipa = 0, end_ipa;
	struct addr_marker ipa_address_markers[3];
	struct pg_level stage2_pg_level[KVM_PGTABLE_MAX_LEVELS] = {0};
	struct kvm_pgtable_walker walker = (struct kvm_pgtable_walker) {
		.cb	= stage2_ptdump_visitor,
		.arg	= &st,
		.flags	= KVM_PGTABLE_WALK_LEAF,
	};

	if (snapshot == NULL || !snapshot->pgtable.pgd)
		return;

	pgtable = &snapshot->pgtable;
	pgtable->mm_ops = &host_mmops;
	end_ipa = BIT(pgtable->ia_bits) - 1;

	memset(&ipa_address_markers[0], 0, sizeof(ipa_address_markers));

	ipa_address_markers[0].start_address = start_ipa;
	ipa_address_markers[0].name = "IPA start";

	ipa_address_markers[1].start_address = end_ipa;
	ipa_address_markers[1].name = "IPA end";

	stage2_ptdump_build_levels(stage2_pg_level, KVM_PGTABLE_MAX_LEVELS,
				   pgtable->start_level);

	st = (struct pg_state) {
		.seq		= s,
		.marker		= &ipa_address_markers[0],
		.level		= -1,
		.pg_level	= &stage2_pg_level[0],
		.f_priv		= f_priv,
		.ptdump		= {
			.note_page	= note_page,
			.range		= (struct ptdump_range[]) {
				{start_ipa,	end_ipa},
				{0,		0},
			},
		},
	};

	kvm_pgtable_walk(pgtable, start_ipa, end_ipa, &walker);
}

void ptdump_register_host_stage2(void)
{
	if (!is_protected_kvm_enabled())
		return;

	stage2_kernel_ptdump_info = (struct ptdump_info) {
		.mc_len			= host_s2_pgtable_pages(),
		.ptdump_prepare_walk	= stage2_ptdump_prepare_walk,
		.ptdump_end_walk	= stage2_ptdump_end_walk,
		.ptdump_walk		= stage2_ptdump_walk,
	};

	ptdump_debugfs_kvm_register(&stage2_kernel_ptdump_info,
				    "host_stage2_page_tables",
				    kvm_debugfs_dir);
}
#endif /* CONFIG_PTDUMP_STAGE2_DEBUGFS */

static int __init ptdump_init(void)
{
	address_markers[PAGE_END_NR].start_address = PAGE_END;
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	address_markers[KASAN_START_NR].start_address = KASAN_SHADOW_START;
#endif
	ptdump_initialize();
	ptdump_debugfs_register(&kernel_ptdump_info, "kernel_page_tables");

	return 0;
}
device_initcall(ptdump_init);
