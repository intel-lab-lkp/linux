// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/kmemdump.h>

#define MAX_ZONES 201

static int default_register_region(const struct kmemdump_backend *be,
				   enum kmemdump_uid id, void *area, size_t sz)
{
	return 0;
}

static int default_unregister_region(const struct kmemdump_backend *be,
				     enum kmemdump_uid id)
{
	return 0;
}

static const struct kmemdump_backend kmemdump_default_backend = {
	.name = "default",
	.register_region = default_register_region,
	.unregister_region = default_unregister_region,
};

static const struct kmemdump_backend *backend = &kmemdump_default_backend;
static DEFINE_MUTEX(kmemdump_lock);
static struct kmemdump_zone kmemdump_zones[MAX_ZONES];

static int __init init_kmemdump(void)
{
	const struct kmemdump_zone *e;

	/* Walk the kmemdump section for static variables and register them */
	for_each_kmemdump_entry(e)
		kmemdump_register_id(e->id, e->zone, e->size);

	return 0;
}
late_initcall(init_kmemdump);

/**
 * kmemdump_register_id() - Register region into kmemdump with given ID.
 * @req_id: Requested unique kmemdump_uid that identifies the region
 *	This can be KMEMDUMP_ID_NO_ID, in which case the function will
 *	find an unused ID and return it.
 * @zone: pointer to the zone of memory
 * @size: region size
 *
 * Return: On success, it returns the unique id for the region.
 *	 On failure, it returns negative error value.
 */
int kmemdump_register_id(enum kmemdump_uid req_id, void *zone, size_t size)
{
	struct kmemdump_zone *z;
	enum kmemdump_uid uid = req_id;
	int ret;

	if (uid < KMEMDUMP_ID_START)
		return -EINVAL;

	if (uid >= MAX_ZONES)
		return -ENOSPC;

	mutex_lock(&kmemdump_lock);

	if (uid == KMEMDUMP_ID_NO_ID)
		while (uid < MAX_ZONES) {
			if (!kmemdump_zones[uid].id)
				break;
			uid++;
		}

	if (uid == MAX_ZONES) {
		mutex_unlock(&kmemdump_lock);
		return -ENOSPC;
	}

	z = &kmemdump_zones[uid];

	if (z->id) {
		mutex_unlock(&kmemdump_lock);
		return -EALREADY;
	}

	ret = backend->register_region(backend, uid, zone, size);
	if (ret) {
		mutex_unlock(&kmemdump_lock);
		return ret;
	}

	z->zone = zone;
	z->size = size;
	z->id = uid;

	mutex_unlock(&kmemdump_lock);

	return uid;
}
EXPORT_SYMBOL_GPL(kmemdump_register_id);

/**
 * kmemdump_unregister() - Unregister region from kmemdump.
 * @id: unique id that was returned when this region was successfully
 *	registered initially.
 *
 * Return: None
 */
void kmemdump_unregister(enum kmemdump_uid id)
{
	struct kmemdump_zone *z = NULL;

	mutex_lock(&kmemdump_lock);

	z = &kmemdump_zones[id];
	if (!z->id) {
		mutex_unlock(&kmemdump_lock);
		return;
	}

	backend->unregister_region(backend, z->id);

	memset(z, 0, sizeof(*z));

	mutex_unlock(&kmemdump_lock);
}
EXPORT_SYMBOL_GPL(kmemdump_unregister);

/**
 * kmemdump_register_backend() - Register a backend into kmemdump.
 * @be: Pointer to a driver allocated backend. This backend must have
 *	two callbacks for registering and deregistering a zone from the
 *	backend.
 *
 * Only one backend is supported at a time.
 *
 * Return: On success, it returns 0, negative error value otherwise.
 */
int kmemdump_register_backend(const struct kmemdump_backend *be)
{
	enum kmemdump_uid uid;
	int ret;

	if (!be || !be->register_region || !be->unregister_region)
		return -EINVAL;

	mutex_lock(&kmemdump_lock);

	/* Try to call the old backend for all existing regions */
	for (uid = KMEMDUMP_ID_START; uid < MAX_ZONES; uid++)
		if (kmemdump_zones[uid].id)
			backend->unregister_region(backend,
						   kmemdump_zones[uid].id);

	backend = be;
	pr_debug("kmemdump backend %s registered successfully.\n",
		 backend->name);

	/* Call the new backend for all existing regions */
	for (uid = KMEMDUMP_ID_START; uid < MAX_ZONES; uid++) {
		if (!kmemdump_zones[uid].id)
			continue;
		ret = backend->register_region(backend,
					       kmemdump_zones[uid].id,
					       kmemdump_zones[uid].zone,
					       kmemdump_zones[uid].size);
		if (ret)
			pr_debug("register region failed with %d\n", ret);
	}

	mutex_unlock(&kmemdump_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(kmemdump_register_backend);

/**
 * kmemdump_unregister_backend() - Unregister the backend from kmemdump.
 * @be: Pointer to a driver allocated backend. This backend must match
 *	the initially registered backend.
 *
 * Only one backend is supported at a time.
 * Before deregistering, this will call the backend to unregister all the
 * previously registered zones.
 *
 * Return: None
 */
void kmemdump_unregister_backend(const struct kmemdump_backend *be)
{
	enum kmemdump_uid uid;

	mutex_lock(&kmemdump_lock);

	if (backend != be) {
		mutex_unlock(&kmemdump_lock);
		return;
	}

	/* Try to call the old backend for all existing regions */
	for (uid = KMEMDUMP_ID_START; uid < MAX_ZONES; uid++)
		if (kmemdump_zones[uid].id)
			backend->unregister_region(backend,
						   kmemdump_zones[uid].id);

	pr_debug("kmemdump backend %s removed successfully.\n", be->name);

	backend = &kmemdump_default_backend;

	mutex_unlock(&kmemdump_lock);
}
EXPORT_SYMBOL_GPL(kmemdump_unregister_backend);

