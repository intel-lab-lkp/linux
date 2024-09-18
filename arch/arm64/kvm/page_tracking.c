// SPDX-License-Identifier: GPL-2.0
#include <asm/page_tracking.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>

#ifndef CONFIG_HAVE_KVM_PAGE_TRACKING_DEVICE

int page_tracking_device_register(struct page_tracking_device *dev) { return 0; }
void page_tracking_device_unregister(struct page_tracking_device *dev) {}
int page_tracking_device_registered(void) { return 0; }
void *page_tracking_allocate(struct pt_config config) { return NULL; }
int page_tracking_release(void *ctx) { return 0; }
int page_tracking_enable(void *ctx, int cpu) { return 0; }
int page_tracking_disable(void *ctx, int cpu) { return 0; }
int page_tracking_flush(void *ctx) { return 0; }
int page_tracking_read_dirty_pages(void *ctx, int cpu, gpa_t *pages, u32 max) { return 0; }

#else

static DEFINE_MUTEX(page_tracking_device_mutex);
static struct page_tracking_device __rcu *pt_dev __read_mostly;

int page_tracking_device_register(struct page_tracking_device *dev)
{
	int rc = 0;

	mutex_lock(&page_tracking_device_mutex);

	if (rcu_dereference_protected(pt_dev, lockdep_is_held(&page_tracking_device_mutex))) {
		rc = -EBUSY;
		goto out;
	}
	rcu_assign_pointer(pt_dev, dev);
out:
	mutex_unlock(&page_tracking_device_mutex);
	return rc;
}
EXPORT_SYMBOL_GPL(page_tracking_device_register);

void page_tracking_device_unregister(struct page_tracking_device *dev)
{
	mutex_lock(&page_tracking_device_mutex);

	if (dev == rcu_dereference_protected(pt_dev,
					     lockdep_is_held(&page_tracking_device_mutex))) {
		/* Disable page tracking device */
		RCU_INIT_POINTER(pt_dev, NULL);
		synchronize_rcu();
	}
	mutex_unlock(&page_tracking_device_mutex);
}
EXPORT_SYMBOL_GPL(page_tracking_device_unregister);

int page_tracking_device_registered(void)
{
	bool registered;

	rcu_read_lock();
	registered = (rcu_dereference(pt_dev) != NULL);
	rcu_read_unlock();
	return registered;
}
EXPORT_SYMBOL_GPL(page_tracking_device_registered);

/* Allocates a per-VM tracker, returns tracking context */
void *page_tracking_allocate(struct pt_config config)
{
	struct page_tracking_device *dev;
	void *ctx = NULL;

	rcu_read_lock();
	dev = rcu_dereference(pt_dev);
	if (likely(dev))
		ctx = dev->allocate_tracker(config);
	rcu_read_unlock();
	return ctx;
}
EXPORT_SYMBOL_GPL(page_tracking_allocate);

/* Releases a per-VM tracker */
int page_tracking_release(void *ctx)
{
	int r;
	struct page_tracking_device *dev;

	rcu_read_lock();
	dev = rcu_dereference(pt_dev);
	if (likely(dev))
		r = dev->release_tracker(ctx);
	rcu_read_unlock();
	return r;
}
EXPORT_SYMBOL_GPL(page_tracking_release);

/* Enables tracking for the specified @ctx and @cpu (-1 for all cpus) */
int page_tracking_enable(void *ctx, int cpu)
{
	int r;
	struct page_tracking_device *dev;

	rcu_read_lock();
	dev = rcu_dereference(pt_dev);
	if (likely(dev))
		r = dev->enable_tracking(ctx, cpu);
	rcu_read_unlock();
	return r;
}
EXPORT_SYMBOL_GPL(page_tracking_enable);

/* Disables tracking for the @ctx and @cpu */
int page_tracking_disable(void *ctx, int cpu)
{
	int r;
	struct page_tracking_device *dev;

	rcu_read_lock();
	dev = rcu_dereference(pt_dev);
	if (likely(dev))
		r = dev->disable_tracking(ctx, cpu);
	rcu_read_unlock();
	return r;
}
EXPORT_SYMBOL_GPL(page_tracking_disable);

/* Flushes any available data */
int page_tracking_flush(void *ctx)
{
	int r;
	struct page_tracking_device *dev;

	rcu_read_lock();
	dev = rcu_dereference(pt_dev);
	if (likely(dev))
		r = dev->flush(ctx);
	rcu_read_unlock();
	return r;
}
EXPORT_SYMBOL_GPL(page_tracking_flush);

/*
 * Reads up to @max dirty pages available for the @ctx and @cpu (-1 for all cpus)
 * @returns number of read pages and -errno in case of error
 */
int page_tracking_read_dirty_pages(void *ctx, int cpu, gpa_t *pages, u32 max)
{
	int r;
	struct page_tracking_device *dev;

	rcu_read_lock();
	dev = rcu_dereference(pt_dev);
	if (likely(dev))
		r = dev->read_dirty_pages(ctx, cpu, pages, max);
	rcu_read_unlock();
	return r;
}
EXPORT_SYMBOL_GPL(page_tracking_read_dirty_pages);

#endif
