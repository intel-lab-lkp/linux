/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SFRAME_H
#define _LINUX_SFRAME_H

#include <linux/mm_types.h>
#include <linux/unwind_user_types.h>

#ifdef CONFIG_HAVE_UNWIND_USER_SFRAME

struct sframe_section {
	unsigned long	sframe_start;
	unsigned long	sframe_end;
	unsigned long	text_start;
	unsigned long	text_end;

	unsigned long	fdes_start;
	unsigned long	fres_start;
	unsigned long	fres_end;
	unsigned int	num_fdes;

	signed char	ra_off;
	signed char	fp_off;
};

extern int sframe_add_section(unsigned long sframe_start, unsigned long sframe_end,
			      unsigned long text_start, unsigned long text_end);
extern int sframe_remove_section(unsigned long sframe_addr);

#else /* !CONFIG_HAVE_UNWIND_USER_SFRAME */

static inline int sframe_add_section(unsigned long sframe_start, unsigned long sframe_end,
				     unsigned long text_start, unsigned long text_end)
{
	return -ENOSYS;
}
static inline int sframe_remove_section(unsigned long sframe_addr) { return -ENOSYS; }

#endif /* CONFIG_HAVE_UNWIND_USER_SFRAME */

#endif /* _LINUX_SFRAME_H */
