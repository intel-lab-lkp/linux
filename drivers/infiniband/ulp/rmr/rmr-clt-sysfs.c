// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include <linux/limits.h>       /* for NAME_MAX */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/delay.h>

#include "rmr-clt.h"

/*
 * Wait a bit before trying to reconnect after a failure
 * in order to give server time to finish clean up which
 * leads to "false positives" failed reconnect attempts
 */
#define RTRS_RECONNECT_BACKOFF 1000

#define RMR_DEFAULT_CHUNK_SIZE 131072 /* 128 KB */

static struct class *rmr_dev_class;
static struct device *rmr_ctl_dev;
static struct device *rmr_pool_dev;
static struct device *rmr_sess_dev;

enum {
	RMR_OPT_ERR		= 0,
	RMR_ADD_OPT_PATH	= 1 << 0,
	RMR_ADD_OPT_SESSNAME	= 1 << 1,
	RMR_ADD_OPT_MODE	= 1 << 2,
	RMR_DEL_OPT_MODE	= 1 << 3,
};

static unsigned int rmr_opt_add_mandatory[] = {
	RMR_ADD_OPT_PATH,
	RMR_ADD_OPT_SESSNAME,
	RMR_ADD_OPT_MODE,
};

/* For sync pools mode is not meaningful; only path and sessname are required. */
static unsigned int rmr_opt_add_sync_mandatory[] = {
	RMR_ADD_OPT_PATH,
	RMR_ADD_OPT_SESSNAME,
};

static const match_table_t rmr_opt_add_tokens = {
	{	RMR_ADD_OPT_PATH,	"path=%s"		},
	{	RMR_ADD_OPT_SESSNAME,	"sessname=%s"		},
	{	RMR_ADD_OPT_MODE,	"mode=%s"		},
	{	RMR_OPT_ERR,		NULL			},
};

enum rmr_opt_join {
	RMR_JOIN_OPT_POOLNAME,
	RMR_JOIN_OPT_Mandatory_count,
	RMR_JOIN_OPT_SYNC,
	RMR_JOIN_OPT_CHUNK_SIZE,
	RMR_JOIN_OPT_ERR,
};

static const char * const rmr_srv_opts_mandatory_names[] = {
	[RMR_JOIN_OPT_POOLNAME] = "poolname",
};

static const match_table_t rmr_opt_join_tokens = {
	{ RMR_JOIN_OPT_POOLNAME, "poolname=%s" },
	{ RMR_JOIN_OPT_SYNC, "sync=%s" },
	{ RMR_JOIN_OPT_CHUNK_SIZE, "chunk_size=%s" },
	{ RMR_JOIN_OPT_ERR, NULL },
};

static unsigned int rmr_opt_del_mandatory[] = {
	RMR_DEL_OPT_MODE,
};

static const match_table_t rmr_opt_del_tokens = {
	{	RMR_DEL_OPT_MODE,	"mode=%s"	},
	{	RMR_OPT_ERR,		NULL		},
};

enum {
	RMR_RECONNECT_OPT_ERR = 0,
	RMR_RECONNECT_OPT_PATH = 1 << 0,
};

static unsigned int rmr_opt_reconnect_mandatory[] = {
	RMR_RECONNECT_OPT_PATH,
};

static const match_table_t rmr_opt_reconnect_tokens = {
	{ RMR_RECONNECT_OPT_PATH, "path=%s" },
	{ RMR_RECONNECT_OPT_ERR, NULL },
};

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

static int rmr_clt_parse_add_sess_opts(const char *buf, char *sessname, int *create,
				       struct rtrs_addr *paths, size_t *path_cnt,
				       size_t max_path_cnt, const char *er_msg,
				       const match_table_t rmr_opt_tokens,
				       unsigned int *rmr_opt_mandatory,
				       size_t num_rmr_opt_mandatory)
{
	char *options, *options_orig, *sep_opt;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int opt_mask = 0;
	int token;
	int ret = -EINVAL;
	int i;
	int p_cnt = 0;

	options_orig = kstrdup(buf, GFP_KERNEL);
	if (!options_orig)
		return -ENOMEM;

	options = strstrip(options_orig);
	strip(options);
	sep_opt = options;
	while ((p = strsep(&sep_opt, " ")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, rmr_opt_tokens, args);
		opt_mask |= token;

		switch (token) {
		case RMR_ADD_OPT_SESSNAME:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(p) > NAME_MAX) {
				pr_err("%s: sessname too long\n", er_msg);
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			strscpy(sessname, p, NAME_MAX);
			kfree(p);
			break;

		case RMR_ADD_OPT_PATH:
			p = match_strdup(args);
			if (!p || p_cnt >= max_path_cnt) {
				ret = -ENOMEM;
				goto out;
			}

			ret = rtrs_addr_to_sockaddr(p, strlen(p), RTRS_PORT,
						     &paths[p_cnt]);
			if (ret) {
				pr_err("Can't parse path %s: %d\n", p, ret);
				kfree(p);
				goto out;
			}

			p_cnt++;

			kfree(p);
			break;

		case RMR_ADD_OPT_MODE:
			if (!create) {
				pr_err("%s: mode option not supported here\n", er_msg);
				ret = -EINVAL;
				goto out;
			}

			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}

			if (!strcmp(p, "create")) {
				*create = true;
			} else if (!strcmp(p, "assemble")) {
				*create = false;
			} else {
				pr_err("%s: Unknown mode '%s' (valid: create, assemble)\n", er_msg, p);
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			kfree(p);
			break;

		default:
			pr_err("%s: Unknown parameter or missing value"
			       " '%s'\n", er_msg, p);
			ret = -EINVAL;
			goto out;
		}
	}

	for (i = 0; i < num_rmr_opt_mandatory; i++) {
		if ((opt_mask & rmr_opt_mandatory[i])) {
			ret = 0;
		} else {
			pr_err("%s: Parameters missing\n", er_msg);
			ret = -EINVAL;
			break;
		}
	}

out:
	if (path_cnt)
		*path_cnt = p_cnt;
	kfree(options_orig);
	return ret;
}

static void rmr_clt_destroy_session_sysfs_files(struct rmr_clt_pool_sess *pool_sess,
						const struct attribute *sysfs_self)
{
	if (pool_sess->kobj.state_in_sysfs) {
		sysfs_remove_link(&pool_sess->kobj, "clt_sess");

		if (sysfs_self)
			sysfs_remove_file_self(&pool_sess->kobj, sysfs_self);
		kobject_del(&pool_sess->kobj);
		kobject_put(&pool_sess->kobj);
	}
}

static int rmr_clt_parse_del_sess_opts(const char *buf, bool *delete)
{
	char *options, *options_orig, *sep_opt, *p;
	substring_t args[MAX_OPT_ARGS];
	int i, token, opt_mask = 0, ret = -EINVAL;

	options_orig = kstrdup(buf, GFP_KERNEL);
	if (!options_orig)
		return -ENOMEM;

	options = strstrip(options_orig);
	strip(options);
	sep_opt = options;
	while ((p = strsep(&sep_opt, " ")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, rmr_opt_del_tokens, args);
		opt_mask |= token;

		switch (token) {
		case RMR_DEL_OPT_MODE:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}

			if (!strcmp(p, "delete")) {
				*delete = true;
			} else if (!strcmp(p, "disassemble")) {
				*delete = false;
			} else {
				pr_err("%s: Unknown mode '%s' (valid: delete, disassemble)\n", "del_sess", p);
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			kfree(p);
			break;

		default:
			pr_err("%s: Unknown parameter or missing value '%s'\n", "del_sess", p);
			ret = -EINVAL;
			goto out;
		}
	}

	for (i = 0; i < ARRAY_SIZE(rmr_opt_del_mandatory); i++) {
		if ((opt_mask & rmr_opt_del_mandatory[i])) {
			ret = 0;
		} else {
			pr_err("%s: Parameters missing\n", "del_sess");
			ret = -EINVAL;
			break;
		}
	}

out:
	kfree(options_orig);
	return ret;
}

static ssize_t rmr_clt_del_sess_show(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 > %s\n",
					 attr->attr.name);
}

static ssize_t rmr_clt_del_sess_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_clt_pool *clt_pool;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_sess *clt_sess;
	int err, i, idx;
	bool delete = false;
	u8 srv_sess_member_id;

	pool_sess = container_of(kobj, struct rmr_clt_pool_sess, kobj);
	clt_sess = pool_sess->clt_sess;
	srv_sess_member_id = pool_sess->member_id;
	pool = pool_sess->pool;
	clt_pool = (struct rmr_clt_pool *)pool->priv;

	err = rmr_clt_parse_del_sess_opts(buf, &delete);
	if (err)
		return err;

	if (pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_REMOVING)) {
		/*
		 * Freeze
		 */
		rmr_clt_pool_io_freeze(clt_pool);

		/*
		 * Wait for all completion
		 */
		rmr_clt_pool_io_wait_complete(clt_pool);

		/*
		 * Remove the storage node from the pool members list.
		 */
		xa_erase(&pool->stg_members, srv_sess_member_id);

		/*
		 * We simply wait for all inflights to get over to make sure
		 * that they are not affected with the delete session messages
		 * we are going to send after this.
		 * Once the inflights are done, we can restart the IOs immediately,
		 * since the session state has been changed to "removing".
		 *
		 * Unfreeze and wake up.
		 */
		rmr_clt_pool_io_unfreeze(clt_pool);

		send_msg_leave_pool(pool_sess, delete, WAIT);
	}

	pr_info("Closing session %s in pool %s\n",
		pool_sess->sessname, pool->poolname);

	if (!pool->sync) {
		if (delete) {
			/*
			 * Delete map for this session if it exists.
			 * For disassemble, keep the map so the piggyback loop
			 * continues to accumulate dirty entries for the member.
			 */
			rmr_pool_remove_map(pool, srv_sess_member_id);

			/*
			 * Clear the srv_md entry so the piggyback loop does
			 * not keep referencing a gone member.
			 * For disassemble, leave it intact — it is needed to
			 * identify the member during piggyback until reassembly.
			 */
			idx = rmr_pool_find_md(&pool->pool_md, srv_sess_member_id, false);

			if (idx >= 0)
				memset(&pool->pool_md.srv_md[idx], 0,
				       sizeof(struct rmr_srv_md));
			/*
			 * TODO: Push the srv_md change to persistence disk on remaining storages.
			 */
		} else {
			/*
			 * Disassemble: if this was the last non-sync session, no IOs
			 * will occur and the dirty maps serve no purpose. Delete them
			 * all; they will be recreated for all members on the first
			 * assemble via rmr_clt_process_non_sync_sess.
			 */
			if (xa_empty(&pool->stg_members)) {
				for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
					u8 mid = pool->pool_md.srv_md[i].member_id;

					if (!mid)
						continue;
					rmr_pool_remove_map(pool, mid);
				}
			}
		}

		/*
		 * Send messages to all other sessions,
		 * Informing them that a particular stor is getting deleted
		 */
		err = rmr_clt_del_stor_from_pool(pool_sess, delete);
		if (err) {
			pr_err("pool %s, del_stor failed for sess with member_id %u, err %d\n",
			       pool->poolname, srv_sess_member_id, err);
			return err;
		}
	}

	/*
	 * Remove the session from the list.
	 */
	mutex_lock(&pool->sess_lock);
	rmr_clt_del_pool_sess(pool_sess);
	mutex_unlock(&pool->sess_lock);

	rmr_clt_destroy_session_sysfs_files(pool_sess, &attr->attr);

	rmr_clt_free_pool_sess(pool_sess);
	rmr_clt_sess_put(clt_sess);

	if (list_empty(&pool->sess_list))
		rmr_clt_change_pool_state(clt_pool, RMR_CLT_POOL_STATE_JOINED, false);

	return count;
}

static struct kobj_attribute rmr_clt_del_pool_sess_attr =
	__ATTR(del_sess, 0644, rmr_clt_del_sess_show,
	       rmr_clt_del_sess_store);

static ssize_t rmr_clt_pool_sess_state_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *page)
{
	struct rmr_clt_pool_sess *pool_sess;
	ssize_t written = 0;

	pool_sess = container_of(kobj, struct rmr_clt_pool_sess, kobj);

	written += scnprintf(page, PAGE_SIZE, "%s\n",
			     rmr_clt_sess_state_str(atomic_read(&pool_sess->state)));

	written += scnprintf(page + written, PAGE_SIZE - written,
			     "Maintenance mode: %d\n", pool_sess->maintenance_mode);

	return written;
}

static struct kobj_attribute rmr_clt_pool_sess_state_attr =
	__ATTR(state, 0444, rmr_clt_pool_sess_state_show, NULL);

static ssize_t rmr_clt_sess_member_id_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *page)
{
	struct rmr_clt_pool_sess *pool_sess;

	pool_sess = container_of(kobj, struct rmr_clt_pool_sess, kobj);

	return scnprintf(page, PAGE_SIZE, "%u\n",
			 pool_sess->member_id);
}

static struct kobj_attribute rmr_clt_pool_sess_member_id_attr =
	__ATTR(member_id, 0644, rmr_clt_sess_member_id_show,
	       NULL);

static ssize_t rmr_clt_sess_enable_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *page)
{
	return scnprintf(page, PAGE_SIZE, "echo '1|0' > this_sysfs\n");
}

static ssize_t rmr_clt_sess_enable_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_pool *pool;
	int pool_sess_state, err;
	bool enable;

	pool_sess = container_of(kobj, struct rmr_clt_pool_sess, kobj);
	pool = pool_sess->pool;

	if (sysfs_streq(buf, "1"))
		enable = true;
	else if (sysfs_streq(buf, "0"))
		enable = false;
	else {
		pr_err("%s: unknown value: '%s'\n", attr->attr.name, buf);
		goto err;
	}

	pool_sess_state = atomic_read(&pool_sess->state);

	/*
	 * Manual disable is interpreted as switching to maintenance mode
	 * And it is only allowed for sessions NOT in "created" and "removing" state
	 * And non-sync sessions
	 */
	if (!enable && ((pool_sess_state == RMR_CLT_POOL_SESS_CREATED) ||
			(pool_sess_state == RMR_CLT_POOL_SESS_REMOVING) ||
			(pool_sess->pool->sync))) {
		pr_err("Cannot put pool_sess in maintenance mode: state %d, sync %d\n",
		       pool_sess_state, pool_sess->pool->sync);
		goto print_state_err;
	}

	if (enable)
		err = rmr_clt_enable_sess(pool_sess);
	else
		err = rmr_clt_set_pool_sess_mm(pool_sess);
	if (err) {
		pr_err("%s failed with err %d\n", __func__, err);
		goto err;
	}

	return count;

print_state_err:
	pr_err("Current state: %d\n", atomic_read(&pool_sess->state));
err:
	return -EINVAL;
}

static struct kobj_attribute rmr_clt_pool_sess_enable_attr =
	__ATTR(enable, 0644, rmr_clt_sess_enable_show,
	       rmr_clt_sess_enable_store);

static ssize_t rmr_clt_sess_check_map_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *page)
{
	return scnprintf(page, PAGE_SIZE, "echo '1' > this_sysfs\n");
}

static ssize_t rmr_clt_sess_check_map_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_msg_pool_cmd msg = {};
	int err;

	pool_sess = container_of(kobj, struct rmr_clt_pool_sess, kobj);

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s: unknown value: '%s'\n", attr->attr.name, buf);
		goto err;
	}

	rmr_clt_init_cmd(pool_sess->pool, &msg);
	msg.cmd_type = RMR_CMD_MAP_CHECK;

	err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
	if (err) {
		pr_err("%s failed with err %d\n", __func__, err);
		goto err;
	}
	return count;

err:
	return -EINVAL;
}

static struct kobj_attribute rmr_clt_pool_sess_check_map_attr =
	__ATTR(check_map, 0644, rmr_clt_sess_check_map_show,
	       rmr_clt_sess_check_map_store);

static struct attribute *rmr_clt_pool_sess_attrs[] = {
	&rmr_clt_del_pool_sess_attr.attr,
	&rmr_clt_pool_sess_state_attr.attr,
	&rmr_clt_pool_sess_member_id_attr.attr,
	&rmr_clt_pool_sess_enable_attr.attr,
	&rmr_clt_pool_sess_check_map_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rmr_clt_pool_sess);

static struct kobj_type rmr_clt_pool_sess_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = rmr_clt_pool_sess_groups,
};

static int rmr_clt_create_session_sysfs_files(struct rmr_clt_pool_sess *pool_sess)
{
	int ret;

	ret = kobject_init_and_add(&pool_sess->kobj, &rmr_clt_pool_sess_ktype,
				   &pool_sess->pool->sessions_kobj,
				   "%s", pool_sess->sessname);
	if (ret)
		pr_err("Failed to create sysfs dir for session '%s': %d\n",
		       pool_sess->sessname, ret);

	return ret;
}

static ssize_t rmr_clt_pool_add_sess_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo \""
					  "sessname=<name of the rtrs session>"
					  " path=<[srcaddr,]dstaddr>"
					  " [path=<[srcaddr,]dstaddr>]\" > %s\n\n"
					  "addr ::= [ ip:<ipv4> | ip:<ipv6> | gid:<gid> ]\n",
			 attr->attr.name);
}

static ssize_t rmr_clt_pool_add_sess_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	struct rtrs_addr paths[3];
	struct sockaddr_storage saddr[ARRAY_SIZE(paths)];
	struct sockaddr_storage daddr[ARRAY_SIZE(paths)];
	struct rmr_pool *pool;
	struct rmr_clt_pool *clt_pool;
	struct rmr_clt_sess *clt_sess;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_pool_md *clt_md;
	char *sessname;
	size_t path_cnt;
	int ret, index, create = 0;

	pool = container_of(kobj, struct rmr_pool, kobj);
	clt_pool = (struct rmr_clt_pool *)pool->priv;

	sessname = kzalloc(NAME_MAX, GFP_KERNEL);
	if (unlikely(!sessname))
		return -ENOMEM;

	for (path_cnt = 0; path_cnt < ARRAY_SIZE(paths); path_cnt++) {
		paths[path_cnt].src = &saddr[path_cnt];
		paths[path_cnt].dst = &daddr[path_cnt];
	}

	ret = rmr_clt_parse_add_sess_opts(buf, sessname,
					  pool->sync ? NULL : &create,
					  paths, &path_cnt, ARRAY_SIZE(paths),
					  "add_sess", rmr_opt_add_tokens,
					  pool->sync ? rmr_opt_add_sync_mandatory
						     : rmr_opt_add_mandatory,
					  pool->sync ? ARRAY_SIZE(rmr_opt_add_sync_mandatory)
						     : ARRAY_SIZE(rmr_opt_add_mandatory));
	if (ret)
		goto free_name;

	pr_info("%s: Creating rmr client session %s in pool %s\n", __func__, sessname,
		pool->poolname);

	clt_sess = find_and_get_or_create_clt_sess(sessname, paths, path_cnt);
	if (IS_ERR(clt_sess)) {
		pr_err("failed to find and get or create clt sess %s\n", sessname);
		ret = PTR_ERR(clt_sess);
		goto free_name;
	}

	pool_sess = rmr_clt_add_pool_sess(pool, clt_sess, create);
	if (IS_ERR(pool_sess)) {
		pr_err("failed to add pool sess %s to the pool %s\n",
		       sessname, pool->poolname);
		ret = PTR_ERR(pool_sess);
		goto put_clt_sess;
	}
	ret = rmr_clt_create_session_sysfs_files(pool_sess);
	if (ret) {
		pr_err("Creating sysfs files for %s in %s failed: %d\n",
		       pool_sess->sessname, pool->poolname, ret);
		goto destroy_sess;
	}

	ret = sysfs_create_link(&pool_sess->kobj, &clt_sess->kobj, "clt_sess");
	if (ret) {
		pr_err("Creating symlink for %s failed, err: %d\n",
		       pool_sess->sessname, ret);
		rmr_clt_destroy_session_sysfs_files(pool_sess, NULL);
		goto destroy_sess;
	}
	// ret = sysfs_create_link(&sess->kobj, sess->sess_kobj,
	// 			RTRS_LINK_NAME);
	// if (ret) {
	// 	pr_err("Creating rtrs symlink for %s in %s failed: %d\n",
	// 	       sess->sessname, pool->poolname, ret);
	// 	rmr_clt_destroy_session_sysfs_files(sess, NULL);
	// 	goto destroy_sess;
	// }
	rmr_clt_change_pool_state(clt_pool, RMR_CLT_POOL_STATE_JOINED, true);

	clt_md = &pool->pool_md;
	index = rmr_pool_find_md(clt_md, pool_sess->member_id, true);
	if (index < 0) {
		pr_err("No space for member %u in the clt_md\n", pool_sess->member_id);
		goto destroy_sess;
	}
	clt_md->srv_md[index].member_id = pool_sess->member_id;
	clt_md->srv_md[index].mapped_size = pool->mapped_size;

	kfree(sessname);
	return count;

destroy_sess:
	rmr_clt_destroy_pool_sess(pool_sess, create);
put_clt_sess:
	rmr_clt_sess_put(clt_sess);
free_name:
	kfree(sessname);
	return ret;
}

static struct kobj_attribute rmr_clt_pool_add_sess_attr =
	__ATTR(add_sess, 0644, rmr_clt_pool_add_sess_show,
	       rmr_clt_pool_add_sess_store);

static ssize_t rmr_clt_pool_leave_pool_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 > %s\n",
			 attr->attr.name);
}

static ssize_t rmr_clt_pool_leave_pool_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_clt_pool *clt_pool;
	int ret;

	pool = container_of(kobj, struct rmr_pool, kobj);
	clt_pool = (struct rmr_clt_pool *)pool->priv;

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s, %s unknown value: '%s'\n",
		       pool->poolname, attr->attr.name, buf);
		return -EINVAL;
	}

	if (refcount_read(&clt_pool->refcount) > 1) {
		pr_err("%s: Pool %s is in use.\n", __func__, pool->poolname);
		return -EINVAL;
	}

	pr_info("clt: Deleting pool '%s'\n", pool->poolname);

	ret = rmr_clt_remove_pool_from_sysfs(pool, &attr->attr);
	if (unlikely(ret))
		return ret;

	return count;
}

static struct kobj_attribute rmr_clt_pool_leave_pool_attr =
	__ATTR(leave_pool, 0644, rmr_clt_pool_leave_pool_show,
	       rmr_clt_pool_leave_pool_store);

static ssize_t rmr_clt_pool_chunk_size_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *page)
{
	struct rmr_pool *pool;
	struct rmr_clt_pool *clt_pool;

	pool = container_of(kobj, struct rmr_pool, kobj);
	clt_pool = (struct rmr_clt_pool *)pool->priv;

	if (pool->chunk_size == UINT_MAX)
		return scnprintf(page, PAGE_SIZE, "undefined\n");

	return scnprintf(page, PAGE_SIZE, "%u\n", pool->chunk_size);
}

static struct kobj_attribute rmr_clt_pool_chunk_size_attr =
	__ATTR(chunk_size, 0644, rmr_clt_pool_chunk_size_show, NULL);

static ssize_t rmr_clt_pool_map_show(struct kobject *kobj,
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

static ssize_t rmr_clt_pool_map_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	struct rmr_pool *pool;
	int err;
	rmr_id_t id = { 0, 0 };
	int srv_id;

	pool = container_of(kobj, struct rmr_pool, kobj);
	if (sscanf(buf, "%llu %llu %d\n", &id.a, &id.b, &srv_id) != 3) {
		pr_err("cannot parse id.a %s\n", buf);
		return -EINVAL;
	}
	pr_debug("add id (%llu, %llu), srv_id %d\n", id.a, id.b, srv_id);

	/*
	 * If given chunk number exceeds total chunks for us, ignore!
	 */
	if (id.b > pool->no_of_chunks)
		return count;

	err = rmr_clt_map_add_id(pool, srv_id, id);
	if (err == -ENOMEM) {
		pr_err("failed insert id (%llu, %llu) srv_id %d\n", id.a, id.b, srv_id);
	} else {
		pr_debug("insert id (%llu, %llu) srv_id %d\n", id.a, id.b, srv_id);
	}

	return count;
}

static struct kobj_attribute rmr_clt_pool_map_attr =
	__ATTR(map, 0644, rmr_clt_pool_map_show,
	       rmr_clt_pool_map_store);

static ssize_t rmr_clt_pool_map_ver_show(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *page)
{
	struct rmr_pool *pool;
	ssize_t written;

	pool = container_of(kobj, struct rmr_pool, kobj);

	written = scnprintf(page, PAGE_SIZE, "Map ver: %llu\n", pool->map_ver);

	return written;
}

static struct kobj_attribute rmr_clt_pool_map_ver_attr =
	__ATTR(map_version, 0444, rmr_clt_pool_map_ver_show, NULL);

static ssize_t rmr_clt_pool_enable_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 > %s\n",
			 attr->attr.name);
}

static ssize_t rmr_clt_pool_enable_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	struct rmr_pool *pool;
	int ret;

	pool = container_of(kobj, struct rmr_pool, kobj);

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s: unknown value: '%s'\n", attr->attr.name, buf);
		return -EINVAL;
	}

	ret = rmr_clt_pool_try_enable(pool);
	if (ret) {
		pr_err("%s: pool %s rmr_clt_pool_try_enable failed with err %d\n",
		       attr->attr.name, pool->poolname, ret);
		return ret;
	}

	return count;
}

static struct kobj_attribute rmr_clt_pool_enable_attr =
	__ATTR(pool_enable, 0644, rmr_clt_pool_enable_show,
	       rmr_clt_pool_enable_store);

static ssize_t rmr_clt_pool_test_map_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 > %s\n",
			 attr->attr.name);
}

static ssize_t rmr_clt_pool_test_map_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	struct rmr_pool *pool;
	int err;

	pool = container_of(kobj, struct rmr_pool, kobj);

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s, %s unknown value: '%s'\n",
		       pool->poolname, attr->attr.name, buf);
		return -EINVAL;
	}

	pr_info("pool %s start test map...\n", pool->poolname);
	err = rmr_clt_test_map(pool, pool);
	if (err) {
		pr_err("pool %s, test map failed, err %d\n",
		       pool->poolname, err);
		return err;
	}
	pr_info("pool %s test map done.", pool->poolname);

	return count;
}

static struct kobj_attribute rmr_clt_pool_test_map_attr =
	__ATTR(test_map, 0644, rmr_clt_pool_test_map_show,
	       rmr_clt_pool_test_map_store);

static struct attribute *rmr_clt_pool_attrs[] = {
	&rmr_clt_pool_add_sess_attr.attr,
	&rmr_clt_pool_leave_pool_attr.attr,
	&rmr_clt_pool_chunk_size_attr.attr,
	&rmr_clt_pool_map_attr.attr,
	&rmr_clt_pool_map_ver_attr.attr,
	&rmr_clt_pool_enable_attr.attr,
	&rmr_clt_pool_test_map_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rmr_clt_pool);

static struct kobj_type rmr_clt_pool_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = rmr_clt_pool_groups,
};

static struct kobj_type ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
};

static ssize_t rmr_clt_join_pool_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo \""
					  "poolname=<poolname> sync=y|Y|0|1 [chunk_size=<chunk_size in bytes>]\" "
					  "> %s\n",
			 attr->attr.name);
}

static int rmr_clt_create_stats_files(struct kobject *kobj,
				      struct kobject *stats_kobj);

static int rmr_clt_create_pool_sysfs_files(struct rmr_pool *pool)
{
	int ret;
	struct rmr_clt_pool *clt_pool;

	ret = kobject_init_and_add(&pool->kobj, &rmr_clt_pool_ktype,
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
		goto put_pool_kobj;
	}
	clt_pool = (struct rmr_clt_pool *)pool->priv;
	ret = rmr_clt_create_stats_files(&pool->kobj, &clt_pool->stats_kobj);
	if (unlikely(ret)) {
		pr_err("Failed to create sysfs stats files "
		       "for pool '%s': %d\n",
		       pool->poolname, ret);
		goto put_sessions_kobj;
	}

	return 0;

put_sessions_kobj:
	kobject_del(&pool->sessions_kobj);
	kobject_put(&pool->sessions_kobj);
put_pool_kobj:
	kobject_del(&pool->kobj);
	kobject_put(&pool->kobj);

	return ret;
}

void rmr_clt_destroy_pool_sysfs_files(struct rmr_pool *pool,
				      const struct attribute *sysfs_self)
{
	struct rmr_clt_pool *clt_pool;

	if (pool->kobj.state_in_sysfs) {
		clt_pool = (struct rmr_clt_pool *)pool->priv;
		kobject_del(&clt_pool->stats_kobj);
		kobject_put(&clt_pool->stats_kobj);

		kobject_del(&pool->sessions_kobj);
		kobject_put(&pool->sessions_kobj);
		if (sysfs_self)
			sysfs_remove_file_self(&pool->kobj, sysfs_self);
		kobject_del(&pool->kobj);
		kobject_put(&pool->kobj);
	}
}

static ssize_t rmr_clt_sess_reconnect_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *page)
{
	return scnprintf(page, PAGE_SIZE, "echo 'path=ip:<IP>' > this_sysfs\n");
}

static ssize_t rmr_clt_sess_reconnect_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	struct rmr_clt_sess *clt_sess;
	struct rmr_clt_pool_sess *pool_sess;
	struct rtrs_addr paths[3];
	struct sockaddr_storage saddr[ARRAY_SIZE(paths)];
	struct sockaddr_storage daddr[ARRAY_SIZE(paths)];
	size_t path_cnt;
	int err;


	clt_sess = container_of(kobj, struct rmr_clt_sess, kobj);

	pr_info("%s: Starting manual reconnect for clt_sess %s\n", __func__, clt_sess->sessname);

	/*
	 * The IP of the server has changed.
	 * Close the old rtrs connection, parse the path IP,
	 * and reconnect the session
	 */
	for (path_cnt = 0; path_cnt < ARRAY_SIZE(paths); path_cnt++) {
		paths[path_cnt].src = &saddr[path_cnt];
		paths[path_cnt].dst = &daddr[path_cnt];
	}

	err = rmr_clt_parse_add_sess_opts(buf, NULL, NULL, paths, &path_cnt, ARRAY_SIZE(paths),
					  "reconnect_sess", rmr_opt_reconnect_tokens,
					  rmr_opt_reconnect_mandatory,
					  ARRAY_SIZE(rmr_opt_reconnect_mandatory));
	if (err) {
		pr_err("%s: failed to parse options, err=%d\n", __func__, err);
		return err;
	}

	if (!IS_ERR_OR_NULL(clt_sess->rtrs)) {
		pr_info("close rtrs clt for session %s\n", clt_sess->sessname);

		clt_sess->state = RMR_CLT_SESS_DISCONNECTED;

		/*
		 * Wait for the state to be seen by rmr client
		 *
		 * The ones which are already in the rcu read section (see rmr_get_sess_iu)
		 * would complete its get_permit for rtrs.
		 * After that, rtrs_clt_close would wait for all the inflight permits to be
		 * returned.
		 */
		mutex_lock(&clt_sess->lock);
		list_for_each_entry(pool_sess, &clt_sess->pool_sess_list, clt_sess_entry)
			synchronize_srcu(&pool_sess->pool->sess_list_srcu);
		mutex_unlock(&clt_sess->lock);

		rtrs_clt_close(clt_sess->rtrs);
		clt_sess->rtrs = NULL;

		msleep(RTRS_RECONNECT_BACKOFF);
	}

	err = rmr_clt_reconnect_sess(clt_sess, paths, path_cnt);
	if (err) {
		pr_err("rmr_clt_reconnect_sess Failed\n");
		return err;
	}

	pr_info("%s: Manual reconnect for clt_sess %s succeeded\n", __func__, clt_sess->sessname);
	return count;
}

static struct kobj_attribute rmr_clt_sess_reconnect_attr =
	__ATTR(reconnect, 0644, rmr_clt_sess_reconnect_show,
	       rmr_clt_sess_reconnect_store);

static const char *rmr_clt_sess_state_names[] = {
	[0] = "invalid state",
	[RMR_CLT_SESS_DISCONNECTED] = "disconnected",
	[RMR_CLT_SESS_CONNECTED] = "connected"
};

static ssize_t rmr_clt_sess_state_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *page)
{
	struct rmr_clt_sess *clt_sess;

	clt_sess = container_of(kobj, struct rmr_clt_sess, kobj);

	return scnprintf(page, PAGE_SIZE, "%s\n",
			 rmr_clt_sess_state_names[clt_sess->state]);
}

static struct kobj_attribute rmr_clt_sess_state_attr =
	__ATTR(state, 0444, rmr_clt_sess_state_show, NULL);

static struct attribute *rmr_clt_sess_attrs[] = {
	&rmr_clt_sess_reconnect_attr.attr,
	&rmr_clt_sess_state_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rmr_clt_sess);

static struct kobj_type rmr_clt_sess_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = rmr_clt_sess_groups,
};

int rmr_clt_create_clt_sess_sysfs_files(struct rmr_clt_sess *clt_sess)
{
	int ret;

	ret = kobject_init_and_add(&clt_sess->kobj, &rmr_clt_sess_ktype,
				   &rmr_sess_dev->kobj, "%s", clt_sess->sessname);
	if (ret) {
		pr_err("Failed to create sysfs dir for sess '%s': %d\n",
		       clt_sess->sessname, ret);
		return ret;
	}

	return 0;
}

void rmr_clt_destroy_clt_sess_sysfs_files(struct rmr_clt_sess *clt_sess)
{
	if (clt_sess->kobj.state_in_sysfs) {
		kobject_del(&clt_sess->kobj);
		kobject_put(&clt_sess->kobj);
	}
}

static int rmr_clt_parse_join_opts(const char *buf, char *poolname,
				   bool *sync, u32 *chunk_size)
{
	char *options, *sep_opt;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int opt_mask = 0;
	int token;
	int ret = -EINVAL;
	int i;

	options = kstrdup(buf, GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	options = strstrip(options);
	strip(options);
	sep_opt = options;
	while ((p = strsep(&sep_opt, " ")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, rmr_opt_join_tokens, args);
		opt_mask |= (1 << token);

		switch (token) {
		case RMR_JOIN_OPT_POOLNAME:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(p) > NAME_MAX) {
				pr_err("join_pool: poolname too long\n");
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			strscpy(poolname, p, NAME_MAX);
			kfree(p);
			break;

		case RMR_JOIN_OPT_SYNC:
			p = match_strdup(args);

			ret = kstrtobool(p, sync);
			if (ret) {
				pr_err("sync isn't a boolean: %d\n", ret);
				kfree(p);
				goto out;
			}

			kfree(p);
			break;

		case RMR_JOIN_OPT_CHUNK_SIZE:
			/*
			 * Min supported chunk_size is PAGE_SIZE.
			 * The value must be power-of-2 and multiples
			 * of SECTOR_SIZE.
			 */
			p = match_strdup(args);

			ret = kstrtou32(p, 0, chunk_size);
			if (ret) {
				pr_err("chunk_size isn't an integer: %d\n", ret);
				kfree(p);
				goto out;
			} else if (*chunk_size < PAGE_SIZE) {
				pr_err("Min supported chunk_size is %lu\n", PAGE_SIZE);
				ret = -EINVAL;
				kfree(p);
				goto out;
			} else if (!is_power_of_2(*chunk_size)) {
				pr_err("chunk_size must be power of 2\n");
				ret = -EINVAL;
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
	}

	for (i = 0; i < RMR_JOIN_OPT_Mandatory_count; i++) {
		if ((opt_mask & (1 << rmr_opt_join_tokens[i].token))) {
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

static struct rmr_clt_pool *rmr_create_clt_pool(char *poolname, bool sync)
{
	struct rmr_clt_pool *clt_pool;
	int ret;

	clt_pool = kzalloc(sizeof(struct rmr_clt_pool), GFP_KERNEL);
	if (unlikely(!clt_pool))
		return ERR_PTR(-ENOMEM);

	refcount_set(&clt_pool->refcount, 1);

	init_waitqueue_head(&clt_pool->map_update_wq);
	atomic_set(&clt_pool->io_freeze, 0);
	mutex_init(&clt_pool->io_freeze_lock);
	mutex_init(&clt_pool->clt_pool_lock);

	clt_pool->recover_wq = alloc_workqueue("%s_recover_wq", 0, 0, poolname);
	if (!clt_pool->recover_wq) {
		ret = -ENOMEM;
		goto free_clt_pool;
	}

	if (!sync) {
		INIT_DELAYED_WORK(&clt_pool->recover_dwork, recover_work);
		queue_delayed_work(clt_pool->recover_wq, &clt_pool->recover_dwork,
				   msecs_to_jiffies(RMR_RECOVER_INTERVAL_MS));
	}

	return clt_pool;

free_clt_pool:
	kfree(clt_pool);
	return ERR_PTR(ret);
}

static ssize_t rmr_clt_join_pool_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct rmr_pool *pool;
	struct rmr_clt_pool *clt_pool;
	struct rmr_pool_md *clt_md;
	char *poolname;
	u32 chunk_size = RMR_DEFAULT_CHUNK_SIZE;
	bool sync = false;
	int err;

	poolname = kzalloc(NAME_MAX, GFP_KERNEL);
	if (unlikely(!poolname))
		return -ENOMEM;

	err = rmr_clt_parse_join_opts(buf, poolname, &sync, &chunk_size);
	if (unlikely(err))
		goto out;

	strip(poolname);

	pr_info("%s: Creating client pool with poolname %s, sync %d\n",
		__func__, poolname, sync);

	clt_pool = rmr_create_clt_pool(poolname, sync);
	if (IS_ERR(clt_pool)) {
		pr_err("%s: Clt pool creationg failed\n", __func__);
		err = PTR_ERR(clt_pool);
		goto out;
	}

	pool = rmr_create_pool(poolname, clt_pool);
	if (IS_ERR(pool)) {
		err = PTR_ERR(pool);
		goto put_clt_pool;
	}

	pool->is_clt = true;
	pool->sync = sync;
	clt_pool->pool = pool;

	pr_debug("pool %p, clt_pool %p\n", pool, pool->priv);

	err = rmr_clt_create_pool_sysfs_files(pool);
	if (err)
		goto put_clt_pool;

	if (!sync) {
		clt_md = &clt_pool->pool->pool_md;
		strscpy(clt_md->poolname, poolname, NAME_MAX);
		clt_md->group_id = pool->group_id;
		clt_md->map_ver = 1;
	}

	kfree(poolname);

	return count;

put_clt_pool:
	if (!sync)
		cancel_delayed_work_sync(&clt_pool->recover_dwork);

	rmr_put_clt_pool(clt_pool);
out:
	kfree(poolname);
	return err;
}

static struct kobj_attribute rmr_clt_join_pool_attr =
	__ATTR(join_pool, 0644,
	       rmr_clt_join_pool_show, rmr_clt_join_pool_store);

static struct attribute *default_attrs[] = {
	&rmr_clt_join_pool_attr.attr,
	NULL,
};

static struct attribute_group default_attr_group = {
	.attrs = default_attrs,
};

void rmr_clt_destroy_sysfs_files(void)
{
	sysfs_remove_group(&rmr_ctl_dev->kobj, &default_attr_group);

	device_unregister(rmr_sess_dev);
	device_unregister(rmr_pool_dev);
	device_unregister(rmr_ctl_dev);

	class_destroy(rmr_dev_class);
}

int rmr_clt_create_sysfs_files(void)
{
	int err;
	dev_t devt = MKDEV(0, 0);

	rmr_dev_class = class_create("rmr-client");
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

	rmr_sess_dev = device_create(rmr_dev_class, NULL, devt, NULL, "sessions");
	if (IS_ERR(rmr_sess_dev)) {
		err = PTR_ERR(rmr_sess_dev);
		goto pool_destroy;
	}

	err = sysfs_create_group(&rmr_ctl_dev->kobj, &default_attr_group);
	if (unlikely(err))
		goto sess_destroy;

	return 0;

sess_destroy:
	device_unregister(rmr_sess_dev);
pool_destroy:
	device_unregister(rmr_pool_dev);
ctl_destroy:
	device_unregister(rmr_ctl_dev);
cls_destroy:
	class_destroy(rmr_dev_class);

	return err;
}

STAT_ATTR(struct rmr_clt_stats, read_retries,
	  rmr_clt_stats_read_retries_to_str, rmr_clt_reset_read_retries);

static struct attribute *rmr_clt_stats_attrs[] = {
	&read_retries_attr.attr,
	NULL,
};

static struct attribute_group rmr_clt_stats_attr_group = {
	.attrs = rmr_clt_stats_attrs,
};

static int rmr_clt_create_stats_files(struct kobject *kobj,
				      struct kobject *stats_kobj)
{
	int ret;

	ret = kobject_init_and_add(stats_kobj, &ktype, kobj, "stats");
	if (ret) {
		pr_err("Failed to init and add stats kobject, err: %d\n",
		       ret);
		return ret;
	}

	ret = sysfs_create_group(stats_kobj, &rmr_clt_stats_attr_group);
	if (ret) {
		pr_err("failed to create stats sysfs group, err: %d\n",
		       ret);
		goto put_stats_obj;
	}

	return 0;

put_stats_obj:
	kobject_del(stats_kobj);
	kobject_put(stats_kobj);

	return ret;
}
