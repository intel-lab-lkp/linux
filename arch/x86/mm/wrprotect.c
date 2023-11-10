// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * wrprotect.c - Kernel space write protection support
 * Copyright (C) 2012 Hitachi, Ltd.
 * Copyright (C) 2023 SUSE
 * Author: YOSHIDA Masanori <masanori.yoshida.tv@hitachi.com>
 * Author: Lukas Hruska <lhruska@suse.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/wrprotect.h>
#include <linux/mm.h>		/* __get_free_page, etc. */
#include <linux/bitmap.h>	/* bit operations */
#include <linux/memblock.h> /* max_pfn */
#include <linux/vmalloc.h>	/* vzalloc, vfree */
#include <linux/hugetlb.h>	/* __flush_tlb_all */
#include <linux/pagewalk.h>	/* walk_page_range_novma */
#include <linux/stop_machine.h>	/* stop_machine */
#include <asm/sections.h>	/* __per_cpu_* */
#include <asm/set_memory.h> /* set_memory_4k */
#include <asm/e820/api.h>	/* e820__mapped_any */
#include <asm/e820/types.h>	/* E820_TYPE_RAM */

#define PGBMP_LEN			PAGE_ALIGN(sizeof(long) * BITS_TO_LONGS(max_pfn))
#define DIRECT_MAP_SIZE		(1UL << MAX_PHYSMEM_BITS)

enum state {
	WRPROTECT_STATE_UNINIT,
	WRPROTECT_STATE_INITED,
	WRPROTECT_STATE_STARTED,
	WRPROTECT_STATE_SWEPT,
};

/* wrprotect's stuffs */
struct wrprotect_state {
	enum state state;

	/*
	 * r/o bitmap after initialization
	 * 0: there is no virt-address pointing at this pfn which
	 *    this module ever holded
	 * 1: there exists an virt-address pointing at this pfn which
	 *    is wprotect interested in
	 */
	unsigned long *pgbmp_original;
	/*
	 * r/w bitmap
	 * 0: content of this pfn was already saved
	 * 1: content of this pfn was still not saved yet
	 */
	unsigned long *pgbmp_save;

	fn_handle_page_t handle_page;
	fn_sm_init_t sm_init;
} __aligned(PAGE_SIZE);

int wrprotect_is_on;
struct wrprotect_state wrprotect_state;

static int split_large_pages_walk_pud(pud_t *pud, unsigned long addr, unsigned long next,
	struct mm_walk *walk)
{
	int ret = 0;

	if (pud_present(*pud) && pud_large(*pud))
		ret = set_memory_4k(addr, 1);
	if (ret)
		return -EFAULT;

	return 0;
}

static int split_large_pages_walk_pmd(pmd_t *pmd, unsigned long addr, unsigned long next,
	struct mm_walk *walk)
{
	int ret = 0;

	if (pmd_present(*pmd) && pmd_large(*pmd))
		ret = set_memory_4k(addr, 1);
	if (ret)
		return -EFAULT;

	return 0;
}

/* split_large_pages
 *
 * This function splits all large pages in straight mapping area into 4K ones.
 * Currently wrprotect supports only 4K pages, and so this is needed.
 */
static int split_large_pages(void)
{
	int ret;
	struct mm_walk_ops split_large_pages_walk_ops;

	memset(&split_large_pages_walk_ops, 0, sizeof(struct mm_walk_ops));
	split_large_pages_walk_ops.pud_entry = split_large_pages_walk_pud;
	split_large_pages_walk_ops.pmd_entry = split_large_pages_walk_pmd;

	mmap_write_lock(&init_mm);
	ret = walk_page_range_novma(&init_mm, PAGE_OFFSET, PAGE_OFFSET + DIRECT_MAP_SIZE,
		&split_large_pages_walk_ops, init_mm.pgd, NULL);
	mmap_write_unlock(&init_mm);

	return 0;
}

struct sm_context {
	int leader_cpu;
	int leader_done;
	int (*fn_leader)(void *arg);
	int (*fn_follower)(void *arg);
	void *arg;
};

static int call_leader_follower(void *data)
{
	int ret;
	struct sm_context *ctx = data;

	if (smp_processor_id() == ctx->leader_cpu) {
		ret = ctx->fn_leader(ctx->arg);
		ctx->leader_done = 1;
	} else {
		while (!ctx->leader_done)
			cpu_relax();
		ret = ctx->fn_follower(ctx->arg);
	}

	return ret;
}

/* stop_machine_leader_follower
 *
 * Calls stop_machine with a leader CPU and follower CPUs
 * executing different codes.
 * At first, the leader CPU is selected randomly and executes its code.
 * After that, follower CPUs execute their codes.
 */
static int stop_machine_leader_follower(
		int (*fn_leader)(void *),
		int (*fn_follower)(void *),
		void *arg)
{
	int cpu;
	struct sm_context ctx;

	preempt_disable();
	cpu = smp_processor_id();
	preempt_enable();

	memset(&ctx, 0, sizeof(ctx));
	ctx.leader_cpu = cpu;
	ctx.leader_done = 0;
	ctx.fn_leader = fn_leader;
	ctx.fn_follower = fn_follower;
	ctx.arg = arg;

	return stop_machine(call_leader_follower, &ctx, cpu_online_mask);
}

/*
 * This functions converts kernel address to it's pfn in most optimal way:
 * direct mapping address -> __pa
 * other address -> lookup_address -> pte_pfn
 */
static unsigned long kernel_address_to_pfn(unsigned long addr, int *level)
{
	pte_t *ptep;
	unsigned long pfn;

	if (addr >= PAGE_OFFSET && addr < PAGE_OFFSET + DIRECT_MAP_SIZE) {
		// Direct-mapped addresses
		pfn = __pa(addr) >> PAGE_SHIFT;
	} else {
		// Non-direct-mapped addresses
		ptep = lookup_address((unsigned long)addr, level);
		if (ptep && pte_present(*ptep))
			pfn = pte_pfn(*ptep);
		else
			pfn = 0;
	}

	return pfn;
}

/* wrprotect_unselect_pages
 *
 * This function clears bits corresponding to pages that cover a range
 * from start to start+len.
 */
void wrprotect_unselect_pages(
		unsigned long start,
		unsigned long len)
{
	unsigned long addr, pfn;
	int level;

	BUG_ON(start & ~PAGE_MASK);
	BUG_ON(len & ~PAGE_MASK);

	for (addr = start; addr < start + len; addr += PAGE_SIZE) {
		pfn = kernel_address_to_pfn(addr, &level);
		clear_bit(pfn, wrprotect_state.pgbmp_original);
	}
}

/* handle_addr_range
 *
 * This function executes wrprotect_state.handle_page in turns against pages that
 * cover a range from start to start+len.
 * At the same time, it clears bits corresponding to the pages.
 */
static void handle_addr_range(unsigned long start, unsigned long len)
{
	int level;
	unsigned long end = start + len;
	unsigned long pfn;

	start &= PAGE_MASK;
	while (start < end) {
		pfn = kernel_address_to_pfn(start, &level);
		if (test_bit(pfn, wrprotect_state.pgbmp_original)) {
			wrprotect_state.handle_page(pfn, start, 0);
			clear_bit(pfn, wrprotect_state.pgbmp_original);
		}
		start += PAGE_SIZE;
	}
}

/* handle_task
 *
 * This function executes handle_addr_range against task_struct & thread_info.
 */
static void handle_task(struct task_struct *t)
{
	BUG_ON(!t);
	BUG_ON(!t->stack);
	BUG_ON((unsigned long)t->stack & ~PAGE_MASK);
	handle_addr_range((unsigned long)t, sizeof(*t));
	handle_addr_range((unsigned long)t->stack, THREAD_SIZE);
}

/* handle_tasks
 *
 * This function executes handle_task against all tasks (including idle_task).
 */
static void handle_tasks(void)
{
	struct task_struct *p, *t;
	unsigned int cpu;

	do_each_thread(p, t) {
		handle_task(t);
	} while_each_thread(p, t);

	for_each_online_cpu(cpu)
		handle_task(idle_task(cpu));
}

static void handle_pmd(pmd_t *pmd)
{
	unsigned long i;

	handle_addr_range((unsigned long)pmd, PAGE_SIZE);
	for (i = 0; i < PTRS_PER_PMD; i++) {
		if (pmd_present(pmd[i]) && !pmd_large(pmd[i]))
			handle_addr_range(pmd_page_vaddr(pmd[i]), PAGE_SIZE);
	}
}

static void handle_pud(pud_t *pud)
{
	unsigned long i;

	handle_addr_range((unsigned long)pud, PAGE_SIZE);
	for (i = 0; i < PTRS_PER_PUD; i++) {
		if (pud_present(pud[i]) && !pud_large(pud[i]))
			handle_pmd((pmd_t *)pud_pgtable(pud[i]));
	}
}

static void handle_p4d(p4d_t *p4d)
{
	unsigned long i;

	handle_addr_range((unsigned long)p4d, PAGE_SIZE);
	for (i = 0; i < PTRS_PER_P4D; i++) {
		if (p4d_present(p4d[i]))
			handle_pud((pud_t *)p4d_pgtable(p4d[i]));
	}
}

/* handle_page_table
 *
 * This function executes wrprotect_state.handle_page against all pages that make up
 * page table structure and clears all bits corresponding to the pages.
 */
static void handle_page_table(void)
{
	pgd_t *pgd;
	p4d_t *p4d;
	unsigned long i;

	pgd = init_mm.pgd;
	handle_addr_range((unsigned long)pgd, PAGE_SIZE);
	for (i = pgd_index(PAGE_OFFSET); i < PTRS_PER_PGD; i++) {
		if (pgd_present(pgd[i])) {
			if (!pgtable_l5_enabled())
				p4d = (p4d_t *)(pgd+i);
			else
				p4d = (p4d_t *)pgd_page_vaddr(pgd[i]);
			handle_p4d(p4d);
		}
	}
}

/* handle_sensitive_pages
 *
 * This function executes wrprotect_state.handle_page against the following pages and
 * clears bits corresponding to them.
 * - All pages that include task_struct & thread_info
 * - All pages that make up page table structure
 * - All pages that include per_cpu variables
 * - All pages that cover kernel's data section
 */
static void handle_sensitive_pages(void)
{
	handle_tasks();
	handle_page_table();
	handle_addr_range((unsigned long)__per_cpu_offset[0], HPAGE_SIZE);
	handle_addr_range((unsigned long)_sdata, _edata - _sdata);
}

/* protect_pte
 *
 * Changes a specified page's _PAGE_RW flag and _PAGE_SOFTW1 flag.
 * If the argument protect is non-zero:
 *  - _PAGE_RW flag is cleared
 *  - _PAGE_SOFTW1 flag is set to original value of _PAGE_RW
 * If the argument protect is zero:
 *  - _PAGE_RW flag is set to _PAGE_SOFTW1
 *
 * The change is executed only when all the following are true.
 *  - The page is mapped as 4K page.
 *  - The page is originally writable.
 *
 * Returns 1 if the change is actually executed, otherwise returns 0.
 */
static int protect_pte(unsigned long addr, int protect)
{
	pte_t *ptep, pte;
	unsigned int level;

	ptep = lookup_address(addr, &level);
	if (WARN(!ptep, "livedump: Page=%016lx isn't mapped.\n", addr) ||
	    WARN(!pte_present(*ptep),
		    "livedump: Page=%016lx isn't mapped.\n", addr) ||
	    WARN(level == PG_LEVEL_NONE,
		    "livedump: Page=%016lx isn't mapped.\n", addr) ||
	    WARN(level == PG_LEVEL_2M,
		    "livedump: Page=%016lx is consisted of 2M page.\n", addr) ||
	    WARN(level == PG_LEVEL_1G,
		    "livedump: Page=%016lx is consisted of 1G page.\n", addr)) {
		return 0;
	}

	pte = *ptep;
	if (protect) {
		if (pte_write(pte)) {
			pte = pte_wrprotect(pte);
			pte = pte_set_flags(pte, _PAGE_SOFTW1);
		} else
			pte = pte_clear_flags(pte, _PAGE_SOFTW1);
	} else if (pte_flags(pte) && _PAGE_SOFTW1)
		pte = pte_mkwrite(pte);
	*ptep = pte;

	return 1;
}

/*
 * Page fault error code bits:
 *
 *   bit 0 ==	 0: no page found	1: protection fault
 *   bit 1 ==	 0: read access		1: write access
 *   bit 2 ==	 0: kernel-mode access	1: user-mode access
 *   bit 3 ==				1: use of reserved bit detected
 *   bit 4 ==				1: fault was an instruction fetch
 */
enum x86_pf_error_code {
	PF_PROT		=		1 << 0,
	PF_WRITE	=		1 << 1,
	PF_USER		=		1 << 2,
	PF_RSVD		=		1 << 3,
	PF_INSTR	=		1 << 4,
};

int wrprotect_page_fault_handler(unsigned long error_code)
{
	unsigned int level;
	unsigned long pfn, addr;

	/*
	 * Handle only kernel-mode write access
	 *
	 * error_code must be:
	 *  (1) PF_PROT
	 *  (2) PF_WRITE
	 *  (3) not PF_USER
	 *  (4) not PF_RSVD
	 *  (5) not PF_INSTR
	 */
	if (!(PF_PROT  & error_code) ||
	    !(PF_WRITE & error_code) ||
	     (PF_USER  & error_code) ||
	     (PF_RSVD  & error_code) ||
	     (PF_INSTR & error_code))
		goto not_processed;

	addr = (unsigned long)read_cr2();
	addr = addr & PAGE_MASK;

	if (addr >= PAGE_OFFSET && addr < PAGE_OFFSET + DIRECT_MAP_SIZE) {
		pfn = __pa(addr) >> PAGE_SHIFT;
	} else {
		pfn = kernel_address_to_pfn(addr, &level);
		if (pfn == 0 || level != PG_LEVEL_4K)
			goto not_processed;
	}

	if (!test_bit(pfn, wrprotect_state.pgbmp_original))
		goto not_processed;

	if (test_and_clear_bit(pfn, wrprotect_state.pgbmp_save))
		wrprotect_state.handle_page(pfn, addr, 0);

	protect_pte(addr, 0);

	return true;

not_processed:
	return false;
}

static int generic_page_walk_pmd(pmd_t *pmd, unsigned long addr, unsigned long next,
	struct mm_walk *walk)
{
	if (WARN(pmd_large(*pmd), "livedump: Page=%016lx is consisted of 2M page.\n", addr))
		return 0;

	return 0;
}

static int sm_leader_page_walk_pte(pte_t *pte, unsigned long addr, unsigned long next,
	struct mm_walk *walk)
{
	unsigned long pfn;

	if (!pte || !pte_present(*pte))
		return 0;

	pfn = pte_pfn(*pte);

	if (test_bit(pfn, wrprotect_state.pgbmp_original)) {
		if (!protect_pte(addr, 1))
			clear_bit(pfn, wrprotect_state.pgbmp_original);
	}

	return 0;
}

/* sm_leader
 *
 * Is executed by a leader CPU during stop-machine.
 *
 * This function does the following:
 * (1)Handle pages that must not be write-protected.
 * (2)Turn on the callback in the page fault handler.
 * (3)Write-protect pages which are specified by the bitmap.
 * (4)Flush TLB cache of the leader CPU.
 */
static int sm_leader(void *arg)
{
	int ret;
	struct mm_walk_ops sm_leader_walk_ops;

	memset(&sm_leader_walk_ops, 0, sizeof(struct mm_walk_ops));
	sm_leader_walk_ops.pmd_entry = generic_page_walk_pmd;
	sm_leader_walk_ops.pte_entry = sm_leader_page_walk_pte;

	handle_sensitive_pages();

	wrprotect_state.sm_init();

	wrprotect_is_on = true;

	mmap_write_lock(&init_mm);
	ret = walk_page_range_novma(&init_mm, PAGE_OFFSET, PAGE_OFFSET + DIRECT_MAP_SIZE,
	    &sm_leader_walk_ops, init_mm.pgd, NULL);
	mmap_write_unlock(&init_mm);

	if (ret)
		return ret;

	memcpy(wrprotect_state.pgbmp_save, wrprotect_state.pgbmp_original,
			PGBMP_LEN);

	__flush_tlb_all();

	return 0;
}

/* sm_follower
 *
 * Is executed by follower CPUs during stop-machine.
 * Flushes TLB cache of each CPU.
 */
static int sm_follower(void *arg)
{
	__flush_tlb_all();
	return 0;
}

/* wrprotect_start
 *
 * This function sets up write protection on the kernel space during the
 * stop-machine state.
 */
int wrprotect_start(void)
{
	int ret;

	if (wrprotect_state.state != WRPROTECT_STATE_INITED) {
		pr_warn("livedump: wrprotect isn't initialized yet.\n");
		return 0;
	}

	ret = stop_machine_leader_follower(sm_leader, sm_follower, NULL);
	if (WARN(ret, "livedump: Failed to protect pages w/errno=%d.\n", ret))
		return ret;

	wrprotect_state.state = WRPROTECT_STATE_STARTED;
	return 0;
}

static int sweep_page_walk_pte(pte_t *pte, unsigned long addr, unsigned long next,
	struct mm_walk *walk)
{
	unsigned long pfn;

	if (!pte || !pte_present(*pte))
		return 0;

	pfn = pte_pfn(*pte);

	if (test_and_clear_bit(pfn, wrprotect_state.pgbmp_save))
		wrprotect_state.handle_page(pfn, addr, 1);
	if (test_bit(pfn, wrprotect_state.pgbmp_original))
		protect_pte(addr, 0);
	if (!(pfn & 0xffUL))
		cond_resched();

	return 0;
}

/* wrprotect_sweep
 *
 * On every page specified by the bitmap, this function executes the following.
 *  - Handle the page by calling wrprotect_state.handle_page.
 *  - Unprotect the page by calling protect_page.
 *
 * The above work may be executed on the same page at the same time
 * by the notifer-call-chain.
 * test_and_clear_bit is used for exclusion control.
 */
int wrprotect_sweep(void)
{
	int ret;
	struct mm_walk_ops sweep_walk_ops;

	memset(&sweep_walk_ops, 0, sizeof(struct mm_walk_ops));
	sweep_walk_ops.pmd_entry = generic_page_walk_pmd;
	sweep_walk_ops.pte_entry = sweep_page_walk_pte;

	if (wrprotect_state.state != WRPROTECT_STATE_STARTED) {
		pr_warn("livedump: Pages aren't protected yet.\n");
		return 0;
	}

	mmap_write_lock(&init_mm);
	ret = walk_page_range_novma(&init_mm, PAGE_OFFSET, PAGE_OFFSET + DIRECT_MAP_SIZE,
	    &sweep_walk_ops, init_mm.pgd, NULL);
	mmap_write_unlock(&init_mm);
	if (ret)
		return ret;

	wrprotect_state.state = WRPROTECT_STATE_SWEPT;
	return 0;
}

/* wrprotect_create_page_bitmap
 *
 * This function creates bitmap of which each bit corresponds to physical page.
 * Here, all ram pages are selected as being write-protected.
 */
static int wrprotect_create_page_bitmap(void)
{
	unsigned long pfn;

	/* allocate on vmap area */
	wrprotect_state.pgbmp_original = vzalloc(PGBMP_LEN);
	if (!wrprotect_state.pgbmp_original)
		return -ENOMEM;
	wrprotect_state.pgbmp_save = vzalloc(PGBMP_LEN);
	if (!wrprotect_state.pgbmp_original)
		return -ENOMEM;

	/* select all ram pages */
	for (pfn = 0; pfn < max_pfn; pfn++) {
		if (e820__mapped_any(pfn << PAGE_SHIFT,
				    (pfn + 1) << PAGE_SHIFT,
				    E820_TYPE_RAM))
			set_bit(pfn, wrprotect_state.pgbmp_original);
		if (!(pfn & 0xffUL))
			cond_resched();
	}

	return 0;
}

/* wrprotect_destroy_page_bitmap
 *
 * This function frees both page bitmaps created by wrprotect_create_page_bitmap.
 */
static void wrprotect_destroy_page_bitmap(void)
{
	vfree(wrprotect_state.pgbmp_original);
	vfree(wrprotect_state.pgbmp_save);
	wrprotect_state.pgbmp_original = NULL;
	wrprotect_state.pgbmp_save = NULL;
}

static void default_handle_page(unsigned long pfn, unsigned long addr, int for_sweep)
{
}

/* wrprotect_init
 *
 * fn_handle_page:
 *   This callback is invoked to handle faulting pages.
 *   This function takes 3 arguments.
 *   First one is PFN that tells where is this address physically located.
 *   Second one is address that tells which page caused page fault.
 *   Third one is a flag that tells whether it's called in the sweep phase.
 */
int wrprotect_init(fn_handle_page_t fn_handle_page, fn_sm_init_t fn_sm_init)
{
	int ret;

	if (wrprotect_state.state != WRPROTECT_STATE_UNINIT) {
		pr_warn("livedump: wrprotect is already initialized.\n");
		return 0;
	}

	ret = wrprotect_create_page_bitmap();
	if (ret < 0) {
		pr_warn("livedump: not enough memory for wrprotect bitmaps\n");
		return -ENOMEM;
	}

	/* split all large pages in straight mapping area */
	ret = split_large_pages();
	if (ret)
		goto err;

	/* unselect internal stuffs of wrprotect */
	wrprotect_unselect_pages(
			(unsigned long)&wrprotect_state, sizeof(wrprotect_state));
	wrprotect_unselect_pages(
			(unsigned long)wrprotect_state.pgbmp_original, PGBMP_LEN);
	wrprotect_unselect_pages(
			(unsigned long)wrprotect_state.pgbmp_save, PGBMP_LEN);

	wrprotect_state.handle_page = fn_handle_page ?: default_handle_page;
	wrprotect_state.sm_init = fn_sm_init;

	wrprotect_state.state = WRPROTECT_STATE_INITED;
	return 0;

err:
	return ret;
}

static int uninit_page_walk_pte(pte_t *pte, unsigned long addr, unsigned long next,
	struct mm_walk *walk)
{
	unsigned long pfn;

	if (!pte || !pte_present(*pte))
		return 0;

	pfn = pte_pfn(*pte);

	if (!test_bit(pfn, wrprotect_state.pgbmp_original))
		return 0;
	protect_pte(addr, 0);
	*pte = pte_clear_flags(*pte, _PAGE_SOFTW1);

	if (!(pfn & 0xffUL))
		cond_resched();

	return 0;
}

void wrprotect_uninit(void)
{
	int ret;
	struct mm_walk_ops uninit_walk_ops;

	if (wrprotect_state.state == WRPROTECT_STATE_UNINIT)
		return;

	if (wrprotect_state.state == WRPROTECT_STATE_STARTED) {
		memset(&uninit_walk_ops, 0, sizeof(struct mm_walk_ops));
		uninit_walk_ops.pmd_entry = generic_page_walk_pmd;
		uninit_walk_ops.pte_entry = uninit_page_walk_pte;

		mmap_write_lock(&init_mm);
		ret = walk_page_range_novma(&init_mm, PAGE_OFFSET, PAGE_OFFSET + DIRECT_MAP_SIZE,
		    &uninit_walk_ops, init_mm.pgd, NULL);
		mmap_write_unlock(&init_mm);

		flush_tlb_all();
	}

	if (wrprotect_state.state >= WRPROTECT_STATE_STARTED)
		wrprotect_is_on = false;

	wrprotect_destroy_page_bitmap();

	wrprotect_state.handle_page = NULL;
	wrprotect_state.state = WRPROTECT_STATE_UNINIT;
}
