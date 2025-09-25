// SPDX-License-Identifier: GPL-2.0
/*
 * SCMI - System Telemetry Driver
 *
 * Copyright (C) 2025 ARM Ltd.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/dcache.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/scmi_protocol.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define TLM_FS_MAGIC		0x75C01C80
#define TLM_FS_NAME		"stlmfs"
#define TLM_FS_MNT		"arm_telemetry"

#define MAX_AVAILABLE_INTERV_CHAR_LENGTH	25
#define MAX_BULK_LINE_CHAR_LENGTH		64
#define MAX_PROP_PER_DE				12

static DEFINE_MUTEX(scmi_tlm_mtx);
static struct super_block *scmi_tlm_sb;

static atomic_t scmi_tlm_instance_count = ATOMIC_INIT(0);

struct scmi_tlm_setup;
struct scmi_tlm_priv {
	char *buf;
	size_t buf_sz;
	int buf_len;
	int (*bulk_retrieve)(const struct scmi_tlm_setup *tsp,
			     int res_id, int *num_samples,
			     struct scmi_telemetry_de_sample *samples);
};

/**
 * struct scmi_tlm_buffer  - Output Telemetry buffer descriptor
 * @used: Current number of used bytes in @buf
 * @buf: Actual buffer for output data
 *
 * This describes an output buffer which will be made available to each r/w
 * entry file_operations.
 */
struct scmi_tlm_buffer {
	size_t used;
#define SCMI_TLM_MAX_BUF_SZ	128
	unsigned char buf[SCMI_TLM_MAX_BUF_SZ];
};

/**
 * struct scmi_tlm_setup  - Telemetry setup descriptor
 * @dev: A reference to the related device
 * @ops: A reference to the protocol ops
 * @ph: A reference to the protocol handle to be used with the ops
 */
struct scmi_tlm_setup {
	struct device *dev;
	struct scmi_protocol_handle *ph;
	const struct scmi_telemetry_proto_ops *ops;
};

/**
 * struct scmi_tlm_class  - Telemetry class descriptor
 * @name: A string to be used for filesystem dentry name.
 * @mode: Filesystem mode mask.
 * @flags: Optional misc flags that can slighly modify provided @f_op behaviour;
 *	   this way the same @scmi_tlm_class can be used to describe multiple
 *	   entries in the filesystem whose @f_op behaviour is very similar.
 * @f_op: Optional file ops attached to this object. Used to initialized inodes.
 * @i_op: Optional inode ops attached to this object. Used to initialize inodes.
 *
 * This structure describes a class of telemetry entities that will be
 * associated with filesystem inodes having the same behaviour, i.e. the same
 * @f_op and @i_op: this way it will be possible to statically define a set of
 * common descriptors to describe all the possible behaviours and then link it
 * to the effective inodes that will be created to support the set of DEs
 * effectively discovered at run-time via SCMI.
 */
struct scmi_tlm_class {
	const char *name;
	umode_t mode;
	const int flags;
#define	TLM_IS_STATE	BIT(0)
#define	TLM_IS_GROUP	BIT(1)
#define IS_STATE(_f)	((_f) & TLM_IS_STATE)
#define IS_GROUP(_f)	((_f) & TLM_IS_GROUP)
	const struct file_operations *f_op;
	const struct inode_operations *i_op;
};

#define TLM_ANON_CLASS(_n, _f, _m, _fo, _io)	\
	{					\
		.name = _n,			\
		.flags = _f,			\
		.f_op = _fo,			\
		.i_op = _io,			\
		.mode = _m,			\
	}

#define DEFINE_TLM_CLASS(_tag, _ns, _fl, _mo, _fop, _iop)	\
	static const struct scmi_tlm_class _tag =		\
		TLM_ANON_CLASS(_ns, _fl, _mo, _fop, _iop)

/**
 * struct scmi_tlm_inode  - Telemetry node descriptor
 * @tsp: A reference to a structure holding data needed to interact with
 *	 the SCMI instance associated to this inode.
 * @cls: A reference to the @scmi_tlm_class describing the behaviour of this
 *	 inode.
 * @priv: Generic private data reference.
 * @de: SCMI DE data reference.
 * @grp: SCMI Group data reference.
 * @info: SCMI instance information data reference.
 * @parent: A reference to the parent inode if any.
 * @dentry: A reference to the dentry associated to this inode.
 * @vfs_inode: The embedded VFS inode that will be initialized and plugged
 *	       into the live filesystem at mount time.
 *
 * This structure is used to describe each SCMI Telemetry entity discovered
 * at probe time, store its related SCMI data, and link to the proper
 * telemetry calss @scmi_tlm_class: all of these created descriptors are stored
 * then in a root-to-leaves order at probe time, so that at mount time they can
 * be used to build the needed filesystem entries in the proper order making use
 * of the embeddded @vfs_inode.
 */
struct scmi_tlm_inode {
	const struct scmi_tlm_setup *tsp;
	const struct scmi_tlm_class *cls;
	union {
		const void *priv;
		const struct scmi_telemetry_de *de;
		const struct scmi_telemetry_group *grp;
		const struct scmi_telemetry_info *info;
	};
	struct scmi_tlm_inode *parent;
	struct dentry *dentry;
	struct inode vfs_inode;
};

#define to_tlm_inode(t)	container_of(t, struct scmi_tlm_inode, vfs_inode)

#define TLM_INODE_SETUP(_ti, _tsp, _cls, _parent, _ptr)		\
({								\
	typeof(_ti) _t = _ti;					\
	struct scmi_tlm_inode *_ino;				\
								\
	if (_t->num_nodes >= _t->max_nodes)			\
		return -ENOSPC;					\
								\
	_ino = scmi_tlm_inode_create(_tsp, _cls, _parent, _ptr);\
	if (!_ino)						\
		return -ENOMEM;					\
								\
	_t->all_nodes[_t->num_nodes++] = _ino;			\
								\
	_ino;							\
})

#define	MAX_INST_NAME		32

#define TOP_NODES_NUM		32
#define NODES_PER_DE_NUM	12
#define NODES_PER_GRP_NUM	 9

/**
 * struct scmi_tlm_instance  - Telemetry instance descriptor
 * @id: Progressive number identifying this probed instance; it will be used
 *	to name the top node at the root of this instance.
 * @name: Name to be used for the top root node of the instance. (tlm_<id>)
 * @node: A node to link this in the list of all instances.
 * @tsp: A reference to the SCMI instance data.
 * @top_cls: A class to represent the top node behaviour.
 * @top_inode: A reference to the inode at the top of this instance tree.
 * @max_nodes: Maximum number of entries that can be hold in @all_nodes.
 * @num_nodes: Number of nodes effectively initialized in @all_nodes
 * @all_nodes: An array to keep track of all the initialized TLM nodes that
 *	       have been created as a result of the usual probe time SCMI
 *	       enumeration process.
 * @info: A handy reference to this instance SCMI Telemetry info data.
 *
 * The most notable field in this structure is the @all_nodes array, which
 * keeps tracks of all of the nodes that has been initialized at probe time,
 * one for each SCMI Telemetry discovered entity disposed in a strict
 * parent-child order: this way at mount time this array can be scanned in its
 * natural order and each contained inode is initialized and plugged into the
 * SCMI Telemetry filesystem tree.
 */
struct scmi_tlm_instance {
	int id;
	char name[MAX_INST_NAME];
	struct list_head node;
	struct scmi_tlm_setup *tsp;
	struct scmi_tlm_class top_cls;
	struct scmi_tlm_inode *top_inode;
	int max_nodes;
	int num_nodes;
	struct scmi_tlm_inode **all_nodes;
	const struct scmi_telemetry_info *info;
};

static int scmi_telemetry_instance_register(struct super_block *sb,
					    struct scmi_tlm_instance *ti);

static LIST_HEAD(scmi_telemetry_instances);

static inline int
__scmi_tlm_generic_open(struct inode *ino, struct file *filp,
			int (*bulk_op)(const struct scmi_tlm_setup *tsp,
				       int res_id, int *num_samples,
				       struct scmi_telemetry_de_sample *samples))
{
	struct scmi_tlm_priv *tp;

	tp = kzalloc(sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return -ENOMEM;

	tp->bulk_retrieve = bulk_op;

	filp->private_data = tp;

	return nonseekable_open(ino, filp);
}

static int scmi_tlm_priv_release(struct inode *ino, struct file *filp)
{
	struct scmi_tlm_priv *tp = filp->private_data;

	kfree(tp->buf);
	kfree(tp);

	return 0;
}

static ssize_t scmi_tlm_all_des_write(struct file *filp,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	const struct scmi_telemetry_info *info = tlmi->priv;
	const struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_tlm_class *cls = tlmi->cls;
	bool enable;
	int ret;

	ret = kstrtobool_from_user(buf, count, &enable);
	if (ret)
		return ret;

	/* When !IS_STATE imply that is a tstamp_enable operation */
	if (IS_STATE(cls->flags) && !enable) {
		ret = tsp->ops->all_disable(tsp->ph, false);
		if (ret)
			return ret;
	} else {
		for (int i = 0; i < info->base.num_des; i++) {
			const struct scmi_telemetry_de *de = info->des[i];

			ret = tsp->ops->state_set(tsp->ph, false, de->info->id,
						  IS_STATE(cls->flags) ? &enable : NULL,
						  !IS_STATE(cls->flags) ? &enable : NULL);
			if (ret)
				return ret;
		}
	}

	return count;
}

static const struct file_operations all_des_fops = {
	.open = nonseekable_open,
	.write = scmi_tlm_all_des_write,
};

static ssize_t scmi_tlm_obj_enable_write(struct file *filp,
					 const char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	const struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_tlm_class *cls = tlmi->cls;
	bool enabled, is_group = IS_GROUP(cls->flags);
	int ret, res_id;

	ret = kstrtobool_from_user(buf, count, &enabled);
	if (ret)
		return ret;

	res_id = !is_group ? tlmi->de->info->id : tlmi->grp->info->id;
	ret = tsp->ops->state_set(tsp->ph, is_group, res_id,
				  IS_STATE(cls->flags) ? &enabled : NULL,
				  !IS_STATE(cls->flags) ? &enabled : NULL);
	if (ret)
		return ret;

	return count;
}

static ssize_t scmi_tlm_obj_enable_read(struct file *filp, char __user *buf,
					size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	const bool *enabled_state, *tstamp_enabled_state;
	char o_buf[2];
	bool enabled;

	if (!IS_GROUP(tlmi->cls->flags)) {
		enabled_state = &tlmi->de->enabled;
		tstamp_enabled_state = &tlmi->de->tstamp_enabled;
	} else {
		enabled_state = &tlmi->grp->enabled;
		tstamp_enabled_state = &tlmi->grp->tstamp_enabled;
	}

	enabled = IS_STATE(tlmi->cls->flags) ? *enabled_state : *tstamp_enabled_state;
	o_buf[0] = enabled ? 'Y' : 'N';
	o_buf[1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, o_buf, 2);
}

static const struct file_operations obj_enable_fops = {
	.open = nonseekable_open,
	.write = scmi_tlm_obj_enable_write,
	.read = scmi_tlm_obj_enable_read,
};

static int scmi_tlm_open(struct inode *ino, struct file *filp)
{
	struct scmi_tlm_buffer *data;

	/* Allocate some per-open buffer */
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	filp->private_data = data;

	return nonseekable_open(ino, filp);
}

static int scmi_tlm_release(struct inode *ino, struct file *filp)
{
	kfree(filp->private_data);

	return 0;
}

static ssize_t
scmi_tlm_update_interval_read(struct file *filp, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_buffer *data = filp->private_data;
	unsigned int active_update_interval;

	if (!data)
		return 0;

	if (!IS_GROUP(tlmi->cls->flags))
		active_update_interval = tlmi->info->active_update_interval;
	else
		active_update_interval = tlmi->grp->active_update_interval;

	if (!data->used)
		data->used =
			scnprintf(data->buf, SCMI_TLM_MAX_BUF_SZ, "%u, %d\n",
				  SCMI_TLM_GET_UPDATE_INTERVAL_SECS(active_update_interval),
				  SCMI_TLM_GET_UPDATE_INTERVAL_EXP(active_update_interval));

	return simple_read_from_buffer(buf, count, ppos, data->buf, data->used);
}

static ssize_t
scmi_tlm_update_interval_write(struct file *filp, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	const struct scmi_tlm_setup *tsp = tlmi->tsp;
	bool is_group = IS_GROUP(tlmi->cls->flags);
	unsigned int update_interval_ms = 0, secs = 0;
	int ret, grp_id, exp = -3;
	char *kbuf, *p, *token;

	kbuf = memdup_user_nul(buf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	p = kbuf;
	token = strsep(&p, " ");
	if (!token) {
		/* At least one token must exist to be a valid input */
		ret = -EINVAL;
		goto err;
	}

	ret = kstrtouint(token, 0, &secs);
	if (ret)
		goto err;

	token = strsep(&p, " ");
	if (token) {
		ret = kstrtoint(token, 0, &exp);
		if (ret)
			goto err;
	}

	kfree(kbuf);

	update_interval_ms = SCMI_TLM_BUILD_UPDATE_INTERVAL(secs, exp);

	grp_id = !is_group ? SCMI_TLM_GRP_INVALID : tlmi->grp->info->id;
	ret = tsp->ops->collection_configure(tsp->ph, grp_id, !is_group, NULL,
					     &update_interval_ms, NULL);
	if (ret)
		return ret;

	return count;

err:
	kfree(kbuf);
	return ret;
}

static const struct file_operations current_interval_fops = {
	.open = scmi_tlm_open,
	.read = scmi_tlm_update_interval_read,
	.write = scmi_tlm_update_interval_write,
	.release = scmi_tlm_release,
};

static ssize_t scmi_tlm_de_read(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	const struct scmi_tlm_setup *tsp = tlmi->tsp;
	struct scmi_tlm_buffer *data = filp->private_data;
	int ret;

	if (!data)
		return 0;

	if (!data->used) {
		struct scmi_telemetry_de_sample sample;

		sample.id = tlmi->de->info->id;
		ret = tsp->ops->de_data_read(tsp->ph, &sample);
		if (ret)
			return ret;

		data->used = scnprintf(data->buf, SCMI_TLM_MAX_BUF_SZ,
				       "%llu: %016llX\n", sample.tstamp,
				       sample.val);
	}

	return simple_read_from_buffer(buf, count, ppos, data->buf, data->used);
}

static const struct file_operations de_read_fops = {
	.open = scmi_tlm_open,
	.read = scmi_tlm_de_read,
	.release = scmi_tlm_release,
};

static ssize_t
scmi_tlm_enable_read(struct file *filp, char __user *buf, size_t count,
		     loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	char o_buf[2];

	o_buf[0] = tlmi->info->enabled ? 'Y' : 'N';
	o_buf[1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, o_buf, 2);
}

static ssize_t
scmi_tlm_enable_write(struct file *filp, const char __user *buf, size_t count,
		      loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	enum scmi_telemetry_collection mode = SCMI_TLM_ONDEMAND;
	const struct scmi_tlm_setup *tsp = tlmi->tsp;
	bool enabled;
	int ret;

	ret = kstrtobool_from_user(buf, count, &enabled);
	if (ret)
		return ret;

	ret = tsp->ops->collection_configure(tsp->ph, SCMI_TLM_GRP_INVALID, true,
					     &enabled, NULL, &mode);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations tlm_enable_fops = {
	.open = nonseekable_open,
	.read = scmi_tlm_enable_read,
	.write = scmi_tlm_enable_write,
};

static ssize_t
scmi_tlm_intrv_discrete_read(struct file *filp, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	bool discrete;
	char o_buf[2];

	discrete = !IS_GROUP(tlmi->cls->flags) ?
		tlmi->info->intervals->discrete : tlmi->grp->intervals->discrete;

	o_buf[0] = discrete ? 'Y' : 'N';
	o_buf[1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, o_buf, 2);
}

static const struct file_operations intrv_discrete_fops = {
	.open = nonseekable_open,
	.read = scmi_tlm_intrv_discrete_read,
};

static ssize_t
scmi_tlm_reset_write(struct file *filp, const char __user *buf, size_t count,
		     loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	int ret;

	ret = tlmi->tsp->ops->reset(tlmi->tsp->ph);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations reset_fops = {
	.open = nonseekable_open,
	.write = scmi_tlm_reset_write,
};

static int sa_u32_get(void *data, u64 *val)
{
	*val = *(u32 *)data;
	return 0;
}

static int sa_u32_set(void *data, u64 val)
{
	*(u32 *)data = val;
	return 0;
}

static int sa_u32_open(struct inode *ino, struct file *filp)
{
	return simple_attr_open(ino, filp, sa_u32_get, sa_u32_set, "%u\n");
}

static int sa_s32_open(struct inode *ino, struct file *filp)
{
	return simple_attr_open(ino, filp, sa_u32_get, sa_u32_set, "%d\n");
}

static int sa_x32_open(struct inode *ino, struct file *filp)
{
	return simple_attr_open(ino, filp, sa_u32_get, sa_u32_set, "0x%X\n");
}

static const struct file_operations sa_x32_ro_fops = {
	.open = sa_x32_open,
	.read = simple_attr_read,
	.release = simple_attr_release,
};

static const struct file_operations sa_u32_ro_fops = {
	.open = sa_u32_open,
	.read = simple_attr_read,
	.release = simple_attr_release,
};

static const struct file_operations sa_s32_ro_fops = {
	.open = sa_s32_open,
	.read = simple_attr_read,
	.release = simple_attr_release,
};

static ssize_t
scmi_de_impl_version_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_buffer *data = filp->private_data;

	if (!data)
		return 0;

	if (!data->used)
		data->used = scnprintf(data->buf, SCMI_TLM_MAX_BUF_SZ,
				       "%pUL\n", tlmi->info->base.de_impl_version);

	return simple_read_from_buffer(buf, count, ppos, data->buf, data->used);
}

static const struct file_operations de_impl_vers_fops = {
	.open = scmi_tlm_open,
	.read = scmi_de_impl_version_read,
	.release = scmi_tlm_release,
};

static ssize_t scmi_string_read(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct scmi_tlm_priv *tp = filp->private_data;

	if (!tp->buf) {
		struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
		const char *str = tlmi->priv;

		tp->buf = kasprintf(GFP_KERNEL, "%s\n", str);
		if (!tp->buf)
			return -ENOMEM;

		tp->buf_len = strlen(tp->buf) + 1;
	}

	return simple_read_from_buffer(buf, count, ppos, tp->buf, tp->buf_len);
}

static int scmi_tlm_priv_open(struct inode *ino, struct file *filp)
{
	return __scmi_tlm_generic_open(ino, filp, NULL);
}

static const struct file_operations string_ro_fops = {
	.open = scmi_tlm_priv_open,
	.read = scmi_string_read,
	.release = scmi_tlm_priv_release,
};

static ssize_t scmi_available_interv_read(struct file *filp, char __user *buf,
					  size_t count, loff_t *ppos)
{
	struct scmi_tlm_priv *tp = filp->private_data;

	if (!tp->buf) {
		struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
		struct scmi_tlm_intervals *intervals;
		int len = 0;

		intervals = !IS_GROUP(tlmi->cls->flags) ?
			tlmi->info->intervals : tlmi->grp->intervals;
		tp->buf_len = intervals->num * MAX_AVAILABLE_INTERV_CHAR_LENGTH;
		tp->buf = kzalloc(tp->buf_len, GFP_KERNEL);
		if (!tp->buf)
			return -ENOMEM;

		for (int i = 0; i < intervals->num; i++) {
			u32 ivl;

			ivl = intervals->update_intervals[i];
			len += scnprintf(tp->buf + len, tp->buf_len - len,
					 "%u,%d ",
					 SCMI_TLM_GET_UPDATE_INTERVAL_SECS(ivl),
					 SCMI_TLM_GET_UPDATE_INTERVAL_EXP(ivl));
		}
		tp->buf[len - 1] = '\n';
	}

	return simple_read_from_buffer(buf, count, ppos, tp->buf, tp->buf_len);
}

static const struct file_operations available_interv_fops = {
	.open = scmi_tlm_priv_open,
	.read = scmi_available_interv_read,
	.release = scmi_tlm_priv_release,
};

static const struct scmi_tlm_class tlm_tops[] = {
	TLM_ANON_CLASS("all_des_enable", TLM_IS_STATE, 0600, &all_des_fops, NULL),
	TLM_ANON_CLASS("all_des_tstamp_enable", 0, 0600, &all_des_fops, NULL),
	TLM_ANON_CLASS("current_update_interval_ms", 0, 0600, &current_interval_fops, NULL),
	TLM_ANON_CLASS("intervals_discrete", 0, 0400, &intrv_discrete_fops, NULL),
	TLM_ANON_CLASS("available_update_intervals_ms", 0, 0400,
		       &available_interv_fops, NULL),
	TLM_ANON_CLASS("de_implementation_version", 0, 0400, &de_impl_vers_fops, NULL),
	TLM_ANON_CLASS("tlm_enable", 0, 0600, &tlm_enable_fops, NULL),
	TLM_ANON_CLASS(NULL, 0, 0, NULL, NULL),
};

DEFINE_TLM_CLASS(reset_tlmo, "reset", 0, 0200, &reset_fops, NULL);

static const struct inode_operations tlm_dir_inode_ops = {
	.lookup = simple_lookup,
};

static const struct inode_operations tlm_file_inode_ops = { };

DEFINE_TLM_CLASS(des_dir_cls, "des", 0, 0700, NULL, &tlm_dir_inode_ops);
DEFINE_TLM_CLASS(name_tlmo, "name", 0, 0400, &string_ro_fops, NULL);
DEFINE_TLM_CLASS(ena_tlmo, "enable", TLM_IS_STATE, 0600, &obj_enable_fops, NULL);
DEFINE_TLM_CLASS(tstamp_ena_tlmo, "tstamp_enable", 0, 0600, &obj_enable_fops, NULL);
DEFINE_TLM_CLASS(type_tlmo, "type", 0, 0400, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(unit_tlmo, "unit", 0, 0400, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(unit_exp_tlmo, "unit_exp", 0, 0400, &sa_s32_ro_fops, NULL);
DEFINE_TLM_CLASS(instance_id_tlmo, "instance_id", 0, 0400, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(compo_type_tlmo, "compo_type", 0, 0400, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(compo_inst_id_tlmo, "compo_instance_id", 0, 0400, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(tstamp_exp_tlmo, "tstamp_exp", 0, 0400, &sa_s32_ro_fops, NULL);
DEFINE_TLM_CLASS(persistent_tlmo, "persistent", 0, 0400, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(value_tlmo, "value", 0, 0400, &de_read_fops, NULL);

static struct scmi_tlm_inode *
scmi_tlm_inode_create(const struct scmi_tlm_setup *tsp,
		      const struct scmi_tlm_class *cls,
		      struct scmi_tlm_inode *parent, const void *priv)
{
	struct scmi_tlm_inode *tlmi;

	tlmi = devm_kzalloc(tsp->dev, sizeof(*tlmi), GFP_KERNEL);
	if (!tlmi)
		return NULL;

	tlmi->cls = cls;
	tlmi->parent = parent;
	tlmi->tsp = tsp;
	tlmi->priv = priv;

	return tlmi;
}

static int scmi_telemetry_des_initialize(struct device *dev,
					 struct scmi_tlm_instance *ti)
{
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct scmi_tlm_inode *des_top_inode;

	des_top_inode = TLM_INODE_SETUP(ti, tsp, &des_dir_cls, NULL, NULL);

	for (int i = 0; i < ti->info->base.num_des; i++) {
		const struct scmi_telemetry_de *de = ti->info->des[i];
		struct scmi_tlm_de_info *dei = de->info;
		struct scmi_tlm_inode *de_dir_inode;
		struct scmi_tlm_class *de_tlm_cls;

		de_tlm_cls = devm_kzalloc(tsp->dev, sizeof(*de_tlm_cls), GFP_KERNEL);
		if (!de_tlm_cls)
			return -ENOMEM;

		de_tlm_cls->name = devm_kasprintf(dev, GFP_KERNEL, "0x%04X", dei->id);
		if (!de_tlm_cls->name)
			return -ENOMEM;

		de_tlm_cls->mode = 0700;
		de_tlm_cls->i_op = &tlm_dir_inode_ops;
		de_dir_inode = TLM_INODE_SETUP(ti, tsp, de_tlm_cls, des_top_inode, de);

		if (de->name_support)
			TLM_INODE_SETUP(ti, tsp, &name_tlmo, de_dir_inode, dei->name);

		TLM_INODE_SETUP(ti, tsp, &ena_tlmo, de_dir_inode, de);
		if (de->tstamp_support) {
			TLM_INODE_SETUP(ti, tsp, &tstamp_ena_tlmo, de_dir_inode, de);
			TLM_INODE_SETUP(ti, tsp, &tstamp_exp_tlmo, de_dir_inode,
					&dei->tstamp_exp);
		}

		TLM_INODE_SETUP(ti, tsp, &type_tlmo, de_dir_inode, &dei->type);
		TLM_INODE_SETUP(ti, tsp, &unit_tlmo, de_dir_inode, &dei->unit);
		TLM_INODE_SETUP(ti, tsp, &unit_exp_tlmo, de_dir_inode,
				&dei->unit_exp);
		TLM_INODE_SETUP(ti, tsp, &instance_id_tlmo, de_dir_inode,
				&dei->instance_id);
		TLM_INODE_SETUP(ti, tsp, &compo_type_tlmo, de_dir_inode,
				&dei->compo_type);
		TLM_INODE_SETUP(ti, tsp, &compo_inst_id_tlmo, de_dir_inode,
				&dei->compo_instance_id);
		TLM_INODE_SETUP(ti, tsp, &persistent_tlmo, de_dir_inode,
				&dei->persistent);

		TLM_INODE_SETUP(ti, tsp, &value_tlmo, de_dir_inode, de);
	}

	dev_info(dev, "Found %d Telemetry DE resources.\n", ti->info->base.num_des);

	return 0;
}

DEFINE_TLM_CLASS(version_tlmo, "version", 0, 0400, &sa_x32_ro_fops, NULL);

static int scmi_tlm_bulk_on_demand(const struct scmi_tlm_setup *tsp,
				   int res_id, int *num_samples,
				   struct scmi_telemetry_de_sample *samples)
{
	return tsp->ops->des_bulk_read(tsp->ph, res_id, num_samples, samples);
}

static int scmi_tlm_data_open(struct inode *ino, struct file *filp)
{
	return __scmi_tlm_generic_open(ino, filp, scmi_tlm_bulk_on_demand);
}

static int scmi_tlm_buffer_fill(struct device *dev, char *buf, size_t size,
				int *len, int num,
				struct scmi_telemetry_de_sample *samples)
{
	int idx, bytes = 0;

	/* Loop till there space for the next line */
	for (idx = 0; idx < num && size - bytes >= MAX_BULK_LINE_CHAR_LENGTH; idx++) {
		bytes += scnprintf(buf + bytes, size - bytes,
				   "0x%04X %llu %016llX\n", samples[idx].id,
				   samples[idx].tstamp, samples[idx].val);
	}

	if (idx < num) {
		dev_err(dev, "Bulk buffer truncated !\n");
		return -ENOSPC;
	}

	if (len)
		*len = bytes;

	return 0;
}

static int scmi_tlm_bulk_buffer_allocate_and_fill(struct scmi_tlm_inode *tlmi,
						  struct scmi_tlm_priv *tp)
{
	const struct scmi_tlm_setup *tsp = tlmi->tsp;
	const struct scmi_tlm_class *cls = tlmi->cls;
	struct scmi_telemetry_de_sample *samples;
	bool is_group = IS_GROUP(cls->flags);
	int ret, num_samples, res_id;

	num_samples = !is_group ? tlmi->info->base.num_des :
		tlmi->grp->info->num_des;
	tp->buf_sz = num_samples * MAX_BULK_LINE_CHAR_LENGTH;
	tp->buf = kzalloc(tp->buf_sz, GFP_KERNEL);
	if (!tp->buf)
		return -ENOMEM;

	res_id = is_group ? tlmi->grp->info->id : SCMI_TLM_GRP_INVALID;
	samples = kcalloc(num_samples, sizeof(*samples), GFP_KERNEL);
	if (!samples) {
		kfree(tp->buf);
		return -ENOMEM;
	}

	ret = tp->bulk_retrieve(tsp, res_id, &num_samples, samples);
	if (ret) {
		kfree(samples);
		kfree(tp->buf);
		return ret;
	}

	ret = scmi_tlm_buffer_fill(tsp->dev, tp->buf, tp->buf_sz, &tp->buf_len,
				   num_samples, samples);
	kfree(samples);

	return ret;
}

static ssize_t scmi_tlm_generic_data_read(struct file *filp, char __user *buf,
					  size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_priv *tp = filp->private_data;
	int ret;

	if (!tp->buf) {
		ret = scmi_tlm_bulk_buffer_allocate_and_fill(tlmi, tp);
		if (ret)
			return ret;
	}

	return simple_read_from_buffer(buf, count, ppos, tp->buf, tp->buf_len);
}

static const struct file_operations scmi_tlm_data_fops = {
	.owner = THIS_MODULE,
	.open = scmi_tlm_data_open,
	.read = scmi_tlm_generic_data_read,
	.release = scmi_tlm_priv_release,
};

DEFINE_TLM_CLASS(data_tlmo, "des_bulk_read", 0, 0400, &scmi_tlm_data_fops, NULL);

static int scmi_tlm_bulk_single_read(const struct scmi_tlm_setup *tsp,
				     int res_id, int *num_samples,
				     struct scmi_telemetry_de_sample *samples)
{
	return tsp->ops->des_sample_get(tsp->ph, res_id, num_samples, samples);
}

static int scmi_tlm_single_read_open(struct inode *ino, struct file *filp)
{
	return __scmi_tlm_generic_open(ino, filp, scmi_tlm_bulk_single_read);
}

/* TODO
 * Could be better with a .poll fops since single sample
 * reads trigger an asynchronous request.
 */
static const struct file_operations scmi_tlm_single_sample_fops = {
	.owner = THIS_MODULE,
	.open = scmi_tlm_single_read_open,
	.read = scmi_tlm_generic_data_read,
	.release = scmi_tlm_priv_release,
};

DEFINE_TLM_CLASS(single_sample_tlmo, "des_single_sample_read", 0, 0400,
		 &scmi_tlm_single_sample_fops, NULL);

static const struct scmi_tlm_class tlm_grps[] = {
	TLM_ANON_CLASS("enable", TLM_IS_STATE | TLM_IS_GROUP, 0600, &obj_enable_fops, NULL),
	TLM_ANON_CLASS("tstamp_enable", TLM_IS_GROUP, 0600, &obj_enable_fops, NULL),
	TLM_ANON_CLASS(NULL, 0, 0, NULL, NULL),
};

DEFINE_TLM_CLASS(grp_data_tlmo, "des_bulk_read", TLM_IS_GROUP, 0400,
		 &scmi_tlm_data_fops, NULL);

DEFINE_TLM_CLASS(groups_dir_cls, "groups", 0, 0700, NULL, &tlm_dir_inode_ops);

DEFINE_TLM_CLASS(grp_single_sample_tlmo, "des_single_sample_read", TLM_IS_GROUP,
		 0400, &scmi_tlm_single_sample_fops, NULL);

DEFINE_TLM_CLASS(grp_composing_des_tlmo, "composing_des", TLM_IS_GROUP, 0400,
		 &string_ro_fops, NULL);

DEFINE_TLM_CLASS(grp_current_interval_tlmo, "current_update_interval_ms",
		 TLM_IS_GROUP, 0600, &current_interval_fops, NULL);

DEFINE_TLM_CLASS(grp_available_interval_tlmo, "available_update_intervals_ms",
		 TLM_IS_GROUP, 0400, &available_interv_fops, NULL);

DEFINE_TLM_CLASS(grp_intervals_discrete_tlmo, "intervals_discrete",
		 TLM_IS_GROUP, 0400, &intrv_discrete_fops, NULL);

static int scmi_telemetry_groups_initialize(struct device *dev,
					    struct scmi_tlm_instance *ti)
{
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct scmi_tlm_inode *groups_top_inode;

	if (ti->info->base.num_groups == 0)
		return 0;

	groups_top_inode = TLM_INODE_SETUP(ti, tsp, &groups_dir_cls, NULL, NULL);

	for (int i = 0; i < ti->info->base.num_groups; i++) {
		const struct scmi_telemetry_group *grp = &ti->info->groups[i];
		struct scmi_tlm_class *grp_tlm_cls;
		struct scmi_tlm_inode *grp_dir_inode;

		grp_tlm_cls = devm_kzalloc(tsp->dev, sizeof(*grp_tlm_cls), GFP_KERNEL);
		if (!grp_tlm_cls)
			return -ENOMEM;

		grp_tlm_cls->name = devm_kasprintf(dev, GFP_KERNEL, "%u", grp->info->id);
		if (!grp_tlm_cls->name)
			return -ENOMEM;

		grp_tlm_cls->mode = 0700;
		grp_tlm_cls->i_op = &tlm_dir_inode_ops;

		grp_dir_inode = TLM_INODE_SETUP(ti, tsp, grp_tlm_cls,
						groups_top_inode, grp);

		for (const struct scmi_tlm_class *gto = tlm_grps; gto->name; gto++)
			TLM_INODE_SETUP(ti, tsp, gto, grp_dir_inode, grp);

		TLM_INODE_SETUP(ti, tsp, &grp_composing_des_tlmo, grp_dir_inode,
				grp->des_str);

		TLM_INODE_SETUP(ti, tsp, &grp_data_tlmo, grp_dir_inode, grp);
		TLM_INODE_SETUP(ti, tsp, &grp_single_sample_tlmo, grp_dir_inode, grp);

		if (ti->info->per_group_config_support) {
			TLM_INODE_SETUP(ti, tsp, &grp_current_interval_tlmo,
					grp_dir_inode, grp);
			TLM_INODE_SETUP(ti, tsp, &grp_available_interval_tlmo,
					grp_dir_inode, grp);
			TLM_INODE_SETUP(ti, tsp, &grp_intervals_discrete_tlmo,
					grp_dir_inode, grp);
		}
	}

	dev_info(dev, "Found %d Telemetry GROUPS resources.\n",
		 ti->info->base.num_groups);

	return 0;
}

static int scmi_tlm_root_instance_initialize(struct device *dev,
					     struct scmi_tlm_instance *ti)
{
	struct scmi_tlm_setup *tsp = ti->tsp;

	/*
	 * Allocate space for all possible nodes, i.e. in order:
	 *  - top level nodes
	 *  - all DE subdirs contained in des/
	 *  - all DE proeperties files inside each 0xNNNN/ DE subdir
	 *  - all GRPS subdirs contained in groups/
	 *  - all GRPS proeperties files inside each <N>/ GRP subdir
	 */
	ti->max_nodes = TOP_NODES_NUM + ti->info->base.num_des +
		NODES_PER_DE_NUM * ti->info->base.num_des +
		ti->info->base.num_groups +
		NODES_PER_GRP_NUM * ti->info->base.num_groups;
	ti->all_nodes = devm_kcalloc(tsp->dev, ti->max_nodes,
				     sizeof(*ti->all_nodes), GFP_KERNEL);
	if (!ti->all_nodes)
		return -ENOMEM;

	scnprintf(ti->name, MAX_INST_NAME, "tlm_%d", ti->id);

	/* Allocate top instance node */
	ti->top_cls.name = ti->name;
	ti->top_cls.mode = 0755;
	/*
	 * Do NOT register the top_node root in all_nodes[] since it is
	 * treated differently at mount time
	 */
	ti->top_inode = scmi_tlm_inode_create(tsp, &ti->top_cls, NULL, NULL);

	for (const struct scmi_tlm_class *tlmo = tlm_tops; tlmo->name; tlmo++)
		TLM_INODE_SETUP(ti, tsp, tlmo, NULL, ti->info);

	if (ti->info->reset_support)
		TLM_INODE_SETUP(ti, tsp, &reset_tlmo, NULL, NULL);

	TLM_INODE_SETUP(ti, tsp, &version_tlmo, NULL, &ti->info->base.version);
	TLM_INODE_SETUP(ti, tsp, &data_tlmo, NULL, ti->info);
	TLM_INODE_SETUP(ti, tsp, &single_sample_tlmo, NULL, ti->info);

	return 0;
}

static struct scmi_tlm_instance *scmi_tlm_init(struct scmi_tlm_setup *tsp,
					       int instance_id)
{
	struct device *dev = tsp->dev;
	struct scmi_tlm_instance *ti;
	int ret;

	ti = devm_kzalloc(dev, sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return ERR_PTR(-ENOMEM);

	ti->info = tsp->ops->info_get(tsp->ph);
	if (!ti->info) {
		dev_err(dev, "invalid Telemetry info !\n");
		return ERR_PTR(-EINVAL);
	}

	ti->id = instance_id;
	ti->tsp = tsp;

	ret = scmi_tlm_root_instance_initialize(dev, ti);
	if (ret)
		return ERR_PTR(ret);

	ret = scmi_telemetry_des_initialize(dev, ti);
	if (ret)
		return ERR_PTR(ret);

	ret = scmi_telemetry_groups_initialize(dev, ti);
	if (ret)
		return ERR_PTR(ret);

	return ti;
}

static int scmi_telemetry_probe(struct scmi_device *sdev)
{
	const struct scmi_handle *handle = sdev->handle;
	struct scmi_protocol_handle *ph;
	struct device *dev = &sdev->dev;
	struct scmi_tlm_instance *ti;
	struct scmi_tlm_setup *tsp;
	const void *ops;

	if (!handle)
		return -ENODEV;

	ops = handle->devm_protocol_get(sdev, sdev->protocol_id, &ph);
	if (IS_ERR(ops))
		return dev_err_probe(dev, PTR_ERR(ops),
				     "Cannot access protocol:0x%X\n",
				     sdev->protocol_id);

	tsp = devm_kzalloc(dev, sizeof(*tsp), GFP_KERNEL);
	if (!tsp)
		return -ENOMEM;

	tsp->dev = dev;
	tsp->ops = ops;
	tsp->ph = ph;

	ti = scmi_tlm_init(tsp, atomic_fetch_inc(&scmi_tlm_instance_count));
	if (IS_ERR(ti))
		return PTR_ERR(ti);

	mutex_lock(&scmi_tlm_mtx);
	list_add(&ti->node, &scmi_telemetry_instances);
	if (scmi_tlm_sb) {
		int ret;

		/*
		 * If the file system was already mounted by the time this
		 * instance was probed, register explicitly, since the list
		 * has been scanned already.
		 */
		mutex_unlock(&scmi_tlm_mtx);
		ret = scmi_telemetry_instance_register(scmi_tlm_sb, ti);
		if (ret)
			return ret;
		mutex_lock(&scmi_tlm_mtx);
	}
	mutex_unlock(&scmi_tlm_mtx);

	dev_set_drvdata(&sdev->dev, ti);

	return 0;
}

static void scmi_telemetry_remove(struct scmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct scmi_tlm_instance *ti;
	bool enabled = false;
	int ret;

	ti = dev_get_drvdata(&sdev->dev);

	/* Stop SCMI Telemetry collection */
	ret = ti->tsp->ops->collection_configure(ti->tsp->ph,
						 SCMI_TLM_GRP_INVALID, true,
						 &enabled, NULL, NULL);
	if (ret)
		dev_warn(dev, "Failed to stop Telemetry collection\n");

	list_del(&ti->node);
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_TELEMETRY, "telemetry" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_telemetry_driver = {
	.name = "scmi-telemetry-driver",
	.probe = scmi_telemetry_probe,
	.remove = scmi_telemetry_remove,
	.id_table = scmi_id_table,
};

static const struct super_operations tlm_sops = {
	.statfs = simple_statfs,
	.drop_inode = generic_delete_inode,
};

static struct inode *
scmi_tlm_inode_initialize(struct super_block *sb, umode_t mode,
			  struct scmi_tlm_inode *tlmi)
{
	struct inode *inode;

	if (!tlmi)
		return NULL;

	inode = &tlmi->vfs_inode;
	if (unlikely(inode_init_always(sb, inode)))
		return NULL;

	inode_sb_list_add(inode);
	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, NULL, mode);
	simple_inode_init_ts(inode);

	if (S_ISDIR(mode)) {
		inode->i_op = &tlm_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;
	} else if (S_ISREG(mode)) {
		inode->i_op = &tlm_file_inode_ops;
		inode->i_fop = tlmi ? tlmi->cls->f_op : NULL;
	}

	inode->i_private = (void *)tlmi->priv;

	return inode;
}

static struct dentry *
scmi_tlm_node_add(struct super_block *sb, struct dentry *parent,
		  const char *name, umode_t mode, struct scmi_tlm_inode *tlmi)
{
	struct inode *ino;
	struct dentry *dentry;

	ino = scmi_tlm_inode_initialize(sb, mode, tlmi);
	if (!ino)
		return ERR_PTR(-ENOMEM);

	dentry = d_alloc_name(parent, name);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	tlmi->dentry = dentry;
	d_add(dentry, ino);

	return dentry;
}

static int scmi_telemetry_instance_register(struct super_block *sb,
					    struct scmi_tlm_instance *ti)
{
	struct dentry *top;

	/* AT first create instance top dir ... */
	top = scmi_tlm_node_add(sb, sb->s_root, ti->top_cls.name,
				S_IFDIR | ti->top_cls.mode, ti->top_inode);
	if (IS_ERR(top))
		return PTR_ERR(top);

	/*
	 * Scan the array of tlm_inode pre-initialized with SCMI Telemetry
	 * discovered entities and map it into the filesystem .
	 */
	for (int i = 0; i < ti->num_nodes; i++) {
		struct scmi_tlm_inode *tlmi = ti->all_nodes[i];
		struct dentry *dentry, *parent;
		umode_t mode;

		/* Check sanity of node tree */
		if (WARN_ON_ONCE(!tlmi))
			continue;

		parent = !tlmi->parent ? top : tlmi->parent->dentry;
		mode = (tlmi->cls->f_op ? S_IFREG : S_IFDIR) | tlmi->cls->mode;
		dentry = scmi_tlm_node_add(sb, parent, tlmi->cls->name, mode, tlmi);
		if (IS_ERR(dentry))
			return PTR_ERR(dentry);
	}

	return 0;
}

static struct scmi_tlm_inode root_tlm;

static int tlm_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct dentry *root_dentry;
	struct scmi_tlm_instance *ti;

	sb->s_magic = TLM_FS_MAGIC;
	sb->s_op = &tlm_sops;

	/* create root inode (directory) */
	root_inode = scmi_tlm_inode_initialize(sb, S_IFDIR | 0755, &root_tlm);
	if (!root_inode)
		return -ENOMEM;

	root_dentry = d_make_root(root_inode);
	if (!root_dentry)
		return -ENOMEM;

	sb->s_root = root_dentry;
	list_for_each_entry(ti, &scmi_telemetry_instances, node) {
		int ret;

		ret = scmi_telemetry_instance_register(sb, ti);
		if (ret)
			return ret;
	}

	guard(mutex)(&scmi_tlm_mtx);
	if (!scmi_tlm_sb)
		scmi_tlm_sb = sb;

	return 0;
}

static struct dentry *tlm_mount(struct file_system_type *fs_type, int flags,
				const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, tlm_fill_super);
}

static void tlm_kill_sb(struct super_block *sb)
{
	kill_litter_super(sb);
}

//XXX Move to new fs_context-based MOUNT process !!! (see debugfs_parse_param)
static struct file_system_type scmi_telemetry_fs = {
	.owner = THIS_MODULE,
	.name = TLM_FS_NAME,
	.mount = tlm_mount,
	.kill_sb = tlm_kill_sb,
	.fs_flags = 0,
};

static int __init scmi_telemetry_init(void)
{
	int ret;

	ret = scmi_register(&scmi_telemetry_driver);
	if (ret)
		return ret;

	ret = sysfs_create_mount_point(fs_kobj, TLM_FS_MNT);
	if (ret && ret != -EEXIST) {
		scmi_unregister(&scmi_telemetry_driver);
		return ret;
	}

	ret = register_filesystem(&scmi_telemetry_fs);
	if (ret) {
		sysfs_remove_mount_point(fs_kobj, TLM_FS_MNT);
		scmi_unregister(&scmi_telemetry_driver);
	}

	return ret;
}
module_init(scmi_telemetry_init);

static void __exit scmi_telemetry_exit(void)
{
	int ret;

	ret = unregister_filesystem(&scmi_telemetry_fs);
	if (!ret)
		sysfs_remove_mount_point(fs_kobj, TLM_FS_MNT);
	else
		pr_err("Failed to unregister %s\n", TLM_FS_NAME);

	scmi_unregister(&scmi_telemetry_driver);
}
module_exit(scmi_telemetry_exit);

MODULE_AUTHOR("Cristian Marussi <cristian.marussi@arm.com>");
MODULE_DESCRIPTION("ARM SCMI Telemetry Driver");
MODULE_LICENSE("GPL");
