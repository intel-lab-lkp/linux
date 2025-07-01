/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SFRAME_DEBUG_H
#define _SFRAME_DEBUG_H

#include <linux/sframe.h>
#include "sframe.h"

#ifdef CONFIG_DYNAMIC_DEBUG

#define dbg(fmt, ...)							\
	pr_debug("%s (%d): " fmt, current->comm, current->pid, ##__VA_ARGS__)

static __always_inline void dbg_print_header(struct sframe_section *sec)
{
	unsigned long fdes_end;

	fdes_end = sec->fdes_start + (sec->num_fdes * sizeof(struct sframe_fde));

	dbg("SEC: sframe:0x%lx-0x%lx text:0x%lx-0x%lx "
	    "fdes:0x%lx-0x%lx fres:0x%lx-0x%lx "
	    "ra_off:%d fp_off:%d\n",
	    sec->sframe_start, sec->sframe_end, sec->text_start, sec->text_end,
	    sec->fdes_start, fdes_end, sec->fres_start, sec->fres_end,
	    sec->ra_off, sec->fp_off);
}

#else /* !CONFIG_DYNAMIC_DEBUG */

#define dbg(args...)			no_printk(args)

static inline void dbg_print_header(struct sframe_section *sec) {}

#endif /* !CONFIG_DYNAMIC_DEBUG */

#endif /* _SFRAME_DEBUG_H */
