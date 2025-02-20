/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COMMON_H
#define _COMMON_H

bool syscall_user_dispatch(struct pt_regs *regs);

/* sched notifiers for CFS bandwidth deferral */
extern void sched_notify_critical_section_entry(void);
extern void sched_notify_critical_section_exit(void);

#endif
