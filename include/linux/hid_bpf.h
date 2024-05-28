/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __HID_BPF_H
#define __HID_BPF_H

#include <linux/bpf.h>
#include <linux/mutex.h>
#include <uapi/linux/hid.h>

struct hid_device;

/*
 * The following is the user facing HID BPF API.
 *
 * Extra care should be taken when editing this part, as
 * it might break existing out of the tree bpf programs.
 */

/**
 * struct hid_bpf_ctx - User accessible data for all HID programs
 *
 * ``data`` is not directly accessible from the context. We need to issue
 * a call to ``hid_bpf_get_data()`` in order to get a pointer to that field.
 *
 * All of these fields are currently read-only.
 *
 * @hid: the ``struct hid_device`` representing the device itself
 * @allocated_size: Allocated size of data.
 *
 *                  This is how much memory is available and can be requested
 *                  by the HID program.
 *                  Note that for ``HID_BPF_RDESC_FIXUP``, that memory is set to
 *                  ``4096`` (4 KB)
 * @size: Valid data in the data field.
 *
 *        Programs can get the available valid size in data by fetching this field.
 *        Programs can also change this value by returning a positive number in the
 *        program.
 *        To discard the event, return a negative error code.
 *
 *        ``size`` must always be less or equal than ``allocated_size`` (it is enforced
 *        once all BPF programs have been run).
 * @retval: Return value of the previous program.
 */
struct hid_bpf_ctx {
	const struct hid_device *hid;
	__u32 allocated_size;
	union {
		__s32 retval;
		__s32 size;
	};
};

/**
 * enum hid_bpf_attach_flags - flags used when attaching a HIF-BPF program
 *
 * @HID_BPF_FLAG_NONE: no specific flag is used, the kernel choses where to
 *                     insert the program
 * @HID_BPF_FLAG_INSERT_HEAD: insert the given program before any other program
 *                            currently attached to the device. This doesn't
 *                            guarantee that this program will always be first
 */
enum hid_bpf_attach_flags {
	HID_BPF_FLAG_NONE = 0,
	HID_BPF_FLAG_INSERT_HEAD = _BITUL(0),

	/* private: internal use only */
	HID_BPF_FLAG_MAX,
};

/*
 * Below is HID internal
 */

#define HID_BPF_MAX_PROGS_PER_DEV 64
#define HID_BPF_FLAG_MASK (((HID_BPF_FLAG_MAX - 1) << 1) - 1)


struct hid_report_enum;

struct hid_ops {
	struct hid_report *(*hid_get_report)(struct hid_report_enum *report_enum, const u8 *data);
	int (*hid_hw_raw_request)(struct hid_device *hdev,
				  unsigned char reportnum, __u8 *buf,
				  size_t len, enum hid_report_type rtype,
				  enum hid_class_request reqtype);
	int (*hid_hw_output_report)(struct hid_device *hdev, __u8 *buf, size_t len);
	int (*hid_input_report)(struct hid_device *hid, enum hid_report_type type,
				u8 *data, u32 size, int interrupt);
	struct module *owner;
	const struct bus_type *bus_type;
};

extern struct hid_ops *hid_ops;

/**
 * struct hid_bpf_ops - A BPF struct_ops of callbacks allowing to attach HID-BPF
 *			programs to a HID device
 * @hid_id: the HID uniq ID to attach to. This is writeable before ``load()``, and
 *	    cannot be changed after
 * @flags: &enum hid_bpf_attach_flags to assign flags before ``load()``.
 *	   Writeable only before ``load()``
 */
struct hid_bpf_ops {
	/* hid_id needs to stay first so we can easily change it
	 * from userspace.
	 */
	int			hid_id;
	u32			flags;

	/* private: internal use only */
	struct list_head	list;

	/* public: rest is public */

/* fast path fields are put first to fill one cache line */

	/**
	 * @hid_device_event: called whenever an event is coming in from the device
	 *
	 * It has the following arguments:
	 *
	 * ``ctx``: The HID-BPF context as &struct hid_bpf_ctx
	 *
	 * Return: %0 on success and keep processing; a positive
	 * value to change the incoming size buffer; a negative
	 * error code to interrupt the processing of this event
	 *
	 * Context: Interrupt context.
	 */
	int (*hid_device_event)(struct hid_bpf_ctx *ctx, enum hid_report_type report_type);

/* control/slow paths put last */

	/**
	 * @hid_rdesc_fixup: called when the probe function parses the report descriptor
	 * of the HID device
	 *
	 * It has the following arguments:
	 *
	 * ``ctx``: The HID-BPF context as &struct hid_bpf_ctx
	 *
	 * Return: %0 on success and keep processing; a positive
	 * value to change the incoming size buffer; a negative
	 * error code to interrupt the processing of this device
	 */
	int (*hid_rdesc_fixup)(struct hid_bpf_ctx *ctx);

	/* private: internal use only */
	struct hid_device *hdev;
} ____cacheline_aligned_in_smp;

/* stored in each device */
struct hid_bpf {
	u8 *device_data;		/* allocated when a bpf program of type
					 * SEC(f.../hid_bpf_device_event) has been attached
					 * to this HID device
					 */
	u32 allocated_data;
	bool destroyed;			/* prevents the assignment of any progs */

	struct hid_bpf_ops *rdesc_ops;
	struct list_head prog_list;
	struct mutex prog_list_lock;	/* protects RCU update of prog_list */
};

#ifdef CONFIG_HID_BPF
u8 *dispatch_hid_bpf_device_event(struct hid_device *hid, enum hid_report_type type, u8 *data,
				  u32 *size, int interrupt);
int hid_bpf_connect_device(struct hid_device *hdev);
void hid_bpf_disconnect_device(struct hid_device *hdev);
void hid_bpf_destroy_device(struct hid_device *hid);
void hid_bpf_device_init(struct hid_device *hid);
u8 *call_hid_bpf_rdesc_fixup(struct hid_device *hdev, u8 *rdesc, unsigned int *size);
#else /* CONFIG_HID_BPF */
static inline u8 *dispatch_hid_bpf_device_event(struct hid_device *hid, enum hid_report_type type,
						u8 *data, u32 *size, int interrupt) { return data; }
static inline int hid_bpf_connect_device(struct hid_device *hdev) { return 0; }
static inline void hid_bpf_disconnect_device(struct hid_device *hdev) {}
static inline void hid_bpf_destroy_device(struct hid_device *hid) {}
static inline void hid_bpf_device_init(struct hid_device *hid) {}
#define call_hid_bpf_rdesc_fixup(_hdev, _rdesc, _size)	\
		((u8 *)kmemdup(_rdesc, *(_size), GFP_KERNEL))

#endif /* CONFIG_HID_BPF */

#endif /* __HID_BPF_H */
