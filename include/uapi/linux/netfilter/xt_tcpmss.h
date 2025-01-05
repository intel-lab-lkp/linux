/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_XT_TCPMSS_H
#define _UAPI_XT_TCPMSS_H

#include <linux/types.h>

#define XT_TCPMSS_CLAMP_PMTU	0xffff

struct xt_tcpmss_match_info {
    __u16 mss_min, mss_max;
    __u8 invert;
};

struct xt_tcpmss_info {
	__u16 mss;
};

#endif /* _UAPI_XT_TCPMSS_H */
