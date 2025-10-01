// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/kvm/lib/x86_64/nested_map.c
 *
 * Copyright (C) 2025, Google LLC.
 */

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "nested_map.h"
#include "vmx.h"

static uint64_t nested_create_pte(struct kvm_vm *vm,
				  uint64_t *pte,
				  uint64_t nested_paddr,
				  uint64_t paddr,
				  int level,
				  bool want_leaf)
{
	bool leaf = want_leaf;
	uint64_t address;

	if (!nested_ept_create_pte(vm, pte, paddr, &address, &leaf)) {
		TEST_ASSERT(!want_leaf,
			    "Cannot create leaf entry at level: %u, nested_paddr: 0x%lx",
			    level, nested_paddr);
		TEST_ASSERT(!leaf,
			    "Leaf entry already exists at level: %u, nested_paddr: 0x%lx",
			    level, nested_paddr);
	}
	return address;
}

static void nested_pg_map(void *root_hva, struct kvm_vm *vm, uint64_t
			  nested_paddr, uint64_t paddr, int target_level)
{
	const uint64_t page_size = PG_LEVEL_SIZE(target_level);
	uint64_t *pt = root_hva, *pte;
	uint16_t index, address;
	bool leaf;

	TEST_ASSERT(vm->mode == VM_MODE_PXXV48_4K, "Attempt to use "
		    "unknown or unsupported guest mode, mode: 0x%x", vm->mode);

	TEST_ASSERT((nested_paddr >> 48) == 0,
		    "Nested physical address 0x%lx requires 5-level paging",
		    nested_paddr);
	TEST_ASSERT((nested_paddr % page_size) == 0,
		    "Nested physical address not on page boundary,\n"
		    "  nested_paddr: 0x%lx page_size: 0x%lx",
		    nested_paddr, page_size);
	TEST_ASSERT((nested_paddr >> vm->page_shift) <= vm->max_gfn,
		    "Physical address beyond beyond maximum supported,\n"
		    "  nested_paddr: 0x%lx vm->max_gfn: 0x%lx vm->page_size: 0x%x",
		    paddr, vm->max_gfn, vm->page_size);
	TEST_ASSERT((paddr % page_size) == 0,
		    "Physical address not on page boundary,\n"
		    "  paddr: 0x%lx page_size: 0x%lx",
		    paddr, page_size);
	TEST_ASSERT((paddr >> vm->page_shift) <= vm->max_gfn,
		    "Physical address beyond beyond maximum supported,\n"
		    "  paddr: 0x%lx vm->max_gfn: 0x%lx vm->page_size: 0x%x",
		    paddr, vm->max_gfn, vm->page_size);

	for (int level = PG_LEVEL_512G; level >= PG_LEVEL_4K; level--) {
		index = (nested_paddr >> PG_LEVEL_SHIFT(level)) & 0x1ffu;
		pte = &pt[index];
		leaf = (level == target_level);

		address = nested_create_pte(vm, pte, nested_paddr, paddr, level, leaf);

		if (leaf)
			break;

		pt = addr_gpa2hva(vm, address * vm->page_size);
	}

}

/*
 * Map a range of EPT guest physical addresses to the VM's physical address
 *
 * Input Args:
 *   vm - Virtual Machine
 *   nested_paddr - Nested guest physical address to map
 *   paddr - VM Physical Address
 *   size - The size of the range to map
 *   level - The level at which to map the range
 *
 * Output Args: None
 *
 * Return: None
 *
 * Within the VM given by vm, creates a nested guest translation for the
 * page range starting at nested_paddr to the page range starting at paddr.
 */
static void __nested_map(void *root_hva, struct kvm_vm *vm, uint64_t
			 nested_paddr, uint64_t paddr, uint64_t size, int level)
{
	size_t page_size = PG_LEVEL_SIZE(level);
	size_t npages = size / page_size;

	TEST_ASSERT(nested_paddr + size > nested_paddr, "Vaddr overflow");
	TEST_ASSERT(paddr + size > paddr, "Paddr overflow");

	while (npages--) {
		nested_pg_map(root_hva, vm, nested_paddr, paddr, level);
		nested_paddr += page_size;
		paddr += page_size;
	}
}

void nested_map(void *root_hva, struct kvm_vm *vm,
		uint64_t nested_paddr, uint64_t paddr, uint64_t size)
{
	__nested_map(root_hva, vm, nested_paddr, paddr, size, PG_LEVEL_4K);
}

/*
 * Prepare an identity nested page table that maps all the
 * physical pages in VM.
 */
void nested_map_memslot(void *root_hva, struct kvm_vm *vm,
			uint32_t memslot)
{
	sparsebit_idx_t i, last;
	struct userspace_mem_region *region =
		memslot2region(vm, memslot);

	i = (region->region.guest_phys_addr >> vm->page_shift) - 1;
	last = i + (region->region.memory_size >> vm->page_shift);
	for (;;) {
		i = sparsebit_next_clear(region->unused_phy_pages, i);
		if (i > last)
			break;

		nested_map(root_hva, vm,
			   (uint64_t)i << vm->page_shift,
			   (uint64_t)i << vm->page_shift,
			   1 << vm->page_shift);
	}
}

/* Identity map a region with 1GiB Pages. */
void nested_identity_map_1g(void *root_hva, struct kvm_vm *vm,
			    uint64_t addr, uint64_t size)
{
	__nested_map(root_hva, vm, addr, addr, size, PG_LEVEL_1G);
}
