/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 * Copyright (c) 2005 Linas Vepstas <linas@linas.org>
 */

#ifndef ASM_POWERPC_EEH_EVENT_H
#define ASM_POWERPC_EEH_EVENT_H
#ifdef __KERNEL__

#include <linux/workqueue.h>

/*
 * structure holding pci controller data that describes a
 * change in the isolation status of a PCI slot.  A pointer
 * to this struct is passed as the data pointer in a notify
 * callback.
 */
struct eeh_event {
	struct work_struct	work;
	struct list_head	list;	/* to form event queue	*/
	struct eeh_pe		*pe;	/* EEH PE		*/
};

extern spinlock_t eeh_eventlist_lock;

int eeh_event_init(void);
int eeh_phb_event(struct eeh_pe *pe);
int eeh_send_failure_event(struct eeh_pe *pe);
int __eeh_send_failure_event(struct eeh_pe *pe);
void eeh_remove_event(struct eeh_pe *pe, bool force);
void eeh_handle_normal_event(struct eeh_pe *pe);
void eeh_handle_normal_event_work(struct work_struct *work);
void eeh_handle_special_event(void);

#endif /* __KERNEL__ */
#endif /* ASM_POWERPC_EEH_EVENT_H */
