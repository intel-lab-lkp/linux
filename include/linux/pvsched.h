/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2024 Google  */

#ifndef _LINUX_PVSCHED_H
#define _LINUX_PVSCHED_H 1

/*
 * List of events for which hypervisor calls back into pvsched driver.
 * Driver can specify the events it is interested in.
 */
enum pvsched_vcpu_events {
	PVSCHED_VCPU_VMENTER = 0x1,
	PVSCHED_VCPU_VMEXIT = 0x2,
	PVSCHED_VCPU_HALT = 0x4,
	PVSCHED_VCPU_INTR_INJ = 0x8,
};

#define PVSCHED_NAME_MAX	32
#define PVSCHED_MAX		8
#define PVSCHED_DRV_BUF_MAX	(PVSCHED_NAME_MAX * PVSCHED_MAX + PVSCHED_MAX)

/*
 * pvsched driver callbacks.
 * TODO: versioning support for better compatibility with the guest
 *       component implementing this feature.
 */
struct pvsched_vcpu_ops {
	/*
	 * pvsched_vcpu_register() - Register the vcpu with pvsched driver.
	 * @pid: pid of the vcpu task.
	 *
	 * pvsched driver can store the pid internally and initialize
	 * itself to prepare for receiving callbacks from thsi vcpu.
	 */
	int (*pvsched_vcpu_register)(struct pid *pid);

	/*
	 * pvsched_vcpu_unregister() - Un-register the vcpu with pvsched driver.
	 * @pid: pid of the vcpu task.
	 */
	void (*pvsched_vcpu_unregister)(struct pid *pid);

	/*
	 * pvsched_vcpu_notify_event() - Callback for pvsched events
	 * @addr: Address of the memory region shared with guest
	 * @pid: pid of the vcpu task.
	 * @events: bit mask of the events that hypervisor wants to notify.
	 */
	void (*pvsched_vcpu_notify_event)(void *addr, struct pid *pid, u32 event);

	char name[PVSCHED_NAME_MAX];
	struct module *owner;
	struct list_head list;
	u32 events;
	u32 key;
};

#ifdef CONFIG_PARAVIRT_SCHED_HOST
int pvsched_get_available_drivers(char *buf, size_t maxlen);

int pvsched_register_vcpu_ops(struct pvsched_vcpu_ops *ops);
void pvsched_unregister_vcpu_ops(struct pvsched_vcpu_ops *ops);

struct pvsched_vcpu_ops *pvsched_get_vcpu_ops(char *name);
void pvsched_put_vcpu_ops(struct pvsched_vcpu_ops *ops);

static inline int pvsched_validate_vcpu_ops(struct pvsched_vcpu_ops *ops)
{
	/*
	 * All callbacks are mandatory.
	 */
	if (!ops->pvsched_vcpu_register || !ops->pvsched_vcpu_unregister ||
			!ops->pvsched_vcpu_notify_event)
		return -EINVAL;

	return 0;
}
#else
static inline void pvsched_get_available_drivers(char *buf, size_t maxlen)
{
}

static inline int pvsched_register_vcpu_ops(struct pvsched_vcpu_ops *ops)
{
	return -ENOTSUPP;
}

static inline void pvsched_unregister_vcpu_ops(struct pvsched_vcpu_ops *ops)
{
}

static inline struct pvsched_vcpu_ops *pvsched_get_vcpu_ops(char *name)
{
	return NULL;
}

static inline void pvsched_put_vcpu_ops(struct pvsched_vcpu_ops *ops)
{
}
#endif

#endif
