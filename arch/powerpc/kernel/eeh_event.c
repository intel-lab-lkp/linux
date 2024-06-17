// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 * Copyright (c) 2005 Linas Vepstas <linas@linas.org>
 */

#include <linux/delay.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <asm/eeh_event.h>
#include <asm/ppc-pci.h>

/** Overview:
 *  EEH error states may be detected within exception handlers;
 *  however, the recovery processing needs to occur asynchronously
 *  in a normal kernel context and not an interrupt context.
 *  This pair of routines creates an event and queues it onto a
 *  work-queue, where a worker thread can drive recovery.
 */

DEFINE_SPINLOCK(eeh_eventlist_lock);
static DECLARE_COMPLETION(eeh_eventlist_event);
static LIST_HEAD(eeh_eventlist);

/**
 * eeh_event_handler - Dispatch EEH events.
 * @dummy - unused
 *
 * The detection of a frozen slot can occur inside an interrupt,
 * where it can be hard to do anything about it.  The goal of this
 * routine is to pull these detection events out of the context
 * of the interrupt handler, and re-dispatch them for processing
 * at a later time in a normal context.
 */
static int eeh_event_handler(void * dummy)
{
	unsigned long flags;
	struct eeh_event *event;

	while (!kthread_should_stop()) {
		if (wait_for_completion_interruptible(&eeh_eventlist_event))
			break;

		/* Fetch EEH event from the queue */
		spin_lock_irqsave(&eeh_eventlist_lock, flags);
		event = NULL;
		if (!list_empty(&eeh_eventlist)) {
			event = list_entry(eeh_eventlist.next,
					   struct eeh_event, list);
			list_del(&event->list);
		}
		spin_unlock_irqrestore(&eeh_eventlist_lock, flags);
		if (!event)
			continue;

		/* We might have event without binding PE */
		if (event->pe)
			eeh_handle_normal_event(event->pe);
		else
			eeh_handle_special_event();

		kfree(event);
	}

	return 0;
}

/**
 * eeh_event_init - Start kernel thread to handle EEH events
 *
 * This routine is called to start the kernel thread for processing
 * EEH event.
 */
int eeh_event_init(void)
{
	struct task_struct *t;
	int ret = 0;

	t = kthread_run(eeh_event_handler, NULL, "eehd");
	if (IS_ERR(t)) {
		ret = PTR_ERR(t);
		pr_err("%s: Failed to start EEH daemon (%d)\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

int eeh_phb_event(struct eeh_pe *pe)
{
	struct eeh_event *event;
	unsigned long flags;
	struct pci_controller *phb;

	event = kzalloc(sizeof(*event), GFP_ATOMIC);
	if (!event)
		return -ENOMEM;

	if (pe) {
		phb = pe->phb;
		event->pe = pe;
		INIT_WORK(&event->work, eeh_handle_normal_event_work);
		eeh_pe_state_mark(pe, EEH_PE_RECOVERING);
		pr_err("EEH: EVENT=ERROR_DETECTED PHB=%#x PE=%#x\n",
		       phb->global_number, pe->addr);
		spin_lock_irqsave(&phb->eeh_eventlist_lock, flags);
		if (phb->eeh_in_progress) {
			pr_info("EEH: EEH already in progress on this PHB, queueing.\n");
			list_add(&event->list, &phb->eeh_eventlist);
		} else {
			pr_info("EEH: Beginning recovery on this PHB.\n");
			WARN_ON_ONCE(!list_empty(&phb->eeh_eventlist));
			phb->eeh_in_progress = true;
			queue_work(system_unbound_wq, &event->work);
		}
		spin_unlock_irqrestore(&phb->eeh_eventlist_lock, flags);
	} else {
		spin_lock_irqsave(&eeh_eventlist_lock, flags);
		list_add(&event->list, &eeh_eventlist);
		complete(&eeh_eventlist_event);
		spin_unlock_irqrestore(&eeh_eventlist_lock, flags);
	}
	return 0;
}
/**
 * eeh_send_failure_event - Generate a PCI error event
 * @pe: EEH PE
 *
 * This routine can be called within an interrupt context;
 * the actual event will be delivered in a normal context
 * (from a workqueue).
 */
int __eeh_send_failure_event(struct eeh_pe *pe)
{
	/*
	 * Mark the PE as recovering before inserting it in the queue.
	 * This prevents the PE from being free()ed by a hotplug driver
	 * while the PE is sitting in the event queue.
	 */
	if (pe) {
#ifdef CONFIG_STACKTRACE
		/*
		 * Save the current stack trace so we can dump it from the
		 * event handler thread.
		 */
		pe->trace_entries = stack_trace_save(pe->stack_trace,
					 ARRAY_SIZE(pe->stack_trace), 0);
#endif /* CONFIG_STACKTRACE */

		eeh_pe_state_mark(pe, EEH_PE_RECOVERING);
	}
	return eeh_phb_event(pe);
}

int eeh_send_failure_event(struct eeh_pe *pe)
{
	/*
	 * If we've manually suppressed recovery events via debugfs
	 * then just drop it on the floor.
	 */
	if (eeh_debugfs_no_recover) {
		pr_err("EEH: Event dropped due to no_recover setting\n");
		return 0;
	}

	return __eeh_send_failure_event(pe);
}

/**
 * eeh_remove_event - Remove EEH event from the queue
 * @pe: Event binding to the PE
 * @force: Event will be removed unconditionally
 *
 * On PowerNV platform, we might have subsequent coming events
 * is part of the former one. For that case, those subsequent
 * coming events are totally duplicated and unnecessary, thus
 * they should be removed.
 */
void eeh_remove_event(struct eeh_pe *pe, bool force)
{
	unsigned long flags;
	struct eeh_event *event, *tmp;

	/*
	 * If we have NULL PE passed in, we have dead IOC
	 * or we're sure we can report all existing errors
	 * by the caller.
	 *
	 * With "force", the event with associated PE that
	 * have been isolated, the event won't be removed
	 * to avoid event lost.
	 */
	spin_lock_irqsave(&eeh_eventlist_lock, flags);
	list_for_each_entry_safe(event, tmp, &eeh_eventlist, list) {
		if (!force && event->pe &&
		    (event->pe->state & EEH_PE_ISOLATED))
			continue;

		if (!pe) {
			list_del(&event->list);
			kfree(event);
		} else if (pe->type & EEH_PE_PHB) {
			if (event->pe && event->pe->phb == pe->phb) {
				list_del(&event->list);
				kfree(event);
			}
		} else if (event->pe == pe) {
			list_del(&event->list);
			kfree(event);
		}
	}
	spin_unlock_irqrestore(&eeh_eventlist_lock, flags);
}
