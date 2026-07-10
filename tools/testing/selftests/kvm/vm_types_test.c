// SPDX-License-Identifier: GPL-2.0-only
/*
 * Verify that KVM_CREATE_VM accepts exactly the VM types enumerated by
 * KVM_CAP_VM_TYPES, and rejects every other type with -EINVAL.
 */
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include <linux/kvm.h>

#include "kvm_util.h"
#include "test_util.h"

int main(void)
{
	unsigned int supported_types;
	unsigned long type;
	int kvm_fd, fd;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_VM_TYPES));

	kvm_fd = open_kvm_dev_path_or_exit();
	supported_types = kvm_check_cap(KVM_CAP_VM_TYPES);
	pr_info("KVM_CAP_VM_TYPES: 0x%x\n", supported_types);

	/*
	 * KVM_CAP_VM_TYPES is a u32 bitmap, so only types 0..31 can ever be
	 * advertised.  Walk past that range as well to confirm that any
	 * out-of-range type is rejected rather than silently accepted.
	 */
	for (type = 0; type < 64; type++) {
		bool supported = type < 32 && (supported_types & (1U << type));

		fd = __kvm_ioctl(kvm_fd, KVM_CREATE_VM, (void *)type);

		if (supported) {
			TEST_ASSERT(fd >= 0,
				    "KVM_CREATE_VM(%lu) should succeed, KVM_CAP_VM_TYPES=0x%x",
				    type, supported_types);
			close(fd);
		} else {
			TEST_ASSERT(fd < 0 && errno == EINVAL,
				    "KVM_CREATE_VM(%lu) should fail with EINVAL, KVM_CAP_VM_TYPES=0x%x",
				    type, supported_types);
		}
	}

	return 0;
}
