// SPDX-License-Identifier: GPL-2.0

#define KMSG_COMPONENT "kvm-s390-arm64"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/miscdevice.h>
#include <linux/kvm.h>
#include <linux/kvm_types.h>
#include <linux/kvm_host.h>

#include "arm.h"

#define CREATE_TRACE_POINTS
#include "trace.h"

static unsigned long system_supported_vcpu_features(void);

#define __INCL_GEN_ARM_FILE
#include "generated/arm.inc"
#undef __INCL_GEN_ARM_FILE

int kvm_vm_ioctl_check_extension(struct kvm *kvm, long ext)
{
	int ret;

	switch (ext) {
	case KVM_CAP_NR_VCPUS:
	case KVM_CAP_MAX_VCPUS:
	case KVM_CAP_MAX_VCPU_ID:
		ret = KVM_MAX_VCPUS;
		break;
	case KVM_CAP_ARM_VM_IPA_SIZE:
		ret = get_kvm_ipa_limit();
		break;
	case KVM_CAP_IOEVENTFD:
		ret = 1;
		break;
	default:
		ret = 0;
	}

	return ret;
}

static u64 kvm_max_guest_address(void)
{
	u64 max_addr;

	if (sclp.hamax == U64_MAX)
		max_addr = TASK_SIZE_MAX;
	else
		max_addr = min_t(u64, TASK_SIZE_MAX, sclp.hamax);
	return ALIGN_DOWN(max_addr + 1, 1 << 30) - 1;
}

vm_fault_t kvm_arch_vcpu_fault(struct kvm_vcpu *vcpu, struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

long kvm_arch_dev_ioctl(struct file *filp,
			unsigned int ioctl, unsigned long arg)
{
	return -EINVAL;
}

u32 get_kvm_ipa_limit(void)
{
	return fls64(kvm_max_guest_address() + 1) - 1;
}

int kvm_arch_vcpu_precreate(struct kvm *kvm, unsigned int id)
{
	return 0;
}

void kvm_arch_vcpu_postcreate(struct kvm_vcpu *vcpu)
{
}

int kvm_arch_vcpu_ioctl_get_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	*mp_state = READ_ONCE(vcpu->arch.mp_state);
	return 0;
}

int kvm_arch_vcpu_ioctl_set_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	return -EINVAL;
}

int kvm_arch_vcpu_runnable(struct kvm_vcpu *vcpu)
{
	return 0;
}

static unsigned long system_supported_vcpu_features(void)
{
	unsigned long features = KVM_S390_ARM64_IMPL_FEATURES;

	return features;
}

bool kvm_arch_irqchip_in_kernel(struct kvm *kvm)
{
	return false;
}

int kvm_set_msi(struct kvm_kernel_irq_routing_entry *e, struct kvm *kvm,
		int irq_source_id, int level, bool line_status)
{
	return -EINVAL;
}

int kvm_set_routing_entry(struct kvm *kvm,
			  struct kvm_kernel_irq_routing_entry *e,
			  const struct kvm_irq_routing_entry *ue)
{
	return -EINVAL;
}

int kvm_cpu_has_pending_timer(struct kvm_vcpu *vcpu)
{
	return 0;
}

#ifdef CONFIG_HAVE_KVM_NO_POLL
__weak bool kvm_arch_no_poll(struct kvm_vcpu *vcpu)
{
	return false;
}
#endif

long kvm_arch_vcpu_unlocked_ioctl(struct file *filp, unsigned int ioctl,
				  unsigned long arg)
{
	return -ENOIOCTLCMD;
}

static int __init kvm_s390_arm64_init(void)
{
	if (!sclp.has_aef)
		return -ENXIO;

	return kvm_init_with_dev(sizeof(struct kvm_vcpu), 0, THIS_MODULE,
				 KVM_DEV_NAME, MISC_DYNAMIC_MINOR);
}

static __exit void kvm_s390_arm64_exit(void)
{
	kvm_exit();
}

module_init(kvm_s390_arm64_init);
module_exit(kvm_s390_arm64_exit);
