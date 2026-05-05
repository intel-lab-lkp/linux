// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/parser.h>

#include "rmr-srv.h"
#include "rmr-map.h"
#include "rmr-clt.h"

#define MAX_POOL_ID 255

static struct class *rmr_dev_class;
static struct device *rmr_ctl_dev;
static struct device *rmr_pool_dev;

static struct kobj_type rmr_srv_sess_ktype = {
	.sysfs_ops      = &kobj_sysfs_ops,
};

int rmr_srv_sysfs_add_sess(struct rmr_pool *pool,
			   struct rmr_srv_pool_sess *pool_sess)
{
	int ret;

	ret = kobject_init_and_add(&pool_sess->kobj, &rmr_srv_sess_ktype,
				   &pool->sessions_kobj, "%s",
				   pool_sess->sessname);
	if (ret)
		pr_err("Failed to add session %s into sysfs\n",
		       pool_sess->sessname);

	return ret;
}

void rmr_srv_sysfs_del_sess(struct rmr_srv_pool_sess *pool_sess)
{
	kobject_del(&pool_sess->kobj);
	kobject_put(&pool_sess->kobj);
}

static ssize_t rmr_srv_member_id_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *page)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	return sprintf(page, "%d\n", srv_pool->member_id);
}

static struct kobj_attribute rmr_srv_member_id_attr =
	__ATTR(member_id, 0444, rmr_srv_member_id_show, NULL);

static ssize_t rmr_srv_pool_blksize_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *page)
{
	/* TODO: introduce blksize for pool */
	return sprintf(page, "128k\n");
}

static struct kobj_attribute rmr_srv_pool_blksize_attr =
	__ATTR(blksize, 0444, rmr_srv_pool_blksize_show, NULL);

static ssize_t rmr_srv_leave_pool_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 > %s\n",
					 attr->attr.name);
}

void rmr_srv_destroy_pool_sysfs_files(struct rmr_pool *pool,
				      const struct attribute *sysfs_self)
{
	if (pool->kobj.state_in_sysfs) {
		WARN_ON(!list_empty(&pool->sess_list));
		kobject_del(&pool->sessions_kobj);
		kobject_put(&pool->sessions_kobj);
		if (sysfs_self)
			sysfs_remove_file_self(&pool->kobj, sysfs_self);
		kobject_del(&pool->kobj);
		kobject_put(&pool->kobj);
	}
}

static ssize_t rmr_srv_leave_pool_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	if (READ_ONCE(srv_pool->io_store)) {
		pr_err("pool %s has a store registered\n", pool->poolname);
		return -EINVAL;
	}

	if (atomic_read(&srv_pool->state) != RMR_SRV_POOL_STATE_EMPTY) {
		pr_err("pool %s cannot leave: not in EMPTY state (state=%d)\n",
		       pool->poolname, atomic_read(&srv_pool->state));
		return -EINVAL;
	}

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s, %s unknown value: '%s'\n",
		       pool->poolname, attr->attr.name, buf);
		return -EINVAL;
	}

	if (srv_pool->clt) {
		int err;

		err = rmr_srv_remove_clt_pool(srv_pool);
		if (err) {
			pr_err("pool %s failed to remove clt_pool\n", pool->poolname);
			return -EINVAL;
		}
	}
	pr_info("srv: Deleting pool '%s'\n", pool->poolname);

	rmr_srv_destroy_pool(pool);
	rmr_srv_destroy_pool_sysfs_files(pool, &attr->attr);
	rmr_put_srv_pool(srv_pool);

	return count;
}

static struct kobj_attribute rmr_srv_leave_pool_attr =
		__ATTR(leave_pool, 0644, rmr_srv_leave_pool_show,
		       rmr_srv_leave_pool_store);

static ssize_t rmr_srv_pool_map_show(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *page)
{
	struct rmr_pool *pool = container_of(kobj, struct rmr_pool, kobj);
	struct rmr_dirty_id_map *map;
	int i, lock_idx;

	lock_idx = srcu_read_lock(&pool->map_srcu);
	for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
		map = rcu_dereference(pool->maps[i]);
		if (!map)
			continue;

		rmr_map_dump_bitmap(map);
	}
	srcu_read_unlock(&pool->map_srcu, lock_idx);

	return 0;
}

static ssize_t rmr_srv_pool_map_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	struct rmr_pool *pool;
	rmr_id_t id = { 0, 0 };
	int srv_id;
	struct rmr_dirty_id_map *map;

	pool = container_of(kobj, struct rmr_pool, kobj);
	if (sscanf(buf, "%llu %llu %d\n", &id.a, &id.b, &srv_id) != 3) {
		pr_err("cannot parse id.a %s\n", buf);
		return -EINVAL;
	}
	pr_debug("Add id (%llu, %llu), srv_id %d\n", id.a, id.b, srv_id);

	/*
	 * If given chunk number exceeds total chunks for us, ignore!
	 */
	if (id.b > pool->no_of_chunks)
		return count;

	map = rmr_pool_find_map(pool, srv_id);
	if (!map) {
		pr_err("in pool %s cannot find map for srv_id %u\n",
		       pool->poolname, srv_id);
		return -EINVAL;
	}

	rmr_map_set_dirty(map, id, 0);
	rmr_srv_mark_maps_dirty((struct rmr_srv_pool *)pool->priv);
	pr_debug("insert id (%llu, %llu) srv_id %d\n", id.a, id.b, srv_id);

	return count;
}

static struct kobj_attribute rmr_srv_pool_map_attr =
	__ATTR(map, 0644, rmr_srv_pool_map_show,
	       rmr_srv_pool_map_store);

static ssize_t rmr_srv_pool_map_ver_show(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *page)
{
	struct rmr_pool *pool;
	ssize_t written;

	pool = container_of(kobj, struct rmr_pool, kobj);

	written = scnprintf(page, PAGE_SIZE, "Map ver: %llu\n", pool->map_ver);

	return written;
}

static struct kobj_attribute rmr_srv_pool_map_ver_attr =
	__ATTR(map_version, 0444, rmr_srv_pool_map_ver_show, NULL);

static ssize_t rmr_srv_pool_last_io_show(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *page)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	ssize_t written = 0;
	int i;
	rmr_id_t *id;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	for (i = 0; i < srv_pool->queue_depth; i++) {
		id = &srv_pool->last_io[i];

		if (id->a == U64_MAX && id->b == U64_MAX)
			continue;

		written += scnprintf(page + written, PAGE_SIZE - written,
				     "[%d]=(%llu,%llu) ", i, id->a, id->b);
	}
	if (written == 0)
		written += scnprintf(page + written, PAGE_SIZE - written,
				     "(empty)");
	written += scnprintf(page + written, PAGE_SIZE - written, "\n");

	return written;
}

static struct kobj_attribute rmr_srv_pool_last_io_attr =
	__ATTR(last_io, 0644, rmr_srv_pool_last_io_show, NULL);

static ssize_t rmr_srv_add_clt_pool_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo poolname > %s\n",
			 attr->attr.name);
}

static ssize_t rmr_srv_add_clt_pool_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	struct rmr_pool *clt = NULL;
	char name[NAME_MAX];
	int err;
	struct rmr_attrs attrs;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	if (sscanf(buf, "%s", name) != 1) {
		pr_err("cannot parse %s\n", buf);
		return -EINVAL;
	}

	clt = rmr_clt_open(NULL, NULL, name);
	if (IS_ERR_OR_NULL(clt)) {
		pr_err("cannot open pool %s err %ld\n", name, PTR_ERR(clt));
		return -EEXIST;
	}

	pr_info("%s: Adding client pool %s, to server pool %s\n",
		__func__, pool->poolname, clt->poolname);

	err = rmr_clt_query(clt, &attrs);
	if (unlikely(err))
		goto close_rmr;

	if (!attrs.sync) {
		pr_err("%s: Add clt called for non-sync rmr client pool %s\n", __func__, name);
		err = -EINVAL;
		goto close_rmr;
	}

	srv_pool->max_sync_io_size = attrs.max_io_size;

	/* The sync client holds a pointer to its parent server pool. */
	srv_pool->clt = clt;

	/* Re-trigger md sync now that the sync path is available. */
	rmr_srv_mark_pool_md_dirty(srv_pool);

	/*
	 * Check if the device paramters of connected servers share the same values.
	 */
	err = rmr_srv_check_params(srv_pool);
	if (err)
		goto close_clt;

	return count;

close_clt:
	srv_pool->clt = NULL;
	srv_pool->max_sync_io_size = 0;
close_rmr:
	pr_err("%s: Adding client pool failed\n", __func__);
	rmr_clt_close(clt);
	return err;
}

static struct kobj_attribute rmr_srv_add_clt_pool_attr =
	__ATTR(add_clt, 0644, rmr_srv_add_clt_pool_show,
	       rmr_srv_add_clt_pool_store);

static ssize_t rmr_srv_pool_sync_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *page)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	return scnprintf(page, PAGE_SIZE, "Usage: echo \"start|stop\" > <path_to_pool>/%s\n",
					  attr->attr.name);
}

static ssize_t rmr_srv_pool_sync_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	int err = 0;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	if (!strncasecmp(buf, "start", 5)) {
		/*
		 * Start
		 */
		if (atomic_read(&srv_pool->thread_state) != SYNC_THREAD_STOPPED) {
			pr_info("For pool %s, sync thread already running\n", pool->poolname);
			goto out;
		}

		mutex_lock(&srv_pool->srv_pool_lock);

		if (!atomic_read(&srv_pool->store_state) &&
		    atomic_read(&srv_pool->state) != RMR_SRV_POOL_STATE_NORMAL) {
			pr_err("Pool %s not in working state. Sync thread start failed\n",
			       pool->poolname);
			err = -EINVAL;
			goto unlock_mutex;
		}

		err = rmr_srv_sync_thread_start(srv_pool);
		if (err) {
			pr_err("For pool %s, rmr_srv_sync_thread_start Error %d\n",
			       pool->poolname, err);
			goto unlock_mutex;
		}

		mutex_unlock(&srv_pool->srv_pool_lock);

	} else if (!strncasecmp(buf, "stop", 4)) {
		/*
		 * Stop
		 */
		if (atomic_read(&srv_pool->thread_state) == SYNC_THREAD_STOPPED) {
			pr_info("For pool %s, sync thread already stopped\n", pool->poolname);
			goto out;
		}

		err = rmr_srv_sync_thread_stop(srv_pool);
		if (err) {
			pr_err("For pool %s, rmr_srv_sync_thread_stop Error %d\n",
			       pool->poolname, err);
			goto err;
		}
	} else {
		pr_err("Unknown value\n");
		err = -EINVAL;
		goto err;
	}

out:
	return count;

unlock_mutex:
	mutex_unlock(&srv_pool->srv_pool_lock);
err:
	return err;
}

static struct kobj_attribute rmr_srv_pool_sync_attr =
	__ATTR(sync, 0644, rmr_srv_pool_sync_show,
	       rmr_srv_pool_sync_store);

static ssize_t sync_state_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *page)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	int state;
	ssize_t written = 0;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	state = atomic_read(&srv_pool->thread_state);
	switch (state) {
	case SYNC_THREAD_RUNNING:
		written = sysfs_emit(page, "Running\n");
		break;
	case SYNC_THREAD_STOPPED:
		written = sysfs_emit(page, "Stopped\n");
		break;
	case SYNC_THREAD_REQ_STOP:
		written = sysfs_emit(page, "Request_to_stop\n");
		break;
	case SYNC_THREAD_WAIT:
		written = sysfs_emit(page, "Wait\n");
		break;
	default:
		written = sysfs_emit(page, "Unknown value %d\n", state);
		break;
	}

	return written;
}

static struct kobj_attribute rmr_srv_pool_sync_state_attr =
	__ATTR_RO(sync_state);

static ssize_t rmr_srv_pool_state_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *page)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	int state;
	ssize_t written = 0;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;
	state = atomic_read(&srv_pool->state);

	switch (state) {
	case RMR_SRV_POOL_STATE_EMPTY:
		written = sysfs_emit(page, "empty\n");

		break;
	case RMR_SRV_POOL_STATE_REGISTERED:
		written = sysfs_emit(page, "registered\n");

		break;
	case RMR_SRV_POOL_STATE_CREATED:
		written = sysfs_emit(page, "created\n");

		break;
	case RMR_SRV_POOL_STATE_NORMAL:
		written = sysfs_emit(page, "normal\n");

		break;
	case RMR_SRV_POOL_STATE_NO_IO:
		written = sysfs_emit(page, "no_io\n");

		break;
	default:
		written = sysfs_emit(page, "Unknown value %d\n", state);

		break;
	}

	written += sysfs_emit_at(page, written, "Maintenance mode: %d\n",
				 srv_pool->maintenance_mode);

	return written;
}

static struct kobj_attribute rmr_srv_pool_state_attr =
	__ATTR(state, 0644, rmr_srv_pool_state_show, NULL);

static ssize_t rmr_srv_remove_clt_pool_show(struct kobject *kobj,
					    struct kobj_attribute *attr, char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 > %s\n",
			 attr->attr.name);
}

static ssize_t rmr_srv_remove_clt_pool_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	int err;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s, %s unknown value: '%s'\n",
		       pool->poolname, attr->attr.name, buf);
		return -EINVAL;
	}
	err = rmr_srv_remove_clt_pool(srv_pool);
	if (err) {
		pr_err("pool %s failed to remove clt_pool\n", pool->poolname);
		return -EINVAL;
	}

	return count;
}

static struct kobj_attribute rmr_srv_remove_clt_pool_attr =
	__ATTR(remove_clt, 0644, rmr_srv_remove_clt_pool_show,
	       rmr_srv_remove_clt_pool_store);

static ssize_t rmr_srv_pool_test_map_show(struct kobject *kobj,
						struct kobj_attribute *attr,
						char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 > %s\n",
			 attr->attr.name);
}

static ssize_t rmr_srv_pool_test_map_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	int err;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s, %s unknown value: '%s'\n",
		       pool->poolname, attr->attr.name, buf);
		return -EINVAL;
	}

	if (!srv_pool->clt) {
		pr_err("pool %s no clt pool assigned to this rmr pool. cannot do map test.\n",
		       pool->poolname);
		return -EINVAL;
	}

	pr_info("pool %s start test map...\n", pool->poolname);
	err = rmr_clt_test_map(pool, srv_pool->clt);
	if (err) {
		pr_err("pool %s, test map failed, err %d\n",
		       pool->poolname, err);
		return err;
	}
	pr_info("pool %s test map done.", pool->poolname);

	return count;
}

static struct kobj_attribute rmr_srv_pool_test_map_attr =
	__ATTR(test_map, 0644, rmr_srv_pool_test_map_show,
	       rmr_srv_pool_test_map_store);

static ssize_t rmr_srv_pool_metadata_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *page)
{
	struct rmr_pool *pool;
	struct rmr_pool_md *pool_md;
	struct rmr_srv_md *srv_md;
	int i;
	ssize_t written = 0;

	pool = container_of(kobj, struct rmr_pool, kobj);
	pool_md = &pool->pool_md;

	written += sysfs_emit_at(page, written,
				 "The metadata of %s is: group_id %u, chunk_size %u, "
				 "mapped_size %llu, queue_depth %u, "
				 "bitmap_offset %llu, bitmap_len %llu, "
				 "last_io_offset %llu, last_io_len %llu\n\n",
				 pool_md->poolname, pool_md->group_id, pool_md->chunk_size,
				 pool_md->mapped_size, pool_md->queue_depth,
				 rmr_bitmap_offset(pool_md->queue_depth),
				 rmr_bitmap_len(pool->no_of_chunks),
				 (u64)RMR_LAST_IO_OFFSET,
				 rmr_last_io_len(pool_md->queue_depth));
	written += sysfs_emit_at(page, written,
				 "The client pool: map_ver %llu\n\n", pool_md->map_ver);

	for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
		srv_md = &pool_md->srv_md[i];
		if (!srv_md->member_id)
			continue;

		written += sysfs_emit_at(page, written, "The server pool with member_id %u: "
					 "mapped_size %llu, store_state %u, "
					 "pool_state %u, map_update_state %u, "
					 "map_ver %llu, discard_entries %x.\n\n",
					 srv_md->member_id, srv_md->mapped_size,
					 srv_md->store_state,
					 srv_md->srv_pool_state,
					 srv_md->map_update_state, srv_md->map_ver,
					 srv_md->discard_entries);
	}

	return written;
}

static struct kobj_attribute rmr_srv_pool_metadata_attr =
	__ATTR(metadata, 0444, rmr_srv_pool_metadata_show, NULL);

static const char *map_update_state_str(enum srv_map_update_state state)
{
	switch (state) {
	case MAP_UPDATE_STATE_DISABLED:
		return "disabled";
	case MAP_UPDATE_STATE_READY:
		return "ready";
	case MAP_UPDATE_STATE_DONE:
		return "done";
	}
	return "unknown";
}

static ssize_t rmr_srv_pool_map_update_state_show(struct kobject *kobj,
						  struct kobj_attribute *attr,
						  char *page)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;

	pool = container_of(kobj, struct rmr_pool, kobj);
	srv_pool = (struct rmr_srv_pool *)pool->priv;

	return sysfs_emit(page, "%s\n", map_update_state_str(srv_pool->map_update_state));
}

static struct kobj_attribute rmr_srv_pool_map_update_state_attr =
	__ATTR(map_update_state, 0644, rmr_srv_pool_map_update_state_show, NULL);

static ssize_t rmr_srv_pool_map_unsynced_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *page)
{
	ssize_t written = 0;
	struct rmr_pool *pool;
	struct rmr_dirty_id_map *map;
	rmr_id_t id;
	int i, j, lock_idx;

	pool = container_of(kobj, struct rmr_pool, kobj);

	id.a = 1;
	lock_idx = srcu_read_lock(&pool->map_srcu);
	for (i = 0; (i < RMR_POOL_MAX_SESS && written < PAGE_SIZE); i++) {
		map = rcu_dereference(pool->maps[i]);
		if (!map)
			continue;

		written += sysfs_emit_at(page, written, "member_id : %d\n", map->member_id);
		for (j = 0; j < map->no_of_chunks; j++) {
			size_t len;

			id.b = j;
			if (rmr_map_check_dirty(map, id) &&
			    (map->bitmap_filter[id.b] & MAP_ENTRY_UNSYNCED)) {
				len = sysfs_emit_at(page, written, "(%llu, %llu) ",
						    id.a, id.b);
				if (!len) // break early if map is too big
					break;
				written += len;
			}
		}
		written += sysfs_emit_at(page, written, "\n");
	}
	srcu_read_unlock(&pool->map_srcu, lock_idx);

	return written;
}

static ssize_t rmr_srv_pool_map_unsynced_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	struct rmr_pool *pool;
	rmr_id_t id = { 0, 0 };
	int srv_id;
	struct rmr_dirty_id_map *map;

	pool = container_of(kobj, struct rmr_pool, kobj);
	if (sscanf(buf, "%llu %llu %d\n", &id.a, &id.b, &srv_id) != 3) {
		pr_err("cannot parse id.a %s\n", buf);
		return -EINVAL;
	}
	pr_debug("add id (%llu, %llu), srv_id %d\n", id.a, id.b, srv_id);

	map = rmr_pool_find_map(pool, srv_id);
	if (!map) {
		pr_err("in pool %s cannot find map for srv_id %u\n",
		       pool->poolname, srv_id);
		return -EINVAL;
	}

	rmr_map_set_dirty(map, id, MAP_ENTRY_UNSYNCED);
	rmr_srv_mark_maps_dirty((struct rmr_srv_pool *)pool->priv);
	pr_debug("insert id (%llu, %llu) srv_id %d\n", id.a, id.b, srv_id);

	return count;
}
static struct kobj_attribute rmr_srv_pool_map_unsynced_attr =
	__ATTR(map_unsynced, 0644, rmr_srv_pool_map_unsynced_show,
	       rmr_srv_pool_map_unsynced_store);

static ssize_t map_summary_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *page)
{
	struct rmr_pool *pool;
	int lock_idx;
	int written;

	pool = container_of(kobj, struct rmr_pool, kobj);

	lock_idx = srcu_read_lock(&pool->map_srcu);
	written = rmr_map_summary_format(pool, page, PAGE_SIZE);
	srcu_read_unlock(&pool->map_srcu, lock_idx);

	return written;
}

static struct kobj_attribute rmr_srv_pool_map_summary_attr =
	__ATTR_RO(map_summary);

static struct attribute *rmr_srv_pool_attrs[] = {
	&rmr_srv_leave_pool_attr.attr,
	&rmr_srv_member_id_attr.attr,
	&rmr_srv_pool_blksize_attr.attr,
	&rmr_srv_pool_map_attr.attr,
	&rmr_srv_pool_map_ver_attr.attr,
	&rmr_srv_pool_last_io_attr.attr,
	&rmr_srv_add_clt_pool_attr.attr,
	&rmr_srv_pool_sync_attr.attr,
	&rmr_srv_pool_sync_state_attr.attr,
	&rmr_srv_pool_state_attr.attr,
	&rmr_srv_remove_clt_pool_attr.attr,
	&rmr_srv_pool_test_map_attr.attr,
	&rmr_srv_pool_metadata_attr.attr,
	&rmr_srv_pool_map_update_state_attr.attr,
	&rmr_srv_pool_map_unsynced_attr.attr,
	&rmr_srv_pool_map_summary_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rmr_srv_pool);

static struct kobj_type rmr_srv_pool_ktype = {
	.sysfs_ops	= &kobj_sysfs_ops,
	.default_groups  = rmr_srv_pool_groups,
};

static struct kobj_type ktype = {
	.sysfs_ops	= &kobj_sysfs_ops,
};

static int rmr_srv_create_pool_sysfs_files(struct rmr_pool *pool)
{
	int ret;

	ret = kobject_init_and_add(&pool->kobj, &rmr_srv_pool_ktype,
				   &rmr_pool_dev->kobj, "%s", pool->poolname);
	if (ret) {
		pr_err("Failed to create sysfs dir for pool '%s': %d\n",
		       pool->poolname, ret);
		return ret;
	}

	ret = kobject_init_and_add(&pool->sessions_kobj, &ktype, &pool->kobj,
				   "sessions");
	if (unlikely(ret)) {
		pr_err("Failed to create sessions dir for pool '%s': %d\n",
		       pool->poolname, ret);
		kobject_del(&pool->kobj);
		kobject_put(&pool->kobj);
	}

	return ret;
}

/* remove new line from string */
static void strip(char *s)
{
	char *p = s;

	while (*s != '\0') {
		if (*s != '\n')
			*p++ = *s++;
		else
			++s;
	}
	*p = '\0';
}

enum rmr_srv_opts {
	RMR_SRV_OPT_POOL_NAME,
	RMR_SRV_OPT_MEMBER_ID,
	RMR_JOIN_OPT_Mandatory_count,
	RMR_SRV_OPT_ERR,
};

static const char * const rmr_srv_opts_mandatory_names[] = {
	[RMR_SRV_OPT_POOL_NAME] = "poolname",
	[RMR_SRV_OPT_MEMBER_ID] = "member_id",
};

static const match_table_t rmr_srv_opt_tokens = {
	{ RMR_SRV_OPT_POOL_NAME, "poolname=%s" },
	{ RMR_SRV_OPT_MEMBER_ID, "member_id=%s" },
	{ RMR_SRV_OPT_ERR, NULL },
};

static int rmr_srv_parse_options(const char *buf, char *poolname,
				 u32 *member_id)
{
	char *options, *p;
	substring_t args[MAX_OPT_ARGS];
	int opt_mask = 0;
	int token, ret = 0, i;

	options = kstrdup(buf, GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	options = strstrip(options);
	strip(options);
	while ((p = strsep(&options, " ")) != NULL) {
		if (!*p)
			continue;
		token = match_token(p, rmr_srv_opt_tokens, args);
		opt_mask |= (1 << token);

		switch (token) {
		case RMR_SRV_OPT_POOL_NAME:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(p) > NAME_MAX) {
				pr_err("join_pool: name too long\n");
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			strscpy(poolname, p, NAME_MAX);
			kfree(p);
			break;

		case RMR_SRV_OPT_MEMBER_ID:
			p = match_strdup(args);

			ret = kstrtou32(p, 0, member_id);
			if (ret) {
				pr_err("member_id isn't an integer: %d\n", ret);
				kfree(p);
				goto out;
			}

			kfree(p);
			break;

		default:
			pr_err("join_pool: Unknown parameter or missing value"
			       " '%s'\n", p);
			ret = -EINVAL;
			goto out;
		}
	};

	for (i = 0; i < RMR_JOIN_OPT_Mandatory_count; i++) {
		if ((opt_mask & (1 << rmr_srv_opt_tokens[i].token))) {
			ret = 0;
		} else {
			pr_err("join_pool: Mandatory parameter missing: %s\n",
			       rmr_srv_opts_mandatory_names[i]);
			ret = -EINVAL;
			break;
		}
	}

out:
	kfree(options);
	return ret;
}


static ssize_t rmr_srv_join_pool_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_srv_pool *srv_pool;
	char poolname[NAME_MAX];
	u32 member_id = UINT_MAX;
	int err;

	err = rmr_srv_parse_options(buf, poolname, &member_id);
	if (unlikely(err))
		return err;

	if (member_id > MAX_POOL_ID) {
		pr_err("%s: member_id gt max allowed pools (%u > %u)\n",
		       __func__, member_id, MAX_POOL_ID);
		return -EINVAL;
	}

	if (member_id == 0) {
		pr_err("%s: member_id is not allowed to be zero\n", __func__);
		return -EINVAL;
	}

	strip(poolname);

	pr_info("%s: Creating server pool with poolname %s, member_id %u\n",
		__func__, poolname, member_id);

	srv_pool = rmr_create_srv_pool(poolname, member_id);
	if (IS_ERR(srv_pool)) {
		pr_err("failed to create srv pool %s\n", poolname);
		return PTR_ERR(srv_pool);
	}

	pool = rmr_create_pool(poolname, srv_pool);
	if (IS_ERR(pool)) {
		err = PTR_ERR(pool);
		goto destroy_pool;
	}

	srv_pool->pool = pool;
	pool->is_clt = false;
	rmr_srv_pool_update_params(pool);

	err = rmr_srv_create_pool_sysfs_files(pool);
	if (err) {
		pr_err("%s: pool %s failed to create sysfs files\n", __func__, pool->poolname);
		goto destroy_pool;
	}

	return count;

destroy_pool:
	rmr_put_srv_pool(srv_pool);

	return err;
}

static ssize_t rmr_srv_join_pool_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *page)
{
	return scnprintf(page, PAGE_SIZE,
			 "Usage: echo \"poolname=<name_of_pool> member_id=<id_of_pool> > %s\n",
			 attr->attr.name);
}

static struct kobj_attribute rmr_srv_join_pool_attr =
	__ATTR(join_pool, 0644, rmr_srv_join_pool_show,
	       rmr_srv_join_pool_store);

static struct attribute *default_attrs[] = {
	&rmr_srv_join_pool_attr.attr,
	NULL,
};

static struct attribute_group default_attr_group = {
	.attrs = default_attrs,
};

int rmr_srv_create_sysfs_files(void)
{
	int err;
	dev_t devt = MKDEV(0, 0);

	rmr_dev_class = class_create("rmr-server");
	if (IS_ERR(rmr_dev_class))
		return PTR_ERR(rmr_dev_class);

	rmr_ctl_dev = device_create(rmr_dev_class, NULL, devt, NULL, "ctl");
	if (IS_ERR(rmr_ctl_dev)) {
		err = PTR_ERR(rmr_ctl_dev);
		goto cls_destroy;
	}

	rmr_pool_dev = device_create(rmr_dev_class, NULL, devt, NULL, "pools");
	if (IS_ERR(rmr_pool_dev)) {
		err = PTR_ERR(rmr_pool_dev);
		goto ctl_destroy;
	}

	err = sysfs_create_group(&rmr_ctl_dev->kobj, &default_attr_group);
	if (unlikely(err))
		goto pool_destroy;

	return 0;

pool_destroy:
	device_unregister(rmr_pool_dev);
ctl_destroy:
	device_unregister(rmr_ctl_dev);
cls_destroy:
	class_destroy(rmr_dev_class);

	return err;
}

void rmr_srv_destroy_sysfs_files(void)
{
	sysfs_remove_group(&rmr_ctl_dev->kobj, &default_attr_group);
	device_unregister(rmr_pool_dev);
	device_unregister(rmr_ctl_dev);
	class_destroy(rmr_dev_class);
}
