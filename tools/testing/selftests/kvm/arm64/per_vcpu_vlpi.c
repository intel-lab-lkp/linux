// SPDX-License-Identifier: GPL-2.0
/*
 * Test per-vCPU vLPI enable/disable/query correctness
 */

#include <linux/kvm.h>
#include <pthread.h>
#include <sys/resource.h>
#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "gic.h"
#include "vgic.h"
#include "../kselftest_harness.h"

static int MAX_VCPUS;
static int ITS_MAX_VPEID;

/* Dynamically fetch MAX_VCPUS and ITS_MAX_VPEID values */
__attribute__((constructor))
static void init_test_limits(void)
{
	int kvm_fd = open("/dev/kvm", O_RDWR);
	int max_vcpus, max_vpeids;

	if (kvm_fd >= 0) {
		max_vcpus = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS);
		if (max_vcpus > 0)
			MAX_VCPUS = max_vcpus;

		max_vpeids = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ARM_MAX_VPEID);
		if (max_vpeids > 0)
			ITS_MAX_VPEID = max_vpeids;

		close(kvm_fd);
	}
}

static void guest_code(void)
{
	GUEST_SYNC(0);
	GUEST_DONE();
}

static void setup_vm_with_gic(struct kvm_vm **vm, struct kvm_vcpu **vcpu, int nr_vcpus)
{
	struct kvm_vcpu **vcpus;

	TEST_REQUIRE(kvm_supports_vgic_v3());

	if (nr_vcpus == 1) {
		*vm = vm_create_with_one_vcpu(vcpu, guest_code);
	} else {
		vcpus = calloc(nr_vcpus, sizeof(*vcpus));
		TEST_ASSERT(vcpus, "Failed to allocate vcpu array");
		*vm = vm_create_with_vcpus(nr_vcpus, guest_code, vcpus);
		*vcpu = vcpus[0];
		free(vcpus);
	}
}

static void cleanup_vm(struct kvm_vm *vm, int its_fd)
{
	if (its_fd >= 0)
		close(its_fd);
	kvm_vm_free(vm);
}

TEST(basic_vlpi_toggle)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int its_fd, ret;
	int vcpu_id = 0;

	setup_vm_with_gic(&vm, &vcpu, 1);
	its_fd = vgic_its_setup(vm);

	ret = ioctl(vm->fd, KVM_QUERY_VCPU_VLPI, &vcpu_id);
	EXPECT_GE(ret, 0);

	ret = ioctl(vm->fd, KVM_ENABLE_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	ret = ioctl(vm->fd, KVM_QUERY_VCPU_VLPI, &vcpu_id);
	EXPECT_GT(ret, 0);

	ret = ioctl(vm->fd, KVM_DISABLE_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	ret = ioctl(vm->fd, KVM_QUERY_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	cleanup_vm(vm, its_fd);
}

/* recycle test */
struct thread_data {
	struct kvm_vm *vm;
	int vcpu_id;
	int ret;
};

static void *vlpi_thread(void *arg)
{
	struct thread_data *data = arg;

	data->ret = ioctl(data->vm->fd, KVM_ENABLE_VCPU_VLPI, &data->vcpu_id);
	if (data->ret == 0)
		data->ret = ioctl(data->vm->fd, KVM_DISABLE_VCPU_VLPI, &data->vcpu_id);

	return NULL;
}

TEST(vpeid_recycling)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int its_fd;
	int vcpu_id, i;
	int cycles = (ITS_MAX_VPEID * 2) / MAX_VCPUS;
	pthread_t threads[MAX_VCPUS];
	struct thread_data data[MAX_VCPUS];

	setup_vm_with_gic(&vm, &vcpu, MAX_VCPUS);
	its_fd = vgic_its_setup(vm);

	for (i = 0; i < cycles; i++) {
		for (vcpu_id = 0; vcpu_id < MAX_VCPUS; vcpu_id++) {
			data[vcpu_id].vm = vm;
			data[vcpu_id].vcpu_id = vcpu_id;
			pthread_create(&threads[vcpu_id], NULL, vlpi_thread, &data[vcpu_id]);
		}

		for (vcpu_id = 0; vcpu_id < MAX_VCPUS; vcpu_id++) {
			pthread_join(threads[vcpu_id], NULL);
			EXPECT_EQ(data[vcpu_id].ret, 0);
		}
	}

	cleanup_vm(vm, its_fd);
}

TEST(double_enable_disable)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int its_fd, ret;
	int vcpu_id = 0;

	setup_vm_with_gic(&vm, &vcpu, 1);
	its_fd = vgic_its_setup(vm);

	ret = ioctl(vm->fd, KVM_ENABLE_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	ret = ioctl(vm->fd, KVM_ENABLE_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	ret = ioctl(vm->fd, KVM_QUERY_VCPU_VLPI, &vcpu_id);
	EXPECT_GT(ret, 0);

	ret = ioctl(vm->fd, KVM_DISABLE_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	ret = ioctl(vm->fd, KVM_DISABLE_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	ret = ioctl(vm->fd, KVM_QUERY_VCPU_VLPI, &vcpu_id);
	EXPECT_EQ(ret, 0);

	cleanup_vm(vm, its_fd);
}

TEST(uninitialized_vcpu)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int its_fd, ret;
	int invalid_vcpu_id = 999;

	setup_vm_with_gic(&vm, &vcpu, 1);
	its_fd = vgic_its_setup(vm);

	ret = ioctl(vm->fd, KVM_QUERY_VCPU_VLPI, &invalid_vcpu_id);
	EXPECT_LT(ret, 0);

	ret = ioctl(vm->fd, KVM_ENABLE_VCPU_VLPI, &invalid_vcpu_id);
	EXPECT_LT(ret, 0);

	ret = ioctl(vm->fd, KVM_DISABLE_VCPU_VLPI, &invalid_vcpu_id);
	EXPECT_LT(ret, 0);

	cleanup_vm(vm, its_fd);
}

TEST(vpeid_exhaustion)
{
	struct rlimit rlim;
	struct kvm_vm **vms;
	struct kvm_vcpu **vcpus;
	int *its_fds;
	/* Allocate enough VMs to exhaust vPEs, plus one */
	int num_vms = ITS_MAX_VPEID / MAX_VCPUS + 1;
	int remainder_vcpus = ITS_MAX_VPEID % MAX_VCPUS;
	int vm_idx, vcpu_id, ret;
	int successful_enables = 0;

	/* Raise fd limit if below vPE limit, as we can't allocate enough vCPUs */
	if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
		struct rlimit new_rlim = rlim;
		/*
		 * Require [num_vms * (vcpus_per_vm + VM_fd + ITS_fd) + KVM] file
		 * descriptors, tripled for safety.
		 */
		int required_fds = (num_vms * (MAX_VCPUS + 2) + 1) * 3;

		if (rlim.rlim_cur < required_fds) {
			new_rlim.rlim_cur = min_t(rlim_t, required_fds, rlim.rlim_max);
			if (setrlimit(RLIMIT_NOFILE, &new_rlim) != 0) {
				SKIP(return, "Need %d FDs, have %ld, cannot increase limit",
					required_fds, rlim.rlim_cur);
			}
		}
	}

	vms = calloc(num_vms, sizeof(*vms));
	vcpus = calloc(num_vms, sizeof(*vcpus));
	its_fds = calloc(num_vms, sizeof(*its_fds));
	TEST_ASSERT(vms && vcpus && its_fds, "Failed to allocate VM arrays");

	/* Create all VMs */
	for (vm_idx = 0; vm_idx < num_vms; vm_idx++) {
		setup_vm_with_gic(&vms[vm_idx], &vcpus[vm_idx], MAX_VCPUS);
		its_fds[vm_idx] = vgic_its_setup(vms[vm_idx]);
	}

	/* Exhaust all vPEs */
	for (vm_idx = 0; vm_idx < num_vms - 1; vm_idx++) {
		for (vcpu_id = 0; vcpu_id < MAX_VCPUS; vcpu_id++) {
			ret = ioctl(vms[vm_idx]->fd, KVM_ENABLE_VCPU_VLPI, &vcpu_id);
			if (ret == 0)
				successful_enables++;
		}
	}

	for (vcpu_id = 0; vcpu_id < remainder_vcpus; vcpu_id++) {
		ret = ioctl(vms[num_vms - 1]->fd, KVM_ENABLE_VCPU_VLPI, &vcpu_id);
		if (ret == 0)
			successful_enables++;
	}

	/* Should have exhausted vPEID limit */
	TEST_ASSERT(successful_enables == ITS_MAX_VPEID,
		"Failed to allocate all existing vPEIDs");

	/* Try assigning one more vPEID past exhaustion*/
	vcpu_id = remainder_vcpus;
	ret = ioctl(vms[num_vms - 1]->fd, KVM_ENABLE_VCPU_VLPI, &vcpu_id);

	/* Verify failure to allocate additional vPEID */
	TEST_ASSERT(ret < 0, "Failed to detect vPEID exhaustion");

	/* Cleanup all VMs */
	for (vm_idx = 0; vm_idx < num_vms; vm_idx++)
		cleanup_vm(vms[vm_idx], its_fds[vm_idx]);

	free(vms);
	free(vcpus);
	free(its_fds);
	setrlimit(RLIMIT_NOFILE, &rlim); /* Restore fd limit */
}

TEST_HARNESS_MAIN
