/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_GOODIX_EC_H_
#define _UAPI_GOODIX_EC_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define GOODIX_EC_UAPI_MAGIC		'G'
#define GOODIX_EC_UAPI_TX_MAX		500u
#define GOODIX_EC_UAPI_RX_MAX		(128u * 1024u)

/*
 * read(2) returns one record:
 *   struct goodix_ec_record_header
 *   followed by len bytes of MP payload (normally one Goodix frame).
 */
struct goodix_ec_record_header {
	__u32 len;
	__u32 mp_type;
	__u64 timestamp_ns;
};

/*
 * write(2) accepts:
 *   struct goodix_ec_tx_header
 *   followed by payload_len bytes used as the MP payload.
 */
struct goodix_ec_tx_header {
	__u8 mp_flags;
	__u8 reserved;
	__u16 payload_len;
	__u32 flags;
};

#define GOODIX_EC_IOCTL_FLUSH_RX	_IO(GOODIX_EC_UAPI_MAGIC, 0x11)

#endif /* _UAPI_GOODIX_EC_H_ */
