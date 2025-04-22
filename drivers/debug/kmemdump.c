// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/kmemdump.h>
#include <linux/idr.h>

#define MAX_ZONES 512

static struct kmemdump_backend *backend;
static DEFINE_IDR(kmemdump_idr);
static DEFINE_MUTEX(kmemdump_lock);
static LIST_HEAD(kmemdump_list);

/**
 * kmemdump_register() - Register region into kmemdump.
 * @handle: string of maximum 8 chars that identifies this region
 * @zone: pointer to the zone of memory
 * @size: region size
 *
 * Return: On success, it returns an allocated unique id that can be used
 *	at a later point to identify the region. On failure, it returns
 *	negative error value.
 */
int kmemdump_register(char *handle, void *zone, size_t size)
{
	struct kmemdump_zone *z = kzalloc(sizeof(*z), GFP_KERNEL);
	int id;

	if (!z)
		return -ENOMEM;

	mutex_lock(&kmemdump_lock);

	id = idr_alloc_cyclic(&kmemdump_idr, z, 0, MAX_ZONES, GFP_KERNEL);
	if (id < 0) {
		mutex_unlock(&kmemdump_lock);
		return id;
	}

	if (!backend)
		pr_debug("kmemdump backend not available yet, waiting...\n");

	z->zone = zone;
	z->size = size;
	z->id = id;

	if (handle)
		strscpy(z->handle, handle, 8);

	if (backend) {
		int ret;

		ret = backend->register_region(id, handle, zone, size);
		if (ret) {
			mutex_unlock(&kmemdump_lock);
			return ret;
		}
		z->registered = true;
		update_elfheader(z);
	}

	mutex_unlock(&kmemdump_lock);
	return id;
}
EXPORT_SYMBOL_GPL(kmemdump_register);

/**
 * kmemdump_unregister() - Unregister region from kmemdump.
 * @id: unique id that was returned when this region was successfully
 *	registered initially.
 *
 * Return: None
 */
void kmemdump_unregister(int id)
{
	struct kmemdump_zone *z;

	mutex_lock(&kmemdump_lock);

	z = idr_find(&kmemdump_idr, id);
	if (!z)
		return;
	if (z->registered && backend)
		backend->unregister_region(z->id);

	clear_elfheader(z);
	idr_remove(&kmemdump_idr, id);
	kfree(z);

	mutex_unlock(&kmemdump_lock);
}
EXPORT_SYMBOL_GPL(kmemdump_unregister);

static int kmemdump_register_fn(int id, void *p, void *data)
{
	struct kmemdump_zone *z = p;
	int ret;

	if (z->registered)
		return 0;

	ret = backend->register_region(z->id, z->handle, z->zone, z->size);
	if (ret)
		return ret;
	z->registered = true;
	update_elfheader(z);

	return 0;
}

/**
 * kmemdump_register_backend() - Register a backend into kmemdump.
 * Only one backend is supported at a time.
 * @be: Pointer to a driver allocated backend. This backend must have
 *	two callbacks for registering and deregistering a zone from the
 *	backend.
 *
 * Return: On success, it returns 0, negative error value otherwise.
 */
int kmemdump_register_backend(struct kmemdump_backend *be)
{
	mutex_lock(&kmemdump_lock);

	if (backend)
		return -EALREADY;

	if (!be || !be->register_region || !be->unregister_region)
		return -EINVAL;

	backend = be;
	pr_info("kmemdump backend %s registered successfully.\n",
		backend->name);

	init_elfheader(backend);

	mutex_unlock(&kmemdump_lock);

	register_coreinfo();

	mutex_lock(&kmemdump_lock);

	/* Try to call the backend for all previously requested zones */
	idr_for_each(&kmemdump_idr, kmemdump_register_fn, NULL);

	mutex_unlock(&kmemdump_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(kmemdump_register_backend);

static int kmemdump_unregister_fn(int id, void *p, void *data)
{
	int ret;
	struct kmemdump_zone *z = p;

	if (!z->registered)
		return 0;

	ret = backend->unregister_region(z->id);
	if (ret)
		return ret;
	z->registered = false;
	clear_elfheader(z);

	return 0;
}

/**
 * kmemdump_register_backend() - Unregister the backend from kmemdump.
 * Only one backend is supported at a time.
 * Before deregistering, this will call the backend to unregister all the
 * previously registered zones.
 * @be: Pointer to a driver allocated backend. This backend must match
 *	the initially registered backend.
 *
 * Return: None
 */
void kmemdump_unregister_backend(struct kmemdump_backend *be)
{
	mutex_lock(&kmemdump_lock);

	if (backend != be) {
		mutex_unlock(&kmemdump_lock);
		return;
	}

	/* Try to call the backend for all previously requested zones */
	idr_for_each(&kmemdump_idr, kmemdump_unregister_fn, NULL);

	backend = NULL;
	pr_info("kmemdump backend %s removed successfully.\n", be->name);

	mutex_unlock(&kmemdump_lock);
}
EXPORT_SYMBOL_GPL(kmemdump_unregister_backend);
