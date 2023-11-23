/* SPDX-License-Identifier: GPL-2.0 */

#include <kvm_util.h>

#define GICD_BASE_GPA	0x8000000ULL
#define GICR_BASE_GPA	0x80A0000ULL

struct vpmu_vm {
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	int gic_fd;
};

struct vpmu_vm *create_vpmu_vm(void *guest_code);

void destroy_vpmu_vm(struct vpmu_vm *vpmu_vm);
