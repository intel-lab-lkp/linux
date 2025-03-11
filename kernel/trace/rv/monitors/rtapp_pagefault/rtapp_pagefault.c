// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/tracepoint.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>
#include <rv/instrumentation.h>
#include <linux/sched/rt.h>
#include <trace/events/sched.h>
#include <trace/events/exceptions.h>
#include <rv_trace.h>

#include "ba.h"

static void handle_page_fault(void *data, unsigned long address, struct pt_regs *regs,
				unsigned long error_code)
{
	rv_rtapp_pagefault_atom_update(current, PAGEFAULT, true);
	rv_rtapp_pagefault_atom_update(current, PAGEFAULT, false);
}

void rv_rtapp_pagefault_atoms_fetch(struct task_struct *task, struct ltl_monitor *mon)
{
	rv_rtapp_pagefault_atom_set(mon, RT, rt_task(task));
}

void rv_rtapp_pagefault_atoms_init(struct task_struct *task, struct ltl_monitor *mon)
{
	rv_rtapp_pagefault_atom_set(mon, PAGEFAULT, false);
}

static int enable_rtapp_pagefault(void)
{
	int ret;

	ret = rv_rtapp_pagefault_init(0);
	if (ret)
		return ret;

	rv_attach_trace_probe("rtapp_pagefault", page_fault_kernel, handle_page_fault);
	rv_attach_trace_probe("rtapp_pagefault", page_fault_user, handle_page_fault);

	return 0;
}

static void disable_rtapp_pagefault(void)
{
	rv_detach_trace_probe("rtapp_pagefault", page_fault_kernel, handle_page_fault);
	rv_detach_trace_probe("rtapp_pagefault", page_fault_user, handle_page_fault);

	rv_rtapp_pagefault_destroy();
}

static struct rv_monitor rv_rtapp_pagefault = {
	.name = "rtapp_pagefault",
	.description = "monitor RT tasks do not page fault",
	.enable = enable_rtapp_pagefault,
	.disable = disable_rtapp_pagefault,
};

void rv_rtapp_pagefault_error(struct task_struct *task, struct ltl_monitor *mon)
{
	trace_rtapp_pagefault_error(task);
#ifdef CONFIG_RV_REACTORS
	if (rv_rtapp_pagefault.react)
		rv_rtapp_pagefault.react("rv: %s[%d](RT) raises a page fault\n",
					task->comm, task->pid);
#endif
}

static int __init register_rtapp_pagefault(void)
{
	rv_register_monitor(&rv_rtapp_pagefault);
	return 0;
}

static void __exit unregister_rtapp_pagefault(void)
{
	rv_unregister_monitor(&rv_rtapp_pagefault);
}

module_init(register_rtapp_pagefault);
module_exit(unregister_rtapp_pagefault);
