// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kvm_host.h>

#include <kvm/etm.h>

static DEFINE_PER_CPU(struct kvm_etm_event, kvm_etm_events);

struct kvm_etm_event *kvm_get_etm_event(void)
{
	return this_cpu_ptr(&kvm_etm_events);
}

void kvm_etm_set_events(struct perf_event_attr *attr)
{
	struct kvm_etm_event *etm_event;

	/*
	 * Exclude guest option only requires extra work with nVHE.
	 * Otherwise it works automatically with TRFCR_EL{1,2}
	 */
	if (has_vhe())
		return;

	etm_event = kvm_get_etm_event();

	etm_event->exclude_guest = attr->exclude_guest;
	etm_event->exclude_host = attr->exclude_host;
	etm_event->exclude_kernel = attr->exclude_kernel;
	etm_event->exclude_user = attr->exclude_user;
}
EXPORT_SYMBOL_GPL(kvm_etm_set_events);

void kvm_etm_clr_events(void)
{
	struct kvm_etm_event *etm_event;

	if (has_vhe())
		return;

	etm_event = kvm_get_etm_event();

	etm_event->exclude_guest = false;
	etm_event->exclude_host = false;
	etm_event->exclude_kernel = false;
	etm_event->exclude_user = false;
}
EXPORT_SYMBOL_GPL(kvm_etm_clr_events);
