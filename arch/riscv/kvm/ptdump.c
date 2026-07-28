// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 SiFive
 */

#include <linux/debugfs.h>
#include <linux/ptdump.h>
#include <linux/seq_file.h>

#include <linux/kvm_host.h>
#include <asm/kvm_gstage.h>
#include <asm/pgtable.h>
#include <asm/ptdump.h>

enum gstage_address_markers_idx {
	GSTAGE_GPA_START_NR,
	GSTAGE_GPA_END_NR,
	END_OF_GSTAGE_SPACE_NR
};

static const struct ptdump_prot_bits gstage_pte_bits[] = {
	{
		.mask = _PAGE_DIRTY,
		.set = "D",
		.clear = ".",
	}, {
		.mask = _PAGE_ACCESSED,
		.set = "A",
		.clear = ".",
	}, {
		.mask = _PAGE_GLOBAL,
		.set = "G",
		.clear = ".",
	}, {
		.mask = _PAGE_USER,
		.set = "U",
		.clear = ".",
	}, {
		.mask = _PAGE_EXEC,
		.set = "X",
		.clear = ".",
	}, {
		.mask = _PAGE_WRITE,
		.set = "W",
		.clear = ".",
	}, {
		.mask = _PAGE_READ,
		.set = "R",
		.clear = ".",
	}, {
		.mask = _PAGE_PRESENT,
		.set = "V",
		.clear = ".",
	}
};

static struct ptdump_pg_level gstage_pg_levels[] = {
	{ /* pgd */
		.bits = gstage_pte_bits,
		.num = ARRAY_SIZE(gstage_pte_bits),
		.name = "PGD",
	}, { /* p4d */
		.bits = gstage_pte_bits,
		.num = ARRAY_SIZE(gstage_pte_bits),
		.name = "P4D",
	}, { /* pud */
		.bits = gstage_pte_bits,
		.num = ARRAY_SIZE(gstage_pte_bits),
		.name = "PUD",
	}, { /* pmd */
		.bits = gstage_pte_bits,
		.num = ARRAY_SIZE(gstage_pte_bits),
		.name = "PMD",
	}, { /* pte */
		.bits = gstage_pte_bits,
		.num = ARRAY_SIZE(gstage_pte_bits),
		.name = "PTE",
	},
};

static void gstage_note_page(struct ptdump_state *pt_st, unsigned long addr,
			     int level, u64 val)
{
	struct ptdump_pg_state *st = container_of(pt_st, struct ptdump_pg_state, ptdump);

	note_page(pt_st, addr, level, val);

	if (level >= 0)
		st->current_prot = val & gstage_pg_levels[level].mask;
}

static void gstage_note_page_pte(struct ptdump_state *pt_st, unsigned long addr, pte_t pte)
{
	gstage_note_page(pt_st, addr, 4, pte_val(pte));
}

static void gstage_note_page_pmd(struct ptdump_state *pt_st, unsigned long addr, pmd_t pmd)
{
	gstage_note_page(pt_st, addr, 3, pmd_val(pmd));
}

static void gstage_note_page_pud(struct ptdump_state *pt_st, unsigned long addr, pud_t pud)
{
	gstage_note_page(pt_st, addr, 2, pud_val(pud));
}

static void gstage_note_page_p4d(struct ptdump_state *pt_st, unsigned long addr, p4d_t p4d)
{
	gstage_note_page(pt_st, addr, 1, p4d_val(p4d));
}

static void gstage_note_page_pgd(struct ptdump_state *pt_st, unsigned long addr, pgd_t pgd)
{
	gstage_note_page(pt_st, addr, 0, pgd_val(pgd));
}

static void gstage_note_page_flush(struct ptdump_state *pt_st)
{
	pte_t pte_zero = {0};

	gstage_note_page(pt_st, 0, -1, pte_val(pte_zero));
}

static int gstage_ptdump_show(struct seq_file *m, void *v)
{
	struct kvm *kvm = m->private;
	gpa_t gpa_size = kvm_riscv_gstage_gpa_size(kvm->arch.pgd_levels);
	struct addr_marker gpa_markers[] = {
		[GSTAGE_GPA_START_NR] = { 0, "Guest Physical Address Start" },
		[GSTAGE_GPA_END_NR]   = { gpa_size - 1, "Guest Physical Address End" },
		[END_OF_GSTAGE_SPACE_NR] = { -1, NULL },
	};
	struct ptdump_pg_state st = {
		.seq = m,
		.marker = gpa_markers,
		.level = -1,
		.pg_level = gstage_pg_levels,
		.ptdump = {
			.note_page_pte = gstage_note_page_pte,
			.note_page_pmd = gstage_note_page_pmd,
			.note_page_pud = gstage_note_page_pud,
			.note_page_p4d = gstage_note_page_p4d,
			.note_page_pgd = gstage_note_page_pgd,
			.note_page_flush = gstage_note_page_flush,
			.range = (struct ptdump_range[]) {
				{0, gpa_size - 1},
				{0, 0}
			}
		}
	};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(gstage_pg_levels); i++) {
		gstage_pg_levels[i].mask = 0;
		for (j = 0; j < ARRAY_SIZE(gstage_pte_bits); j++)
			gstage_pg_levels[i].mask |= gstage_pte_bits[j].mask;
	}

	if (kvm->arch.pgd_levels < 5)
		gstage_pg_levels[1].name = "PGD";
	if (kvm->arch.pgd_levels < 4)
		gstage_pg_levels[2].name = "PGD";
	if (kvm->arch.pgd_levels < 3)
		gstage_pg_levels[3].name = "PGD";

	ptdump_walk_pgd(&st.ptdump, kvm->mm, kvm->arch.pgd);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(gstage_ptdump);

void kvm_arch_create_vm_debugfs(struct kvm *kvm)
{
	debugfs_create_file("gstage_page_tables", 0400,
			    kvm->debugfs_dentry, kvm, &gstage_ptdump_fops);
}
