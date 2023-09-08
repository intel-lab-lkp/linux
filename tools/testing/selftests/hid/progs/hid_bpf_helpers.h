/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2022 Benjamin Tissoires
 */

#ifndef __HID_BPF_HELPERS_H
#define __HID_BPF_HELPERS_H

/* "undefine" structs in vmlinux.h, because we "override" them below */
#define hid_bpf_ctx hid_bpf_ctx___not_used
#define hid_report_type hid_report_type___not_used
#define hid_class_request hid_class_request___not_used
#define hid_bpf_attach_flags hid_bpf_attach_flags___not_used
#include "vmlinux.h"
#undef hid_bpf_ctx
#undef hid_report_type
#undef hid_class_request
#undef hid_bpf_attach_flags

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <linux/const.h>

enum hid_report_type {
	HID_INPUT_REPORT		= 0,
	HID_OUTPUT_REPORT		= 1,
	HID_FEATURE_REPORT		= 2,

	HID_REPORT_TYPES,
};

struct hid_bpf_ctx {
	__u32 index;
	const struct hid_device *hid;
	__u32 allocated_size;
	enum hid_report_type report_type;
	union {
		__s32 retval;
		__s32 size;
	};
} __attribute__((preserve_access_index));

enum hid_class_request {
	HID_REQ_GET_REPORT		= 0x01,
	HID_REQ_GET_IDLE		= 0x02,
	HID_REQ_GET_PROTOCOL		= 0x03,
	HID_REQ_SET_REPORT		= 0x09,
	HID_REQ_SET_IDLE		= 0x0A,
	HID_REQ_SET_PROTOCOL		= 0x0B,
};

enum hid_bpf_attach_flags {
	HID_BPF_FLAG_NONE = 0,
	HID_BPF_FLAG_INSERT_HEAD = _BITUL(0),
	HID_BPF_FLAG_MAX,
};

/* following are kfuncs exported by HID for HID-BPF */
extern __u8 *hid_bpf_get_data(struct hid_bpf_ctx *ctx,
			      unsigned int offset,
			      const size_t __sz) __ksym;
extern int hid_bpf_attach_prog(unsigned int hid_id, int prog_fd, u32 flags) __ksym;
extern struct hid_bpf_ctx *hid_bpf_allocate_context(unsigned int hid_id) __ksym;
extern void hid_bpf_release_context(struct hid_bpf_ctx *ctx) __ksym;
extern int hid_bpf_hw_request(struct hid_bpf_ctx *ctx,
			      __u8 *data,
			      size_t buf__sz,
			      enum hid_report_type type,
			      enum hid_class_request reqtype) __ksym;

#endif /* __HID_BPF_HELPERS_H */
