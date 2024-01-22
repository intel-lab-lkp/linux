// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/types.h>
#include <linux/jump_label.h>
#include <linux/kvm_para.h>
#include <asm/paravirt.h>
#include <linux/static_call.h>

struct static_key paravirt_steal_enabled;
struct static_key paravirt_steal_rq_enabled;

static u64 native_steal_clock(int cpu)
{
	return 0;
}

DEFINE_STATIC_CALL(pv_steal_clock, native_steal_clock);

static bool kvm_para_available(void)
{
	static int hypervisor_type;
	int config;

	if (!hypervisor_type) {
		config = read_cpucfg(CPUCFG_KVM_SIG);
		if (!memcmp(&config, KVM_SIGNATURE, 4))
			hypervisor_type = HYPERVISOR_KVM;
	}

	return hypervisor_type == HYPERVISOR_KVM;
}

int __init pv_guest_init(void)
{
	if (!cpu_has_hypervisor)
		return 0;
	if (!kvm_para_available())
		return 0;

	return 1;
}
