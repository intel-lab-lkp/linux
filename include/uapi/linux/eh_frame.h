/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_EH_FRAME_H
#define _UAPI_LINUX_EH_FRAME_H

#include <linux/types.h>

struct eh_frame_setup {
	__u64	eh_frame_hdr_start;
	__u64	eh_frame_hdr_size;
	__u64	text_start;
	__u64	text_size;
};

#endif /* _UAPI_LINUX_EH_FRAME_H */
