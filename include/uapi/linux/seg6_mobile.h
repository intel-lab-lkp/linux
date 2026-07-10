/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 *  SRv6 Mobile User Plane implementation
 */
#ifndef _UAPI_LINUX_SEG6_MOBILE_H
#define _UAPI_LINUX_SEG6_MOBILE_H

enum {
	SEG6_MOBILE_UNSPEC,
	SEG6_MOBILE_ACTION,
	SEG6_MOBILE_NH6,
	SEG6_MOBILE_COUNTERS,
	__SEG6_MOBILE_MAX,
};

#define SEG6_MOBILE_MAX	(__SEG6_MOBILE_MAX - 1)

enum {
	SEG6_MOBILE_ACTION_UNSPEC	= 0,
	/* swap IPv6 DA with the next SID, leave SRH untouched */
	SEG6_MOBILE_ACTION_END_MAP	= 1,

	__SEG6_MOBILE_ACTION_MAX,
};

#define SEG6_MOBILE_ACTION_MAX	(__SEG6_MOBILE_ACTION_MAX - 1)

/* SRv6 Mobile Behavior counters are encoded as netlink attributes
 * guaranteeing the correct alignment.
 * Each counter is identified by a different attribute type (i.e.
 * SEG6_MOBILE_CNT_PACKETS).
 *
 * - SEG6_MOBILE_CNT_PACKETS: identifies a counter that counts the number
 *   of packets that have been CORRECTLY processed by an SRv6 Behavior
 *   instance (i.e., packets that generate errors or are dropped are NOT
 *   counted).
 *
 * - SEG6_MOBILE_CNT_BYTES: identifies a counter that counts the total
 *   amount of traffic in bytes of all packets that have been CORRECTLY
 *   processed by an SRv6 Behavior instance (i.e., packets that generate
 *   errors or are dropped are NOT counted).
 *
 * - SEG6_MOBILE_CNT_ERRORS: identifies a counter that counts the number
 *   of packets that have NOT been properly processed by an SRv6 Behavior
 *   instance (i.e., packets that generate errors or are dropped).
 */
enum {
	SEG6_MOBILE_CNT_UNSPEC,
	SEG6_MOBILE_CNT_PACKETS,
	SEG6_MOBILE_CNT_BYTES,
	SEG6_MOBILE_CNT_ERRORS,
	SEG6_MOBILE_CNT_PAD,		/* pad for 64 bits values */
	__SEG6_MOBILE_CNT_MAX,
};

#define SEG6_MOBILE_CNT_MAX	(__SEG6_MOBILE_CNT_MAX - 1)

#endif /* _UAPI_LINUX_SEG6_MOBILE_H */
