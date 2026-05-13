/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VMCLOCK_HOST_H
#define _LINUX_VMCLOCK_HOST_H

struct timekeeper;

extern void (*vmclock_host_update_fn)(struct timekeeper *tk);

static inline void vmclock_host_update(struct timekeeper *tk)
{
	typeof(vmclock_host_update_fn) fn = READ_ONCE(vmclock_host_update_fn);

	if (fn)
		fn(tk);
}

#endif /* _LINUX_VMCLOCK_HOST_H */
