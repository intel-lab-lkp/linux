// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test reset dirty ring on gmem slot on x86.
 * Copyright (C) 2025, Intel, Inc.
 *
 * The slot flag KVM_MEM_GUEST_MEMFD is incompatible with the flag
 * KVM_MEM_LOG_DIRTY_PAGES, which means KVM does not permit dirty page tracking
 * on gmem slots.
 *
 * When dirty ring is enabled, although KVM does not mark GFNs in gmem slots as
 * dirty, userspace can reset and write arbitrary data into the dirty ring
 * entries shared between KVM and userspace. This can lead KVM to incorrectly
 * clear write permission or dirty bits on SPTEs of gmem slots.
 *
 * While this might be harmless for non-TDX VMs, it could cause inconsistencies
 * between the mirror SPTEs and the external SPTEs in hardware, or even trigger
 * KVM_BUG_ON() for TDX.
 *
 * Purposely reset dirty ring incorrectly on gmem slots (gmem slots do not allow
 * dirty page tracking) to verify malbehaved userspace cannot cause any SPTE
 * permission reduction change.
 *
 * Steps conducted in this test:
 * 1. echo nop > ${TRACING_ROOT}/current_tracer
 *    echo 1 > ${TRACING_ROOT}/events/kvmmmu/kvm_tdp_mmu_spte_changed/enable
 *    echo > ${TRACING_ROOT}/set_event_pid
 *    echo > ${TRACING_ROOT}/set_event_notrace_pid
 *
 * 2. echo "common_pid == $tid && gfn == 0xc0400" > \
 *    ${TRACING_ROOT}/events/kvmmmu/kvm_tdp_mmu_spte_changed/filter
 *
 * 3. echo 0 > ${TRACING_ROOT}/tracing_on
 *    echo > ${TRACING_ROOT}/trace
 *    echo 1 > ${TRACING_ROOT}/tracing_on
 *
 * 4. purposely reset dirty ring incorrectly
 *
 * 5. cat ${TRACING_ROOT}/trace
 */
#include <linux/kvm.h>
#include <asm/barrier.h>
#include <test_util.h>
#include <kvm_util.h>
#include <processor.h>

#define DEBUGFS "/sys/kernel/debug/tracing"
#define TRACEFS "/sys/kernel/tracing"

#define TEST_DIRTY_RING_GPA (0xc0400000)
#define TEST_DIRTY_RING_GVA (0x90400000)
#define TEST_DIRTY_RING_REGION_SLOT 11
#define TEST_DIRTY_RING_REGION_SIZE 0x200000
#define TEST_DIRTY_RING_COUNT 4096
#define TEST_DIRTY_RING_GUEST_WRITE_MAX_CNT 3

static const char *PATTEN = "spte_changed";
static char *tracing_root;

static int open_path(char *subpath, int flags)
{
	static char path[100];
	int count, fd;

	count = snprintf(path, sizeof(path), "%s/%s", tracing_root, subpath);
	TEST_ASSERT(count > 0, "Incorrect path\n");
	fd = open(path, flags);
	TEST_ASSERT(fd >= 0, "Cannot open %s\n", path);

	return fd;
}

static void setup_tracing(void)
{
	int fd;

	/* set current_tracer to nop */
	fd = open_path("current_tracer", O_WRONLY);
	test_write(fd, "nop\n", 4);
	close(fd);

	/* turn on event kvm_tdp_mmu_spte_changed */
	fd = open_path("events/kvmmmu/kvm_tdp_mmu_spte_changed/enable", O_WRONLY);
	test_write(fd, "1\n", 2);
	close(fd);

	/* clear set_event_pid & set_event_notrace_pid */
	fd = open_path("set_event_pid", O_WRONLY | O_TRUNC);
	close(fd);

	fd = open_path("set_event_notrace_pid", O_WRONLY | O_TRUNC);
	close(fd);
}

static void filter_event(void)
{
	int count, fd;
	char buf[100];

	fd = open_path("events/kvmmmu/kvm_tdp_mmu_spte_changed/filter",
		       O_WRONLY | O_TRUNC);

	count = snprintf(buf, sizeof(buf), "common_pid == %d && gfn == 0x%x\n",
			 gettid(), TEST_DIRTY_RING_GPA >> PAGE_SHIFT);
	TEST_ASSERT(count > 0, "Incorrect number of data written\n");
	test_write(fd, buf, count);
	close(fd);
}

static void enable_tracing(bool enable)
{
	char *val = enable ? "1\n" : "0\n";
	int fd;

	if (enable) {
		/* clear trace log before enabling */
		fd = open_path("trace", O_WRONLY | O_TRUNC);
		close(fd);
	}

	fd = open_path("tracing_on", O_WRONLY);
	test_write(fd, val, 2);
	close(fd);
}

static void reset_tracing(void)
{
	enable_tracing(false);
	enable_tracing(true);
}

static void detect_spte_change(void)
{
	static char buf[1024];
	FILE *file;
	int count;

	count = snprintf(buf, sizeof(buf), "%s/trace", tracing_root);
	TEST_ASSERT(count > 0, "Incorrect path\n");
	file = fopen(buf, "r");
	TEST_ASSERT(file, "Cannot open %s\n", buf);

	while (fgets(buf, sizeof(buf), file))
		TEST_ASSERT(!strstr(buf, PATTEN), "Unexpected SPTE change %s\n", buf);

	fclose(file);
}

/*
 * Write to a gmem slot and exit to host after each write to allow host to check
 * dirty ring.
 */
void guest_code(void)
{
	uint64_t count = 0;

	while (count < TEST_DIRTY_RING_GUEST_WRITE_MAX_CNT) {
		count++;
		memset((void *)TEST_DIRTY_RING_GVA, 1, 8);
		GUEST_SYNC(count);
	}
	GUEST_DONE();
}

/*
 * Verify that KVM_MEM_LOG_DIRTY_PAGES cannot be set on a memslot with flag
 * KVM_MEM_GUEST_MEMFD.
 */
static void verify_turn_on_log_dirty_pages_flag(struct kvm_vcpu *vcpu)
{
	struct userspace_mem_region *region;
	int ret;

	region = memslot2region(vcpu->vm, TEST_DIRTY_RING_REGION_SLOT);
	region->region.flags |= KVM_MEM_LOG_DIRTY_PAGES;

	ret = __vm_ioctl(vcpu->vm, KVM_SET_USER_MEMORY_REGION2, &region->region);

	TEST_ASSERT(ret, "KVM_SET_USER_MEMORY_REGION2 incorrectly succeeds\n");
	region->region.flags &= ~KVM_MEM_LOG_DIRTY_PAGES;
}

static inline bool dirty_gfn_is_dirtied(struct kvm_dirty_gfn *gfn)
{
	return smp_load_acquire(&gfn->flags) == KVM_DIRTY_GFN_F_DIRTY;
}

static inline void dirty_gfn_set_collected(struct kvm_dirty_gfn *gfn)
{
	smp_store_release(&gfn->flags, KVM_DIRTY_GFN_F_RESET);
}

static bool dirty_ring_empty(struct kvm_vcpu *vcpu)
{
	struct kvm_dirty_gfn *dirty_gfns = vcpu_map_dirty_ring(vcpu);
	struct kvm_dirty_gfn *cur;
	int i;

	for (i = 0; i < TEST_DIRTY_RING_COUNT; i++) {
		cur = &dirty_gfns[i];

		if (dirty_gfn_is_dirtied(cur))
			return false;
	}
	return true;
}

/*
 * Purposely reset the dirty ring incorrectly by resetting a dirty ring entry
 * even when KVM does not report the entry as dirty.
 *
 * In the kvm_dirty_gfn entry, specify the slot to the gmem slot that does not
 * allow dirty page tracking and has no flag KVM_MEM_LOG_DIRTY_PAGES.
 */
static void reset_dirty_ring(struct kvm_vcpu *vcpu, int *reset_index)
{
	struct kvm_dirty_gfn *dirty_gfns = vcpu_map_dirty_ring(vcpu);
	struct kvm_dirty_gfn *cur = &dirty_gfns[*reset_index];
	uint32_t cleared;

	reset_tracing();

	cur->slot = TEST_DIRTY_RING_REGION_SLOT;
	cur->offset = 0;
	dirty_gfn_set_collected(cur);
	cleared = kvm_vm_reset_dirty_ring(vcpu->vm);
	*reset_index += cleared;
	TEST_ASSERT(cleared == 1, "Unexpected cleared count %d\n", cleared);

	detect_spte_change();
}

/*
 * The vCPU worker to loop vcpu_run(). After each vCPU access to a GFN, check if
 * the dirty ring is empty and reset the dirty ring.
 */
static void reset_dirty_ring_worker(struct kvm_vcpu *vcpu)
{
	struct kvm_run *run = vcpu->run;
	struct ucall uc;
	uint64_t cmd;
	int index = 0;

	filter_event();
	while (1) {
		vcpu_run(vcpu);

		if (run->exit_reason == KVM_EXIT_IO) {
			cmd = get_ucall(vcpu, &uc);
			if (cmd != UCALL_SYNC)
				break;

			TEST_ASSERT(dirty_ring_empty(vcpu),
				    "Guest write should not cause GFN dirty\n");

			reset_dirty_ring(vcpu, &index);
		}
	}
}

static struct kvm_vm *create_vm(unsigned long vm_type, struct kvm_vcpu **vcpu,
				bool private)
{
	unsigned int npages = TEST_DIRTY_RING_REGION_SIZE / getpagesize();
	const struct vm_shape shape = {
		.mode = VM_MODE_DEFAULT,
		.type = vm_type,
	};
	struct kvm_vm *vm;

	vm = __vm_create(shape, 1, 0);
	vm_enable_dirty_ring(vm, TEST_DIRTY_RING_COUNT * sizeof(struct kvm_dirty_gfn));
	*vcpu = vm_vcpu_add(vm, 0, guest_code);
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS,
				    TEST_DIRTY_RING_GPA,
				    TEST_DIRTY_RING_REGION_SLOT,
				    npages, KVM_MEM_GUEST_MEMFD);
	vm->memslots[MEM_REGION_TEST_DATA] = TEST_DIRTY_RING_REGION_SLOT;
	virt_map(vm, TEST_DIRTY_RING_GVA, TEST_DIRTY_RING_GPA, npages);
	if (private)
		vm_mem_set_private(vm, TEST_DIRTY_RING_GPA,
				   TEST_DIRTY_RING_REGION_SIZE);
	return vm;
}

struct test_config {
	unsigned long vm_type;
	bool manual_protect_and_init_set;
	bool private_access;
	char *test_desc;
};

void test_dirty_ring_on_gmem_slot(struct test_config *config)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	if (config->vm_type &&
	    !(kvm_check_cap(KVM_CAP_VM_TYPES) & BIT(config->vm_type))) {
		ksft_test_result_skip("\n");
		return;
	}

	vm = create_vm(config->vm_type, &vcpu, config->private_access);

	/*
	 * Let KVM detect that kvm_dirty_log_manual_protect_and_init_set() is
	 * true in kvm_arch_mmu_enable_log_dirty_pt_masked() to check if
	 * kvm_mmu_slot_gfn_write_protect() will be called on a gmem memslot.
	 */
	if (config->manual_protect_and_init_set) {
		u64 manual_caps;

		manual_caps = kvm_check_cap(KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2);

		manual_caps &= (KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE |
				KVM_DIRTY_LOG_INITIALLY_SET);

		if (!manual_caps)
			return;

		vm_enable_cap(vm, KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2, manual_caps);
	}

	verify_turn_on_log_dirty_pages_flag(vcpu);

	reset_dirty_ring_worker(vcpu);

	kvm_vm_free(vm);
	ksft_test_result_pass("\n");
}

static bool dirty_ring_supported(void)
{
	return (kvm_has_cap(KVM_CAP_DIRTY_LOG_RING) ||
		kvm_has_cap(KVM_CAP_DIRTY_LOG_RING_ACQ_REL));
}

static bool has_tracing(void)
{
	if (faccessat(AT_FDCWD, DEBUGFS, F_OK, AT_EACCESS) == 0) {
		tracing_root = DEBUGFS;
		return true;
	}

	if (faccessat(AT_FDCWD, TRACEFS, F_OK, AT_EACCESS) == 0) {
		tracing_root = TRACEFS;
		return true;
	}

	return false;
}

static struct test_config tests[] = {
	{
		.vm_type = KVM_X86_SW_PROTECTED_VM,
		.manual_protect_and_init_set = false,
		.private_access = true,
		.test_desc = "SW_PROTECTED_VM, manual_protect_and_init_set=false, private access",
	},
	{
		.vm_type = KVM_X86_SW_PROTECTED_VM,
		.manual_protect_and_init_set = true,
		.private_access = true,
		.test_desc = "SW_PROTECTED_VM, manual_protect_and_init_set=true, private access",
	},
};

int main(int argc, char **argv)
{
	int test_cnt = ARRAY_SIZE(tests);

	ksft_print_header();
	ksft_set_plan(test_cnt);

	TEST_REQUIRE(get_kvm_param_bool("tdp_mmu"));
	TEST_REQUIRE(has_tracing());
	TEST_REQUIRE(dirty_ring_supported());

	setup_tracing();

	for (int i = 0; i < test_cnt; i++) {
		pthread_t vm_thread;

		pthread_create(&vm_thread, NULL,
			       (void *(*)(void *))test_dirty_ring_on_gmem_slot,
			       &tests[i]);
		pthread_join(vm_thread, NULL);
	}

	ksft_finished();
	return 0;
}
