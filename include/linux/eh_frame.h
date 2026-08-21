/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_EH_FRAME_H
#define _LINUX_EH_FRAME_H

#ifdef CONFIG_HAVE_UNWIND_USER_EH_FRAME

struct eh_frame_section {
	unsigned long	eh_frame_hdr_start;
	unsigned long	eh_frame_hdr_end;
	unsigned long	text_start;
	unsigned long	text_end;

	/* .eh_frame_hdr information */
	unsigned long	eh_frame_start;
	unsigned long	eh_frame_vma_end;
	unsigned long	binary_search_table_start;
	unsigned long	binary_search_table_end;
	unsigned long	fde_count;
	u8		binary_search_table_enc;
};

extern int eh_frame_add_section(unsigned long eh_frame_hdr_start,
				unsigned long eh_frame_hdr_end,
				unsigned long text_start,
				unsigned long text_end);
extern int eh_frame_remove_section(unsigned long eh_frame_hdr_start);

#else /* !CONFIG_HAVE_UNWIND_USER_EH_FRAME */

static inline int eh_frame_add_section(unsigned long eh_frame_hdr_start,
				       unsigned long eh_frame_hdr_end,
				       unsigned long text_start,
				       unsigned long text_end)
{
	return -ENOSYS;
}

static inline int eh_frame_remove_section(unsigned long eh_frame_hdr_start)
{
	return -ENOSYS;
}

#endif /* CONFIG_HAVE_UNWIND_USER_EH_FRAME */

#endif /* _LINUX_EH_FRAME_H */
