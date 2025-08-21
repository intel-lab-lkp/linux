// SPDX-License-Identifier: GPL-2.0-only

#include <stdint.h>

#include "kvm_util.h"
#include "processor.h"
#include "tdx/td_boot.h"
#include "tdx/td_boot_asm.h"
#include "tdx/tdx_util.h"

/* Arbitrarily selected to avoid overlaps with anything else */
#define TD_BOOT_CODE_SLOT	20
#define TD_BOOT_PARAMETERS_SLOT	21

#define X86_RESET_VECTOR	0xfffffff0ul
#define X86_RESET_VECTOR_SIZE	16

void vm_tdx_setup_boot_code_region(struct kvm_vm *vm)
{
	size_t total_code_size = TD_BOOT_CODE_SIZE + X86_RESET_VECTOR_SIZE;
	vm_paddr_t boot_code_gpa = X86_RESET_VECTOR - TD_BOOT_CODE_SIZE;
	vm_paddr_t alloc_gpa = round_down(boot_code_gpa, PAGE_SIZE);
	size_t nr_pages = DIV_ROUND_UP(total_code_size, PAGE_SIZE);
	vm_paddr_t gpa;
	uint8_t *hva;

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    alloc_gpa,
				    TD_BOOT_CODE_SLOT, nr_pages,
				    KVM_MEM_GUEST_MEMFD);

	gpa = vm_phy_pages_alloc(vm, nr_pages, alloc_gpa, TD_BOOT_CODE_SLOT);
	TEST_ASSERT(gpa == alloc_gpa, "Failed vm_phy_pages_alloc\n");

	virt_map(vm, alloc_gpa, alloc_gpa, nr_pages);
	hva = addr_gpa2hva(vm, boot_code_gpa);
	memcpy(hva, td_boot, TD_BOOT_CODE_SIZE);

	hva += TD_BOOT_CODE_SIZE;
	TEST_ASSERT(hva == addr_gpa2hva(vm, X86_RESET_VECTOR),
		    "Expected RESET vector at hva 0x%lx, got %lx",
		    (unsigned long)addr_gpa2hva(vm, X86_RESET_VECTOR), (unsigned long)hva);

	/*
	 * Handcode "JMP rel8" at the RESET vector to jump back to the TD boot
	 * code, as there are only 16 bytes at the RESET vector before RIP will
	 * wrap back to zero.  Insert a trailing int3 so that the vCPU crashes
	 * in case the JMP somehow falls through.  Note!  The target address is
	 * relative to the end of the instruction!
	 */
	TEST_ASSERT(TD_BOOT_CODE_SIZE < 256,
		    "TD boot code not addressable by 'JMP rel8'");
	hva[0] = 0xeb;
	hva[1] = 256 - 2 - TD_BOOT_CODE_SIZE;
	hva[2] = 0xcc;
}

void vm_tdx_setup_boot_parameters_region(struct kvm_vm *vm, uint32_t nr_runnable_vcpus)
{
	size_t boot_params_size =
		sizeof(struct td_boot_parameters) +
		nr_runnable_vcpus * sizeof(struct td_per_vcpu_parameters);
	int npages = DIV_ROUND_UP(boot_params_size, PAGE_SIZE);
	vm_paddr_t gpa;

	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    TD_BOOT_PARAMETERS_GPA,
				    TD_BOOT_PARAMETERS_SLOT, npages,
				    KVM_MEM_GUEST_MEMFD);
	gpa = vm_phy_pages_alloc(vm, npages, TD_BOOT_PARAMETERS_GPA, TD_BOOT_PARAMETERS_SLOT);
	TEST_ASSERT(gpa == TD_BOOT_PARAMETERS_GPA, "Failed vm_phy_pages_alloc\n");

	virt_map(vm, TD_BOOT_PARAMETERS_GPA, TD_BOOT_PARAMETERS_GPA, npages);
}

void vm_tdx_load_common_boot_parameters(struct kvm_vm *vm)
{
	struct td_boot_parameters *params =
		addr_gpa2hva(vm, TD_BOOT_PARAMETERS_GPA);
	uint32_t cr4;

	TEST_ASSERT_EQ(vm->mode, VM_MODE_PXXV48_4K);

	cr4 = kvm_get_default_cr4();

	/* TDX spec 11.6.2: CR4 bit MCE is fixed to 1 */
	cr4 |= X86_CR4_MCE;

	/* Set this because UEFI also sets this up, to handle XMM exceptions */
	cr4 |= X86_CR4_OSXMMEXCPT;

	/* TDX spec 11.6.2: CR4 bit VMXE and SMXE are fixed to 0 */
	cr4 &= ~(X86_CR4_VMXE | X86_CR4_SMXE);

	/* Set parameters! */
	params->cr0 = kvm_get_default_cr0();
	params->cr3 = vm->pgd;
	params->cr4 = cr4;
	params->idtr.base = vm->arch.idt;
	params->idtr.limit = kvm_get_default_idt_limit();
	params->gdtr.base = vm->arch.gdt;
	params->gdtr.limit = kvm_get_default_gdt_limit();

	TEST_ASSERT(params->cr0 != 0, "cr0 should not be 0");
	TEST_ASSERT(params->cr3 != 0, "cr3 should not be 0");
	TEST_ASSERT(params->cr4 != 0, "cr4 should not be 0");
	TEST_ASSERT(params->gdtr.base != 0, "gdt base address should not be 0");
	TEST_ASSERT(params->idtr.base != 0, "idt base address should not be 0");
}

void vm_tdx_load_vcpu_boot_parameters(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	struct td_boot_parameters *params =
		addr_gpa2hva(vm, TD_BOOT_PARAMETERS_GPA);
	struct td_per_vcpu_parameters *vcpu_params =
		&params->per_vcpu[vcpu->id];

	vcpu_params->esp_gva = kvm_allocate_vcpu_stack(vm);
}

void vm_tdx_set_vcpu_entry_point(struct kvm_vcpu *vcpu, void *guest_code)
{
	struct td_boot_parameters *params = addr_gpa2hva(vcpu->vm, TD_BOOT_PARAMETERS_GPA);
	struct td_per_vcpu_parameters *vcpu_params = &params->per_vcpu[vcpu->id];

	vcpu_params->guest_code = (uint64_t)guest_code;
}

static struct kvm_tdx_capabilities *tdx_read_capabilities(struct kvm_vm *vm)
{
	struct kvm_tdx_capabilities *tdx_cap = NULL;
	int nr_cpuid_configs = 4;
	int rc = -1;
	int i;

	do {
		nr_cpuid_configs *= 2;

		tdx_cap = realloc(tdx_cap, sizeof(*tdx_cap) +
					   sizeof(tdx_cap->cpuid) +
					   (sizeof(struct kvm_cpuid_entry2) * nr_cpuid_configs));
		TEST_ASSERT(tdx_cap,
			    "Could not allocate memory for tdx capability nr_cpuid_configs %d\n",
			    nr_cpuid_configs);

		tdx_cap->cpuid.nent = nr_cpuid_configs;
		rc = __vm_tdx_vm_ioctl(vm, KVM_TDX_CAPABILITIES, 0, tdx_cap);
	} while (rc < 0 && errno == E2BIG);

	TEST_ASSERT(rc == 0, "KVM_TDX_CAPABILITIES failed: %d %d",
		    rc, errno);

	pr_debug("tdx_cap: supported_attrs: 0x%016llx\n"
		 "tdx_cap: supported_xfam 0x%016llx\n",
		 tdx_cap->supported_attrs, tdx_cap->supported_xfam);

	for (i = 0; i < tdx_cap->cpuid.nent; i++) {
		const struct kvm_cpuid_entry2 *config = &tdx_cap->cpuid.entries[i];

		pr_debug("cpuid config[%d]: leaf 0x%x sub_leaf 0x%x eax 0x%08x ebx 0x%08x ecx 0x%08x edx 0x%08x\n",
			 i, config->function, config->index,
			 config->eax, config->ebx, config->ecx, config->edx);
	}

	return tdx_cap;
}

static struct kvm_cpuid_entry2 *tdx_find_cpuid_config(struct kvm_tdx_capabilities *cap,
						      uint32_t leaf, uint32_t sub_leaf)
{
	struct kvm_cpuid_entry2 *config;
	uint32_t i;

	for (i = 0; i < cap->cpuid.nent; i++) {
		config = &cap->cpuid.entries[i];

		if (config->function == leaf && config->index == sub_leaf)
			return config;
	}

	return NULL;
}

/*
 * Filter CPUID based on TDX supported capabilities
 *
 * Input Args:
 *   vm - Virtual Machine
 *   cpuid_data - CPUID fileds to filter
 *
 * Output Args: None
 *
 * Return: None
 *
 * For each CPUID leaf, filter out non-supported bits based on the capabilities reported
 * by the TDX module
 */
static void vm_tdx_filter_cpuid(struct kvm_vm *vm,
				struct kvm_cpuid2 *cpuid_data)
{
	struct kvm_tdx_capabilities *tdx_cap;
	struct kvm_cpuid_entry2 *config;
	struct kvm_cpuid_entry2 *e;
	int i;

	tdx_cap = tdx_read_capabilities(vm);

	i = 0;
	while (i < cpuid_data->nent) {
		e = cpuid_data->entries + i;
		config = tdx_find_cpuid_config(tdx_cap, e->function, e->index);

		if (!config) {
			int left = cpuid_data->nent - i - 1;

			if (left > 0)
				memmove(cpuid_data->entries + i,
					cpuid_data->entries + i + 1,
					sizeof(*cpuid_data->entries) * left);
			cpuid_data->nent--;
			continue;
		}

		e->eax &= config->eax;
		e->ebx &= config->ebx;
		e->ecx &= config->ecx;
		e->edx &= config->edx;

		i++;
	}

	free(tdx_cap);
}

static void tdx_check_attributes(struct kvm_vm *vm, uint64_t attributes)
{
	struct kvm_tdx_capabilities *tdx_cap;

	tdx_cap = tdx_read_capabilities(vm);

	/* TDX spec: any bits 0 in supported_attrs must be 0 in attributes */
	TEST_ASSERT_EQ(attributes & ~tdx_cap->supported_attrs, 0);

	/* TDX spec: any bits 1 in attributes must be 1 in supported_attrs */
	TEST_ASSERT_EQ(attributes & tdx_cap->supported_attrs, attributes);

	free(tdx_cap);
}

void vm_tdx_init_vm(struct kvm_vm *vm, uint64_t attributes)
{
	struct kvm_tdx_init_vm *init_vm;
	const struct kvm_cpuid2 *tmp;
	struct kvm_cpuid2 *cpuid;

	tmp = kvm_get_supported_cpuid();

	cpuid = allocate_kvm_cpuid2(MAX_NR_CPUID_ENTRIES);
	memcpy(cpuid, tmp, kvm_cpuid2_size(tmp->nent));
	vm_tdx_filter_cpuid(vm, cpuid);

	init_vm = calloc(1, sizeof(*init_vm) +
			 sizeof(init_vm->cpuid.entries[0]) * cpuid->nent);
	TEST_ASSERT(init_vm, "init_vm allocation failed");

	memcpy(&init_vm->cpuid, cpuid, kvm_cpuid2_size(cpuid->nent));
	free(cpuid);

	tdx_check_attributes(vm, attributes);

	init_vm->attributes = attributes;

	vm_tdx_vm_ioctl(vm, KVM_TDX_INIT_VM, 0, init_vm);

	free(init_vm);
}

static void tdx_init_mem_region(struct kvm_vm *vm, void *source_pages,
				uint64_t gpa, uint64_t size)
{
	uint32_t metadata = KVM_TDX_MEASURE_MEMORY_REGION;
	struct kvm_tdx_init_mem_region mem_region = {
		.source_addr = (uint64_t)source_pages,
		.gpa = gpa,
		.nr_pages = size / PAGE_SIZE,
	};
	struct kvm_vcpu *vcpu;

	vcpu = list_first_entry_or_null(&vm->vcpus, struct kvm_vcpu, list);

	TEST_ASSERT((mem_region.nr_pages > 0) &&
		    ((mem_region.nr_pages * PAGE_SIZE) == size),
		    "Cannot add partial pages to the guest memory.\n");
	TEST_ASSERT(((uint64_t)source_pages & (PAGE_SIZE - 1)) == 0,
		    "Source memory buffer is not page aligned\n");
	vm_tdx_vcpu_ioctl(vcpu, KVM_TDX_INIT_MEM_REGION, metadata, &mem_region);
}

static void tdx_init_pages(struct kvm_vm *vm, void *hva, uint64_t gpa,
			   uint64_t size)
{
	void *scratch_page = calloc(1, PAGE_SIZE);
	uint64_t nr_pages = size / PAGE_SIZE;
	int i;

	TEST_ASSERT(scratch_page,
		    "Could not allocate memory for loading memory region");

	for (i = 0; i < nr_pages; i++) {
		memcpy(scratch_page, hva, PAGE_SIZE);

		tdx_init_mem_region(vm, scratch_page, gpa, PAGE_SIZE);

		hva += PAGE_SIZE;
		gpa += PAGE_SIZE;
	}

	free(scratch_page);
}

static void load_td_private_memory(struct kvm_vm *vm)
{
	struct userspace_mem_region *region;
	int ctr;

	hash_for_each(vm->regions.slot_hash, ctr, region, slot_node) {
		const struct sparsebit *protected_pages = region->protected_phy_pages;
		const vm_paddr_t gpa_base = region->region.guest_phys_addr;
		const uint64_t hva_base = region->region.userspace_addr;
		const sparsebit_idx_t lowest_page_in_region = gpa_base >> vm->page_shift;

		sparsebit_idx_t i;
		sparsebit_idx_t j;

		if (!sparsebit_any_set(protected_pages))
			continue;

		sparsebit_for_each_set_range(protected_pages, i, j) {
			const uint64_t size_to_load = (j - i + 1) * vm->page_size;
			const uint64_t offset =
				(i - lowest_page_in_region) * vm->page_size;
			const uint64_t hva = hva_base + offset;
			const uint64_t gpa = gpa_base + offset;

			vm_set_memory_attributes(vm, gpa, size_to_load,
						 KVM_MEMORY_ATTRIBUTE_PRIVATE);

			/*
			 * Here, memory is being loaded from hva to gpa. If the memory
			 * mapped to hva is also used to back gpa, then a copy has to be
			 * made just for loading, since KVM_TDX_INIT_MEM_REGION ioctl
			 * cannot encrypt memory in place.
			 *
			 * To determine if memory mapped to hva is also used to back
			 * gpa, use a heuristic:
			 *
			 * If this memslot has guest_memfd, then this memslot should
			 * have memory backed from two sources: hva for shared memory
			 * and gpa will be backed by guest_memfd.
			 */
			if (region->region.guest_memfd == -1)
				tdx_init_pages(vm, (void *)hva, gpa, size_to_load);
			else
				tdx_init_mem_region(vm, (void *)hva, gpa, size_to_load);
		}
	}
}

void vm_tdx_finalize(struct kvm_vm *vm)
{
	load_td_private_memory(vm);
	vm_tdx_vm_ioctl(vm, KVM_TDX_FINALIZE_VM, 0, NULL);
}

struct kvm_vm *vm_tdx_create_with_one_vcpu(void *guest_code,
					   struct kvm_vcpu **vcpu)
{
	struct vm_shape shape = {
		.mode = VM_MODE_DEFAULT,
		.type = KVM_X86_TDX_VM,
	};
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpus[1];

	vm = __vm_create_with_vcpus(shape, 1, 0, guest_code, vcpus);
	*vcpu = vcpus[0];

	vm_tdx_finalize(vm);

	return vm;
}
