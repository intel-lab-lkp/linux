/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARM64_PAGE_TRACKING_DEVICE_H
#define _ARM64_PAGE_TRACKING_DEVICE_H

#include <linux/types.h>
#include <linux/kvm_types.h>

/* Page tracking mode */
enum pt_mode {
	dirty_pages,
};

/* Configuration of a per-VM page tracker */
struct pt_config {
	enum pt_mode mode; /* Tracking mode */
	u32 vmid;	/* VMID to track */
};

/* Interface provided by the page tracking device */
struct page_tracking_device {

	/* Allocates a per-VM tracker, returns tracking context */
	void* (*allocate_tracker)(struct pt_config config);

	/* Releases a per-VM tracker */
	int (*release_tracker)(void *ctx);

	/*
	 * Enables tracking for the specified @ctx and the specified @cpu,
	 * @cpu = -1 enables tracking for all cpus
	 *
	 * The function may be called for the same @ctx and @cpu multiple
	 * times and the implementation has to do reference counting to
	 * correctly disable the tracking.
	 * @returns 0 on success, negative errno in case of a failure
	 */
	int (*enable_tracking)(void *ctx, int cpu);

	/*
	 * Disables tracking for the @ctx
	 *
	 * Does actually disable the tracking of the @ctx and the @cpu only
	 * when the number of disable and enable calls matches, i.e. when the
	 * reference counter is at 0. @returns 0 in this case, -EBUSY while
	 * reference counter > 0 and negative errno in case of a failure
	 */
	int (*disable_tracking)(void *ctx, int cpu);

	/*
	 * Flushes any tracking data available for the @ctx,
	 * @returns 0 on success, negative errno in case of a failure
	 */
	int (*flush)(void *ctx);

	/*
	 * Reads up to @max dirty pages available for the @ctx
	 * In case @cpu id is not -1, reads only pages dirtied by the specified cpu
	 * @returns number of read pages and -errno in case of a failure
	 */
	int (*read_dirty_pages)(void *ctx,
				int cpu,
				gpa_t *pages,
				u32 max);
};

/* Page tracking device tear-down, bring-up and existence checks */
void page_tracking_device_unregister(struct page_tracking_device *pt_dev);
int page_tracking_device_register(struct page_tracking_device *pt_dev);
int page_tracking_device_registered(void);

/* Page tracking device wrappers */
void *page_tracking_allocate(struct pt_config config);
int page_tracking_release(void *ctx);
int page_tracking_enable(void *ctx, int cpu);
int page_tracking_disable(void *ctx, int cpu);
int page_tracking_flush(void *ctx);
int page_tracking_read_dirty_pages(void *ctx, int cpu, gpa_t *pages, u32 max);

#endif /*_ARM64_PAGE_TRACKNG_DEVICE_H */
