/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Copyright (c) Jarkko Sakkinen 2025-2026
 */

#ifndef _UAPI_LINUX_VCAM_H
#define _UAPI_LINUX_VCAM_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define VCAM_IOC_BASE 'v'

/**
 * DOC: vcam uAPI
 *
 * The ioctl API of /dev/vcam provides ioctls for creating DMA-BUF backed
 * virtual capture devices, and pushing image frames for consumption.
 *
 * Frames are queued with %VCAM_IOC_QUEUE and recycled with %VCAM_IOC_DEQUEUE.
 * Queueing without dequeuing eventually exhausts the output queue.
 */

/**
 * enum vcam_status - Status bits
 * @VCAM_STATUS_IDLE: Capture queue is not streaming.
 * @VCAM_STATUS_STREAMING: Capture queue is streaming.
 */
enum vcam_status {
	VCAM_STATUS_IDLE = 1U << 0,
	VCAM_STATUS_STREAMING = 1U << 1,
};

/**
 * struct vcam_ioc_create - Create a virtual camera device
 * @device_name: (input) User pointer to device name string.
 * @width: (input) Frame width in pixels. Must be non-zero.
 * @height: (input) Frame height in pixels. Must be non-zero.
 * @pixelformat: (input) Four CC format code.
 * @colorspace: (input) V4L2 colorspace value.
 * @bytesperline: (input) Bytes per line in the output format.
 * @reserved: Reserved for future use. Must be set to zero.
 * @device_nr: (output) Device number (must be 0 on input).
 * @nr_frames: (input) Number of entries in @frames.
 * @frames: (input/output) User pointer to an array of &struct vcam_frame.
 */
struct vcam_ioc_create {
	__u64 device_name;
	__u32 width;
	__u32 height;
	__u32 pixelformat;
	__u32 colorspace;
	__u32 bytesperline;
	__u32 reserved;
	__u32 device_nr;
	__u32 nr_frames;
	__u64 frames;
};

/**
 * struct vcam_frame - a frame descriptor
 * @index: Frame index assigned by the driver.
 * @length: Frame size in bytes.
 */
struct vcam_frame {
	__u32 index;
	__u32 length;
};

/**
 * struct vcam_ioc_queue - Produce an output buffer
 * @fd: (input) DMA-BUF file descriptor.
 * @index: (input) Buffer index for %VCAM_IOC_QUEUE.
 * @length: (input) Payload length in bytes for %VCAM_IOC_QUEUE.
 * @reserved: Reserved for future use. Must be set to zero.
 * @timestamp: (input) Timestamp in nanoseconds for %VCAM_IOC_QUEUE.
 */
struct vcam_ioc_queue {
	__u32 fd;
	__u32 index;
	__u32 length;
	__u32 reserved;
	__u64 timestamp;
};

/**
 * struct vcam_ioc_dequeue - Dequeue an output buffer
 * @index: (output) Buffer index for %VCAM_IOC_DEQUEUE.
 * @length: (output) Payload length in bytes for %VCAM_IOC_DEQUEUE.
 * @timestamp: (output) Timestamp in nanoseconds for %VCAM_IOC_DEQUEUE.
 */
struct vcam_ioc_dequeue {
	__u32 index;
	__u32 length;
	__u64 timestamp;
};

/**
 * struct vcam_ioc_wait - Wait for capture status
 * @mask: (input) Mask of status bits to wait for.
 * @status: (output) Current status bit mask.
 */
struct vcam_ioc_wait {
	__u64 mask;
	__u64 status;
};

/**
 * DOC: vcam ioctls
 *
 * %VCAM_IOC_CREATE: Creates a virtual camera device and associates output
 * buffers described by &struct vcam_frame with DMA-BUF file descriptors.
 * %VCAM_IOC_QUEUE: Enqueues an output buffer for capture.
 * %VCAM_IOC_DEQUEUE: Dequeues a consumed output buffer for reuse.
 * %VCAM_IOC_STATUS: Reads the driver status bits.
 * %VCAM_IOC_WAIT: Waits for the subset of status bits to activate.
 */
#define VCAM_IOC_CREATE _IOWR(VCAM_IOC_BASE, 0x00, struct vcam_ioc_create)
#define VCAM_IOC_QUEUE _IOW(VCAM_IOC_BASE, 0x01, struct vcam_ioc_queue)
#define VCAM_IOC_DEQUEUE _IOR(VCAM_IOC_BASE, 0x02, struct vcam_ioc_dequeue)
#define VCAM_IOC_STATUS _IOR(VCAM_IOC_BASE, 0x03, __u64)
#define VCAM_IOC_WAIT _IOWR(VCAM_IOC_BASE, 0x04, struct vcam_ioc_wait)

#endif /* _UAPI_LINUX_VCAM_H */
