/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _UAPI_LINUX_COREDUMP_H
#define _UAPI_LINUX_COREDUMP_H

#include <linux/types.h>

/**
 * coredump_{req,ack} flags
 * @COREDUMP_KERNEL: kernel writes coredump
 * @COREDUMP_USERSPACE: userspace writes coredump
 * @COREDUMP_REJECT: don't generate coredump
 * @COREDUMP_WAIT: wait for coredump server
 */
enum {
	COREDUMP_KERNEL		= (1ULL << 0),
	COREDUMP_USERSPACE	= (1ULL << 1),
	COREDUMP_REJECT		= (1ULL << 2),
	COREDUMP_WAIT		= (1ULL << 3),
};

/**
 * struct coredump_req - message kernel sends to userspace
 * @size: size of struct coredump_req
 * @size_ack: known size of struct coredump_ack on this kernel
 * @mask: supported features
 *
 * When a coredump happens the kernel will connect to the coredump
 * socket and send a coredump request to the coredump server. The @size
 * member is set to the size of struct coredump_req and provides a hint
 * to userspace how much data can be read. Userspace may use MSG_PEEK to
 * peek the size of struct coredump_req and then choose to consume it in
 * one go. Userspace may also simply read a COREDUMP_ACK_SIZE_VER0
 * request. If the size the kernel sends is larger userspace simply
 * discards any remaining data.
 *
 * The coredump_req->mask member is set to the currently know features.
 * Userspace may only set coredump_ack->mask to the bits raised by the
 * kernel in coredump_req->mask.
 *
 * The coredump_req->size_ack member is set by the kernel to the size of
 * struct coredump_ack the kernel knows. Userspace may only send up to
 * coredump_req->size_ack bytes to the kernel and must set
 * coredump_ack->size accordingly.
 */
struct coredump_req {
	__u32 size;
	__u32 size_ack;
	__u64 mask;
};

enum {
	COREDUMP_REQ_SIZE_VER0 = 16U, /* size of first published struct */
};

/**
 * struct coredump_ack - message userspace sends to kernel
 * @size: size of the struct
 * @spare: unused
 * @mask: features kernel is supposed to use
 *
 * The @size member must be set to the size of struct coredump_ack. It
 * may never exceed what the kernel returned in coredump_req->size_ack
 * but it may of course be smaller (>= COREDUMP_ACK_SIZE_VER0 and <=
 * coredump_req->size_ack).
 *
 * The @mask member must be set to the features the coredump server
 * wants the kernel to use. Only bits the kernel returned in
 * coredump_req->mask may be set.
 */
struct coredump_ack {
	__u32 size;
	__u32 spare;
	__u64 mask;
};

enum {
	COREDUMP_ACK_SIZE_VER0 = 16U, /* size of first published struct */
};

/**
 * enum coredump_oob - Out-of-band markers for the coredump socket
 *
 * The kernel will place a single byte coredump_oob marker on the
 * coredump socket. An interested coredump server can listen for POLLPRI
 * and figure out why the provided coredump_ack was invalid.
 *
 * The out-of-band markers allow advanced userspace to infer more details
 * about a coredump ack. They are optional and can be ignored. They
 * aren't necessary for the coredump server to function correctly.
 *
 * @COREDUMP_OOB_INVALIDSIZE: the provided coredump_ack size was invalid
 * @COREDUMP_OOB_UNSUPPORTED: the provided coredump_ack mask was invalid
 * @COREDUMP_OOB_CONFLICTING: the provided coredump_ack mask has conflicting options
 * @__COREDUMP_OOB_MAX: the maximum value for coredump_oob
 */
enum coredump_oob {
	COREDUMP_OOB_INVALIDSIZE = 1U,
	COREDUMP_OOB_UNSUPPORTED = 2U,
	COREDUMP_OOB_CONFLICTING = 3U,
	__COREDUMP_OOB_MAX       = 255U,
} __attribute__ ((__packed__));

#endif /* _UAPI_LINUX_COREDUMP_H */
