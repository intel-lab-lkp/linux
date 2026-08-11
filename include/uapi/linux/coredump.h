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
 * @COREDUMP_HEADER: send the coredump as a sequence of frames instead of
 *                   as a plain byte stream, see struct coredump_frame_header;
 *                   requires COREDUMP_KERNEL
 * @COREDUMP_SPARSE: describe the holes in the coredump as zero frames
 *                   instead of transferring them; requires COREDUMP_HEADER
 */
enum {
	COREDUMP_KERNEL		= (1ULL << 0),
	COREDUMP_USERSPACE	= (1ULL << 1),
	COREDUMP_REJECT		= (1ULL << 2),
	COREDUMP_WAIT		= (1ULL << 3),
	COREDUMP_HEADER		= (1ULL << 4),
	COREDUMP_SPARSE		= (1ULL << 5),
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
 * one go. Userspace may also simply read a COREDUMP_REQ_SIZE_VER0
 * request. If the size the kernel sends is larger userspace simply
 * discards any remaining data.
 *
 * The coredump_req->mask member is set to the currently known features.
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
 * enum coredump_mark - Markers for the coredump socket
 *
 * The kernel will place a single byte on the coredump socket. The
 * markers notify userspace whether the coredump ack succeeded or
 * failed.
 *
 * @COREDUMP_MARK_MINSIZE: the provided coredump_ack size was too small
 * @COREDUMP_MARK_MAXSIZE: the provided coredump_ack size was too big
 * @COREDUMP_MARK_UNSUPPORTED: the provided coredump_ack mask was invalid
 * @COREDUMP_MARK_CONFLICTING: the provided coredump_ack mask has conflicting options
 * @COREDUMP_MARK_REQACK: the coredump request and ack was successful
 * @__COREDUMP_MARK_MAX: the maximum coredump mark value
 */
enum coredump_mark {
	COREDUMP_MARK_REQACK		= 0U,
	COREDUMP_MARK_MINSIZE		= 1U,
	COREDUMP_MARK_MAXSIZE		= 2U,
	COREDUMP_MARK_UNSUPPORTED	= 3U,
	COREDUMP_MARK_CONFLICTING	= 4U,
	__COREDUMP_MARK_MAX		= (1U << 31),
};

/**
 * enum coredump_frame_type - Type of a coredump frame
 *
 * @COREDUMP_FRAME_DATA: the header is followed by ->len bytes of data
 * @COREDUMP_FRAME_ZERO: the header stands for ->len zero bytes and is not
 *                       followed by any data
 * @__COREDUMP_FRAME_MAX: the maximum coredump frame type value
 */
enum coredump_frame_type {
	COREDUMP_FRAME_DATA	= 0U,
	COREDUMP_FRAME_ZERO	= 1U,
	__COREDUMP_FRAME_MAX	= (1U << 31),
};

/**
 * struct coredump_frame_header - header of a coredump frame
 * @size: size of struct coredump_frame_header
 * @type: one of enum coredump_frame_type
 * @flags: modifiers for this frame
 * @offset: offset of this frame in the coredump
 * @len: length of this frame in the coredump
 *
 * If the coredump server raises COREDUMP_HEADER in coredump_ack->mask the
 * kernel doesn't send the coredump as a plain byte stream. It sends a
 * sequence of frames instead. A COREDUMP_FRAME_DATA frame is followed by
 * @len bytes of actual coredump data. A COREDUMP_FRAME_ZERO frame is
 * followed by nothing and stands for @len zero bytes. A server that didn't
 * raise COREDUMP_SPARSE never sees a zero frame.
 *
 * The @size member is set to the size of struct coredump_frame_header the
 * kernel knows and lets the header grow later. It comes first so it can be
 * peeked. Userspace must consume @size bytes and discard anything beyond
 * what it knows. The same way it deals with struct coredump_req. It must
 * refuse a @size smaller than COREDUMP_FRAME_HEADER_SIZE_VER0.
 *
 * The @flags member carries modifiers that change how the frame is to be
 * interpreted. No flags are defined yet. Userspace must refuse a frame
 * carrying a flag it doesn't know.
 *
 * COREDUMP_HEADER must be combined with COREDUMP_KERNEL, and
 * COREDUMP_SPARSE with COREDUMP_HEADER.
 */
struct coredump_frame_header {
	__u32 size;
	__u32 type;
	__u64 flags;
	__u64 offset;
	__u64 len;
};

enum {
	COREDUMP_FRAME_HEADER_SIZE_VER0 = 32U, /* size of first published struct */
};

#endif /* _UAPI_LINUX_COREDUMP_H */
