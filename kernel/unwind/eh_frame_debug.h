/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EH_FRAME_DEBUG_H
#define _EH_FRAME_DEBUG_H

#include <linux/eh_frame.h>
#include "eh_frame.h"

#ifdef CONFIG_DYNAMIC_DEBUG

#define dbg(fmt, ...)							\
	pr_debug("%s (%d): " fmt, current->comm, current->pid, ##__VA_ARGS__)

#else /* !CONFIG_DYNAMIC_DEBUG */

#define dbg(args...)			no_printk(args)

#endif /* !CONFIG_DYNAMIC_DEBUG */

#endif /* _EH_FRAME_DEBUG_H */
