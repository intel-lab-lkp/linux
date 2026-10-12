// SPDX-License-Identifier: GPL-2.0-only
/*
 * gstage_dirty_log_transitions - Exercise G-stage huge-page/page
 * granularity changes caused by dirty logging, and verify that
 * dirty tracking and guest memory contents remain correct.
 *
 * With hugetlb backing and a 2M-aligned slot, G-stage faults install
 * 2M mappings while dirty logging is disabled. Enabling dirty logging
 * requires 4K-granularity tracking, so KVM splits existing huge
 * mappings either eagerly when logging is enabled (eager_page_split=1)
 * or on the first write fault (eager_page_split=0).
 *
 * Since KVM RISC-V cannot track individual 4K writes within a 2M
 * leaf, a dirty bitmap that identifies exactly the written 4K pages
 * provides evidence that the huge mapping was split into 4K mappings.
 *
 * The asserted properties are that the guest completes the sequence,
 * every KVM_RUN succeeds, the dirty bitmap identifies exactly the
 * 4K pages written by the guest, and guest data is preserved across
 * the transitions.
 *
 * Scenario 1 - Transitions on aligned blocks:
 *   Fault in NR_BLOCKS hugetlb-backed 2M blocks with dirty logging
 *   disabled, then enable logging and write selected 4K pages in
 *   each block. Verify the dirty bitmap, disable logging, and write
 *   again while checking that guest data remains intact.
 *
 * Scenario 2 - Repeated transitions:
 *   Repeat the Scenario 1 sequence over several rounds on the same
 *   blocks, verifying the dirty bitmap and guest data after each
 *   round.
 *
 * Scenario 3 - Manual dirty log protect:
 *   Repeat the transitions with KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2
 *   enabled. After each GET_DIRTY_LOG, KVM_CLEAR_DIRTY_LOG clears
 *   the reported dirty bits and re-protects the corresponding pages.
 *   The guest then writes a second set of pages, which must be
 *   reported independently. That set overlaps the first in one page,
 *   so the second bitmap also verifies that the cleared pages were
 *   write-protected again. Skipped if the capability is unavailable.
 *
 * Scenario 4 - Unaligned slot with an empty leading block:
 *   Regression test for commit 0d9dc20842db ("RISC-V: KVM: Fix skip of
 *   valid pages in kvm_riscv_gstage_wp_range"). The slot starts 4K
 *   past a 2M boundary and the first block is never touched.
 *   The next block uses 4K anonymous backing to ensure that it is
 *   mapped with 4K leaves.
 *
 *   Enabling dirty logging walks from the unaligned start, encounters
 *   the empty block, and must resume at the next 2M boundary. The
 *   broken walker advanced by 2M from the unaligned start, skipping
 *   write protection of the first 4K page in the next block. A guest
 *   write to that page then went unreported. This scenario does not
 *   require hugepages.
 */
#include <linux/bitmap.h>
#include <linux/sizes.h>
#include <sys/mman.h>

#include "kvm_util.h"
#include "processor.h"
#include "test_util.h"
#include "ucall_common.h"

#define DATA_SLOT		1
#define TEST_GVA		0xc0000000UL
#define BLOCK_SIZE		SZ_2M
#define PAGES_PER_BLOCK		(BLOCK_SIZE / SZ_4K)

/* Exercise dirty-log transitions across multiple 2M blocks. */
#define NR_BLOCKS		16

/* Two page sets with one shared page to exercise repeated dirty writes. */
static const unsigned int sel_a[] = { 0, 3, 257, PAGES_PER_BLOCK - 1 };
static const unsigned int sel_b[] = { 1, 3, 128, 400 };
#define NR_SEL_A		ARRAY_SIZE(sel_a)
#define NR_SEL_B		ARRAY_SIZE(sel_b)

#define PATTERN(blk, pg)	(0x1000000000000000ULL | ((u64)(blk) << 32) | (pg))
#define PATTERN_LOGGED		0x5a5a5a5a5a5a5a5aULL
#define PATTERN_LOGGED2		0x5b5b5b5b5b5b5b5bULL
#define PATTERN_AFTER		0xa5a5a5a5a5a5a5a5ULL

/* Guest -> host stages. */
#define STAGE_BLOCKS_READY	1
#define STAGE_LOGGED_WRITES	2
#define STAGE_LOGGED_WRITES2	3
#define STAGE_ROUND_DONE	4

static u64 *page_addr(u64 base, unsigned int blk, unsigned int pg)
{
	return (u64 *)(base + (u64)blk * BLOCK_SIZE + (u64)pg * SZ_4K);
}

static bool page_in_set(unsigned int pg, const unsigned int *sel, unsigned int nsel)
{
	unsigned int i;

	for (i = 0; i < nsel; i++)
		if (sel[i] == pg)
			return true;
	return false;
}

static void guest_fill_blocks(u64 base, u64 nblocks)
{
	unsigned int blk, pg;

	for (blk = 0; blk < nblocks; blk++)
		for (pg = 0; pg < PAGES_PER_BLOCK; pg++)
			WRITE_ONCE(*page_addr(base, blk, pg), PATTERN(blk, pg));
}

static void guest_write_selected(u64 base, u64 nblocks,
				 const unsigned int *sel, unsigned int nsel, u64 val)
{
	unsigned int blk, i;

	for (blk = 0; blk < nblocks; blk++)
		for (i = 0; i < nsel; i++)
			WRITE_ONCE(*page_addr(base, blk, sel[i]), val);
}

static void guest_restore_selected(u64 base, u64 nblocks,
				   const unsigned int *sel, unsigned int nsel)
{
	unsigned int blk, i;

	for (blk = 0; blk < nblocks; blk++)
		for (i = 0; i < nsel; i++)
			WRITE_ONCE(*page_addr(base, blk, sel[i]),
				   PATTERN(blk, sel[i]));
}

static void guest_check_blocks(u64 base, u64 nblocks, u64 val_a, u64 val_b)
{
	unsigned int blk, pg;
	u64 expect;

	for (blk = 0; blk < nblocks; blk++) {
		for (pg = 0; pg < PAGES_PER_BLOCK; pg++) {
			if (page_in_set(pg, sel_b, NR_SEL_B))
				expect = val_b;
			else if (page_in_set(pg, sel_a, NR_SEL_A))
				expect = val_a;
			else
				expect = PATTERN(blk, pg);
			GUEST_ASSERT_EQ(READ_ONCE(*page_addr(base, blk, pg)), expect);
		}
	}
}

static void guest_one_round(u64 base, u64 nblocks)
{
	/* Logging on: first page set. */
	guest_write_selected(base, nblocks, sel_a, NR_SEL_A, PATTERN_LOGGED);
	GUEST_SYNC(STAGE_LOGGED_WRITES);

	/* Host checked set A and reset dirty tracking for the next writes. */
	guest_write_selected(base, nblocks, sel_b, NR_SEL_B, PATTERN_LOGGED2);
	GUEST_SYNC(STAGE_LOGGED_WRITES2);

	/* Host verified set B and disabled logging. */
	guest_check_blocks(base, nblocks, PATTERN_LOGGED, PATTERN_LOGGED2);
	guest_write_selected(base, nblocks, sel_a, NR_SEL_A, PATTERN_AFTER);
	guest_write_selected(base, nblocks, sel_b, NR_SEL_B, PATTERN_AFTER);
	guest_check_blocks(base, nblocks, PATTERN_AFTER, PATTERN_AFTER);

	/* Restore the base pattern while logging is still off. */
	guest_restore_selected(base, nblocks, sel_a, NR_SEL_A);
	guest_restore_selected(base, nblocks, sel_b, NR_SEL_B);

	GUEST_SYNC(STAGE_ROUND_DONE);
}

static void transitions_guest_code(u64 base, u64 nblocks, u64 nrounds)
{
	u64 r;

	guest_fill_blocks(base, nblocks);
	GUEST_SYNC(STAGE_BLOCKS_READY);

	for (r = 0; r < nrounds; r++)
		guest_one_round(base, nblocks);

	GUEST_DONE();
}

static void expect_sync(struct kvm_vcpu *vcpu, u64 stage)
{
	struct ucall uc;

	vcpu_run(vcpu);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_SYNC:
		TEST_ASSERT(uc.args[1] == stage, "Expected stage %lu, got %lu",
			    stage, uc.args[1]);
		return;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
	default:
		TEST_FAIL("Unexpected ucall %lu (wanted stage %lu)", uc.cmd, stage);
	}
}

static void expect_done(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	vcpu_run(vcpu);
	switch (get_ucall(vcpu, &uc)) {
	case UCALL_DONE:
		return;
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
	default:
		TEST_FAIL("Unexpected ucall %lu (wanted done)", uc.cmd);
	}
}

static unsigned int count_dirty(const unsigned long *bmap, unsigned int nbits)
{
	unsigned int i, n = 0;

	for (i = 0; i < nbits; i++)
		if (test_bit(i, bmap))
			n++;
	return n;
}

static void check_selected_dirty(unsigned long *bmap, unsigned int nblocks,
				 const unsigned int *sel, unsigned int nsel)
{
	unsigned int blk, i, idx;

	TEST_ASSERT_EQ(count_dirty(bmap, nblocks * PAGES_PER_BLOCK), nblocks * nsel);
	for (blk = 0; blk < nblocks; blk++) {
		for (i = 0; i < nsel; i++) {
			idx = blk * PAGES_PER_BLOCK + sel[i];
			TEST_ASSERT(test_bit(idx, bmap),
				    "Block %u page %u not dirty", blk, sel[i]);
		}
	}
}

static void run_transitions(unsigned int nblocks, unsigned int nrounds, bool manual)
{
	size_t npages = nblocks * PAGES_PER_BLOCK;
	struct kvm_vcpu *vcpu;
	unsigned long *bmap;
	struct kvm_vm *vm;
	unsigned int r;
	u64 gpa;

	vm = vm_create_with_one_vcpu(&vcpu, transitions_guest_code);
	if (manual)
		vm_enable_cap(vm, KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2,
			      KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE);

	gpa = (vm_compute_max_gfn(vm) * vm->page_size) - nblocks * BLOCK_SIZE;
	gpa = align_down(gpa, BLOCK_SIZE);

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS_HUGETLB_2MB, gpa,
				    DATA_SLOT, npages, 0);
	virt_map(vm, TEST_GVA, gpa, npages);
	vcpu_args_set(vcpu, 3, TEST_GVA, (u64)nblocks, (u64)nrounds);

	bmap = bitmap_zalloc(npages);

	/* Logging off: fault in hugetlb-backed blocks before enabling logging. */
	expect_sync(vcpu, STAGE_BLOCKS_READY);

	for (r = 0; r < nrounds; r++) {
		vm_mem_region_set_flags(vm, DATA_SLOT, KVM_MEM_LOG_DIRTY_PAGES);

		/* Writes under logging: split (eager or at fault) + 4K tracking. */
		expect_sync(vcpu, STAGE_LOGGED_WRITES);

		bitmap_zero(bmap, npages);
		kvm_vm_get_dirty_log(vm, DATA_SLOT, bmap);
		check_selected_dirty(bmap, nblocks, sel_a, NR_SEL_A);

		if (manual)
			kvm_vm_clear_dirty_log(vm, DATA_SLOT, bmap, 0, npages);

		expect_sync(vcpu, STAGE_LOGGED_WRITES2);

		bitmap_zero(bmap, npages);
		kvm_vm_get_dirty_log(vm, DATA_SLOT, bmap);
		check_selected_dirty(bmap, nblocks, sel_b, NR_SEL_B);

		if (manual)
			kvm_vm_clear_dirty_log(vm, DATA_SLOT, bmap, 0, npages);

		vm_mem_region_set_flags(vm, DATA_SLOT, 0);

		/* Logging off: writes must complete on the existing 4K leaves, data intact. */
		expect_sync(vcpu, STAGE_ROUND_DONE);
	}

	expect_done(vcpu);

	free(bmap);
	kvm_vm_free(vm);
}

static void test_aligned_transitions(void)
{
	run_transitions(NR_BLOCKS, 1, false);
}

static void test_repeated_transitions(void)
{
	run_transitions(2, 4, false);
}

static void test_manual_protect_transitions(void)
{
	run_transitions(2, 2, true);
}

#define UNALIGNED_NPAGES	(2 * PAGES_PER_BLOCK)
/* Guest offset of the next 2M boundary past the unaligned slot start. */
#define UNALIGNED_BLOCK1_OFF	(BLOCK_SIZE - SZ_4K)

static void unaligned_guest_code(u64 base)
{
	u64 *first = (u64 *)(base + UNALIGNED_BLOCK1_OFF);
	unsigned int pg;

	/* Fault in the second 2M block only, as 4K leaves. The first stays empty. */
	for (pg = 0; pg < PAGES_PER_BLOCK; pg++)
		WRITE_ONCE(*(first + pg * (SZ_4K / sizeof(u64))), 0x1);
	GUEST_SYNC(STAGE_BLOCKS_READY);

	/* Logging on: this is the page the broken walker never write-protected. */
	WRITE_ONCE(*first, 0x2);
	GUEST_SYNC(STAGE_LOGGED_WRITES);

	GUEST_DONE();
}

static void test_unaligned_slot(void)
{
	struct kvm_vcpu *vcpu;
	unsigned long *bmap;
	struct kvm_vm *vm;
	u64 gpa;

	vm = vm_create_with_one_vcpu(&vcpu, unaligned_guest_code);

	gpa = (vm_compute_max_gfn(vm) * vm->page_size) - 3 * BLOCK_SIZE;
	gpa = align_down(gpa, BLOCK_SIZE) + SZ_4K;

	/* Plain anonymous backing: the library disables THP on it. */
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, gpa, DATA_SLOT,
				    UNALIGNED_NPAGES, 0);
	virt_map(vm, TEST_GVA, gpa, UNALIGNED_NPAGES);
	vcpu_args_set(vcpu, 1, TEST_GVA);

	bmap = bitmap_zalloc(UNALIGNED_NPAGES);

	expect_sync(vcpu, STAGE_BLOCKS_READY);

	/* wp_range() starts at the unaligned slot base and meets an empty 2M block. */
	vm_mem_region_set_flags(vm, DATA_SLOT, KVM_MEM_LOG_DIRTY_PAGES);

	expect_sync(vcpu, STAGE_LOGGED_WRITES);

	kvm_vm_get_dirty_log(vm, DATA_SLOT, bmap);
	TEST_ASSERT_EQ(count_dirty(bmap, UNALIGNED_NPAGES), 1);
	TEST_ASSERT(test_bit(UNALIGNED_BLOCK1_OFF / SZ_4K, bmap),
		    "First page of the second block was not write-protected");

	vm_mem_region_set_flags(vm, DATA_SLOT, 0);
	expect_done(vcpu);

	free(bmap);
	kvm_vm_free(vm);
}

static bool hugepages_available(size_t bytes)
{
	void *mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
			-1, 0);

	if (mem == MAP_FAILED)
		return false;
	munmap(mem, bytes);
	return true;
}

int main(void)
{
	if (hugepages_available(NR_BLOCKS * BLOCK_SIZE)) {
		test_aligned_transitions();
		test_repeated_transitions();
		if (kvm_has_cap(KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2))
			test_manual_protect_transitions();
		else
			ksft_print_msg("SKIP manual protect: capability unsupported\n");
	} else {
		ksft_print_msg("SKIP hugetlb scenarios: need %d reserved 2M hugepages\n",
			       NR_BLOCKS);
	}

	test_unaligned_slot();

	ksft_print_msg("All ok!\n");
	return 0;
}
