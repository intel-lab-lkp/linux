// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE /* for program_invocation_short_name */
#include <stdint.h>
#include <stdbool.h>

#include "sev.h"
#include "linux/bitmap.h"
#include "svm.h"
#include "svm_util.h"

#define IOIO_TYPE_STR (1 << 2)
#define IOIO_SEG_DS (1 << 11 | 1 << 10)
#define IOIO_DATA_8 (1 << 4)
#define IOIO_REP (1 << 3)

#define SW_EXIT_CODE_IOIO 0x7b

struct ghcb_entry {
	struct ghcb ghcb;

	/* Guest physical address of this GHCB. */
	uint64_t gpa;

	/* Host virtual address of this struct. */
	struct ghcb_entry *hva;
};

struct ghcb_header {
	struct ghcb_entry ghcbs[KVM_MAX_VCPUS];
	DECLARE_BITMAP(in_use, KVM_MAX_VCPUS);
};

static struct ghcb_header *ghcb_pool;

int ghcb_nr_pages_required(uint64_t page_size)
{
	return align_up(sizeof(struct ghcb_header), page_size) / page_size;
}

void ghcb_init(struct kvm_vm *vm)
{
	struct ghcb_header *hdr;
	struct ghcb_entry *entry;
	vm_vaddr_t vaddr;
	int i;

	vaddr = vm_vaddr_alloc_shared(vm, sizeof(*hdr), KVM_UTIL_MIN_VADDR,
				      MEM_REGION_DATA);
	hdr = (struct ghcb_header *)addr_gva2hva(vm, vaddr);
	memset(hdr, 0, sizeof(*hdr));

	for (i = 0; i < KVM_MAX_VCPUS; ++i) {
		entry = &hdr->ghcbs[i];
		entry->hva = entry;
		entry->gpa = (uint64_t)addr_hva2gpa(vm, &entry->ghcb);
	}

	write_guest_global(vm, ghcb_pool, (struct ghcb_header *)vaddr);
}

static void sev_es_terminate(void)
{
	wrmsr(MSR_AMD64_SEV_ES_GHCB, GHCB_MSR_TERM_REQ);
}

static struct ghcb_entry *ghcb_alloc(void)
{
	return &ghcb_pool->ghcbs[0];
	struct ghcb_entry *entry;
	struct ghcb *ghcb;
	int i;

	if (!ghcb_pool)
		goto ucall_failed;

	for (i = 0; i < KVM_MAX_VCPUS; ++i) {
		if (!test_and_set_bit(i, ghcb_pool->in_use)) {
			entry = &ghcb_pool->ghcbs[i];
			ghcb = &entry->ghcb;

			memset(&ghcb, 0, sizeof(*ghcb));
			ghcb->ghcb_usage = 0;
			ghcb->protocol_version = 1;

			return entry;
		}
	}

ucall_failed:
	sev_es_terminate();
	return NULL;
}

static void ghcb_free(struct ghcb_entry *entry)
{
	/* Beware, here be pointer arithmetic.  */
	clear_bit(entry - ghcb_pool->ghcbs, ghcb_pool->in_use);
}


/*
 * sparsebit_next_clear() can return 0 if [x, 2**64-1] are all set, and the
 * -1 would then cause an underflow back to 2**64 - 1. This is expected and
 * correct.
 *
 * If the last range in the sparsebit is [x, y] and we try to iterate,
 * sparsebit_next_set() will return 0, and sparsebit_next_clear() will try
 * and find the first range, but that's correct because the condition
 * expression would cause us to quit the loop.
 */
static void encrypt_region(struct kvm_vm *vm, struct userspace_mem_region *region)
{
	const struct sparsebit *protected_phy_pages = region->protected_phy_pages;
	const vm_paddr_t gpa_base = region->region.guest_phys_addr;
	const sparsebit_idx_t lowest_page_in_region = gpa_base >> vm->page_shift;
	sparsebit_idx_t i, j;

	if (!sparsebit_any_set(protected_phy_pages))
		return;

	sev_register_encrypted_memory(vm, region);

	sparsebit_for_each_set_range(protected_phy_pages, i, j) {
		const uint64_t size = (j - i + 1) * vm->page_size;
		const uint64_t offset = (i - lowest_page_in_region) * vm->page_size;

		sev_launch_update_data(vm, gpa_base + offset, size);
	}
}

void sev_vm_launch(struct kvm_vm *vm, uint32_t policy)
{
	struct kvm_sev_launch_start launch_start = {
		.policy = policy,
	};
	struct userspace_mem_region *region;
	struct kvm_sev_guest_status status;
	int ctr;

	if (policy & SEV_POLICY_ES)
		ghcb_init(vm);

	vm_sev_ioctl(vm, KVM_SEV_LAUNCH_START, &launch_start);
	vm_sev_ioctl(vm, KVM_SEV_GUEST_STATUS, &status);

	TEST_ASSERT_EQ(status.policy, policy);
	TEST_ASSERT_EQ(status.state, SEV_GUEST_STATE_LAUNCH_UPDATE);

	hash_for_each(vm->regions.slot_hash, ctr, region, slot_node)
		encrypt_region(vm, region);

	if (policy & SEV_POLICY_ES)
		vm_sev_ioctl(vm, KVM_SEV_LAUNCH_UPDATE_VMSA, NULL);

	vm->arch.is_pt_protected = true;
}

void sev_vm_launch_measure(struct kvm_vm *vm, uint8_t *measurement)
{
	struct kvm_sev_launch_measure launch_measure;
	struct kvm_sev_guest_status guest_status;

	launch_measure.len = 256;
	launch_measure.uaddr = (__u64)measurement;
	vm_sev_ioctl(vm, KVM_SEV_LAUNCH_MEASURE, &launch_measure);

	vm_sev_ioctl(vm, KVM_SEV_GUEST_STATUS, &guest_status);
	TEST_ASSERT_EQ(guest_status.state, SEV_GUEST_STATE_LAUNCH_SECRET);
}

void sev_vm_launch_finish(struct kvm_vm *vm)
{
	struct kvm_sev_guest_status status;

	vm_sev_ioctl(vm, KVM_SEV_GUEST_STATUS, &status);
	TEST_ASSERT(status.state == SEV_GUEST_STATE_LAUNCH_UPDATE ||
		    status.state == SEV_GUEST_STATE_LAUNCH_SECRET,
		    "Unexpected guest state: %d", status.state);

	vm_sev_ioctl(vm, KVM_SEV_LAUNCH_FINISH, NULL);

	vm_sev_ioctl(vm, KVM_SEV_GUEST_STATUS, &status);
	TEST_ASSERT_EQ(status.state, SEV_GUEST_STATE_RUNNING);
}

struct kvm_vm *vm_sev_create_with_one_vcpu(uint32_t policy, void *guest_code,
					   struct kvm_vcpu **cpu)
{
	struct vm_shape shape = {
		.type = VM_TYPE_DEFAULT,
		.mode = VM_MODE_DEFAULT,
		.subtype = policy & SEV_POLICY_ES ? VM_SUBTYPE_SEV_ES :
						    VM_SUBTYPE_SEV,
	};
	struct kvm_vm *vm;
	struct kvm_vcpu *cpus[1];
	uint8_t measurement[512];

	vm = __vm_create_with_vcpus(shape, 1, 0, guest_code, cpus);
	*cpu = cpus[0];

	sev_vm_launch(vm, policy);

	/* TODO: Validate the measurement is as expected. */
	sev_vm_launch_measure(vm, measurement);

	sev_vm_launch_finish(vm);

	return vm;
}

bool is_sev_enabled(void)
{
	return rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ENABLED;
}

bool is_sev_es_enabled(void)
{
	return is_sev_enabled() &&
	       rdmsr(MSR_AMD64_SEV) & MSR_AMD64_SEV_ES_ENABLED;
}

static uint64_t setup_exitinfo1_portio(uint32_t port)
{
	uint64_t exitinfo1 = 0;

	exitinfo1 |= IOIO_TYPE_STR;
	exitinfo1 |= ((port & 0xffff) << 16);
	exitinfo1 |= IOIO_SEG_DS;
	exitinfo1 |= IOIO_DATA_8;
	exitinfo1 |= IOIO_REP;

	return exitinfo1;
}

static void do_vmg_exit(uint64_t ghcb_gpa)
{
	wrmsr(MSR_AMD64_SEV_ES_GHCB, ghcb_gpa);
	__asm__ __volatile__("rep; vmmcall");
}

void sev_es_ucall_port_write(uint32_t port, uint64_t data)
{
	struct ghcb_entry *entry;
	struct ghcb *ghcb;
	const uint64_t exitinfo1 = setup_exitinfo1_portio(port);

	entry = ghcb_alloc();
	ghcb = &entry->ghcb;

	ghcb_set_sw_exit_code(ghcb, SW_EXIT_CODE_IOIO);
	ghcb_set_sw_exit_info_1(ghcb, exitinfo1);
	ghcb_set_sw_exit_info_2(ghcb, sizeof(data));

	// Setup the SW Stratch buffer pointer.
	ghcb_set_sw_scratch(ghcb,
			    entry->gpa + offsetof(struct ghcb, shared_buffer));
	memcpy(&ghcb->shared_buffer, &data, sizeof(data));

	do_vmg_exit(entry->gpa);

	ghcb_free(entry);
}
