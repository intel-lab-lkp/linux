// SPDX-License-Identifier: GPL-2.0
/*
 * SCMI - System Telemetry Driver
 *
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/ctype.h>
#include <linux/dcache.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
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

static struct kmem_cache *stlmfs_inode_cachep;

static DEFINE_MUTEX(stlmfs_mtx);
static struct super_block *stlmfs_sb;

static atomic_t scmi_tlm_instance_count = ATOMIC_INIT(0);

struct scmi_tlm_setup;

struct scmi_tlm_priv {
	char *buf;
	size_t buf_sz;
	int buf_len;
	int (*bulk_retrieve)(struct scmi_tlm_setup *tsp,
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
 * @ph: A reference to the protocol handle to be used with the ops
 * @rinfo: A reference to the resource info descriptor
 * @ops: A reference to the protocol ops
 */
struct scmi_tlm_setup {
	struct device *dev;
	struct scmi_protocol_handle *ph;
	const struct scmi_telemetry_res_info __private *rinfo;
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
	int flags;
#define	TLM_IS_STATE	BIT(0)
#define	TLM_IS_GROUP	BIT(1)
#define	TLM_IS_DYNAMIC	BIT(2)
#define IS_STATE(_f)	((_f) & TLM_IS_STATE)
#define IS_GROUP(_f)	((_f) & TLM_IS_GROUP)
#define IS_DYNAMIC(_f)	((_f) & TLM_IS_DYNAMIC)
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
 * @vfs_inode: The embedded VFS inode that will be initialized and plugged
 *	       into the live filesystem at mount time.
 *
 * This structure is used to describe each SCMI Telemetry entity discovered
 * at probe time, store its related SCMI data, and link to the proper
 * telemetry class @scmi_tlm_class.
 */
struct scmi_tlm_inode {
	struct scmi_tlm_setup *tsp;
	const struct scmi_tlm_class *cls;
	union {
		const void *priv;
		const struct scmi_telemetry_de *de;
		const struct scmi_telemetry_group *grp;
		const struct scmi_telemetry_info *info;
	};
	struct inode vfs_inode;
};

#define to_tlm_inode(t)	container_of(t, struct scmi_tlm_inode, vfs_inode)

#define	MAX_INST_NAME		32

#define TOP_NODES_NUM		32
#define NODES_PER_DE_NUM	12
#define NODES_PER_GRP_NUM	 9

/**
 * struct scmi_tlm_instance  - Telemetry instance descriptor
 * @id: Progressive number identifying this probed instance; it will be used
 *	to name the top node at the root of this instance.
 * @res_enumerated: A flag to indicate if full resources enumeration has been
 *		    successfully performed.
 * @name: Name to be used for the top root node of the instance. (tlm_<id>)
 * @node: A node to link this in the list of all instances.
 * @sb: A reference to the current super_block.
 * @tsp: A reference to the SCMI instance data.
 * @top_cls: A class to represent the top node behaviour.
 * @top_dentry: A reference to the top dentry for this instance.
 * @des_dentry: A reference to the DES dentry for this instance.
 * @grps_dentry: A reference to the groups dentry for this instance.
 * @info: A handy reference to this instance SCMI Telemetry info data.
 *
 */
struct scmi_tlm_instance {
	int id;
	bool res_enumerated;
	char name[MAX_INST_NAME];
	struct list_head node;
	struct super_block *sb;
	struct scmi_tlm_setup *tsp;
	struct scmi_tlm_class top_cls;
	struct dentry *top_dentry;
	struct dentry *des_dentry;
	struct dentry *grps_dentry;
	const struct scmi_telemetry_info *info;
};

static int scmi_telemetry_instance_register(struct super_block *sb,
					    struct scmi_tlm_instance *ti);

static LIST_HEAD(scmi_telemetry_instances);

static struct inode *stlmfs_get_inode(struct super_block *sb)
{
	struct inode *inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		simple_inode_init_ts(inode);
	}

	return inode;
}

static int stlmfs_failed_creating(struct dentry *dentry)
{
	simple_done_creating(dentry);

	return -ENOMEM;
}

static struct dentry *
stlmfs_create_dentry(struct super_block *sb, struct scmi_tlm_setup *tsp,
		     struct dentry *parent, const struct scmi_tlm_class *cls,
		     const void *priv)
{
	struct scmi_tlm_inode *tlmi;
	struct dentry *dentry;
	struct inode *inode;

	if (!parent)
		parent = sb->s_root;

	if (IS_ERR(parent))
		return parent;

	dentry = simple_start_creating(parent, cls->name);
	if (IS_ERR(dentry))
		return dentry;

	inode = stlmfs_get_inode(sb);
	if (unlikely(!inode)) {
		dev_err(tsp->dev,
			"out of free dentries, cannot create '%s'",
			cls->name);
		return ERR_PTR(stlmfs_failed_creating(dentry));
	}

	if (S_ISDIR(cls->mode)) {
		inode->i_op = cls->i_op ?: &simple_dir_inode_operations;
		inode->i_fop = cls->f_op ?: &simple_dir_operations;
	} else {
		inode->i_op = cls->i_op ?: &simple_dir_inode_operations;
		inode->i_fop = cls->f_op;
	}

	inode->i_mode = cls->mode;
	inode_init_owner(&nop_mnt_idmap, inode, NULL, inode->i_mode);
	inode->i_private = (void *)priv;

	tlmi = to_tlm_inode(inode);

	tlmi->cls = cls;
	tlmi->tsp = tsp;
	tlmi->priv = priv;

	d_make_persistent(dentry, inode);

	simple_done_creating(dentry);

	return dentry;
}

static inline int
__scmi_tlm_generic_open(struct inode *ino, struct file *filp,
			int (*bulk_op)(struct scmi_tlm_setup *tsp,
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

static inline const struct scmi_telemetry_res_info *
scmi_telemetry_res_info_get(struct scmi_tlm_setup *tsp)
{
	const struct scmi_telemetry_res_info *rinfo;

	if (tsp->rinfo)
		return ACCESS_PRIVATE(tsp, rinfo);

	rinfo = tsp->ops->res_get(tsp->ph);
	/* Cache the retrieved resource info value */
	smp_store_mb(tsp->rinfo, rinfo);

	return rinfo;
}

static ssize_t scmi_tlm_all_des_write(struct file *filp,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(file_inode(filp));
	struct scmi_tlm_setup *tsp = tlmi->tsp;
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
		const struct scmi_telemetry_res_info *rinfo;

		rinfo = scmi_telemetry_res_info_get(tsp);
		if (!rinfo)
			return -ENODEV;

		for (int i = 0; i < rinfo->num_des; i++) {
			ret = tsp->ops->state_set(tsp->ph, false,
						  rinfo->des[i]->info->id,
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
	struct scmi_tlm_setup *tsp = tlmi->tsp;
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
	struct scmi_tlm_setup *tsp = tlmi->tsp;
	struct scmi_tlm_buffer *data = filp->private_data;
	bool is_group = IS_GROUP(tlmi->cls->flags);
	unsigned int update_interval_ms = 0, secs = 0;
	int ret, grp_id, exp = -3;
	char *p, *token;

	if (count >= SCMI_TLM_MAX_BUF_SZ)
		return -ENOSPC;

	if (copy_from_user(data->buf, buf, count))
		return -EFAULT;

	p = data->buf;
	token = strsep(&p, ",");
	if (!token || iscntrl(token[0]))
		return -EINVAL;

	ret = kstrtouint(strim(token), 0, &secs);
	if (ret)
		return ret;

	if (p) {
		token = p;
		if (!token || iscntrl(token[0]))
			return -EINVAL;

		ret = kstrtoint(strim(token), 0, &exp);
		if (ret)
			return ret;
	}

	update_interval_ms = SCMI_TLM_BUILD_UPDATE_INTERVAL(secs, exp);

	grp_id = !is_group ? SCMI_TLM_GRP_INVALID : tlmi->grp->info->id;
	ret = tsp->ops->collection_configure(tsp->ph, grp_id, !is_group, NULL,
					     &update_interval_ms, NULL);
	if (ret)
		return ret;

	return count;
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
	struct scmi_tlm_setup *tsp = tlmi->tsp;
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
	struct scmi_tlm_setup *tsp = tlmi->tsp;
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

	/*
	 * Note that tp->buf is a scratch buffer, filled once, used to support
	 * multiple chunked read and freed in scmi_tlm_priv_release.
	 */
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

	/*
	 * Note that tp->buf is a scratch buffer, filled once, used to support
	 * multiple chunked read and freed in scmi_tlm_priv_release.
	 */
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
	TLM_ANON_CLASS("all_des_enable", TLM_IS_STATE,
		       S_IFREG | S_IWUSR, &all_des_fops, NULL),
	TLM_ANON_CLASS("all_des_tstamp_enable", 0,
		       S_IFREG | S_IWUSR, &all_des_fops, NULL),
	TLM_ANON_CLASS("current_update_interval_ms", 0,
		       S_IFREG | S_IRUSR | S_IWUSR, &current_interval_fops, NULL),
	TLM_ANON_CLASS("intervals_discrete", 0,
		       S_IFREG | S_IRUSR, &intrv_discrete_fops, NULL),
	TLM_ANON_CLASS("available_update_intervals_ms", 0,
		       S_IFREG | S_IRUSR, &available_interv_fops, NULL),
	TLM_ANON_CLASS("de_implementation_version", 0,
		       S_IFREG | S_IRUSR, &de_impl_vers_fops, NULL),
	TLM_ANON_CLASS("tlm_enable", 0,
		       S_IFREG | S_IRUSR | S_IWUSR, &tlm_enable_fops, NULL),
	TLM_ANON_CLASS(NULL, 0, 0, NULL, NULL),
};

DEFINE_TLM_CLASS(reset_tlmo, "reset", 0, S_IFREG | S_IWUSR, &reset_fops, NULL);

DEFINE_TLM_CLASS(des_dir_cls, "des", 0,
		 S_IFDIR | S_IRWXU, NULL, NULL);
DEFINE_TLM_CLASS(name_tlmo, "name", 0,
		 S_IFREG | S_IRUSR, &string_ro_fops, NULL);
DEFINE_TLM_CLASS(ena_tlmo, "enable", TLM_IS_STATE,
		 S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL);
DEFINE_TLM_CLASS(tstamp_ena_tlmo, "tstamp_enable", 0,
		 S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL);
DEFINE_TLM_CLASS(type_tlmo, "type", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(unit_tlmo, "unit", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(unit_exp_tlmo, "unit_exp", 0,
		 S_IFREG | S_IRUSR, &sa_s32_ro_fops, NULL);
DEFINE_TLM_CLASS(instance_id_tlmo, "instance_id", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(compo_type_tlmo, "compo_type", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(compo_inst_id_tlmo, "compo_instance_id", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(tstamp_exp_tlmo, "tstamp_exp", 0,
		 S_IFREG | S_IRUSR, &sa_s32_ro_fops, NULL);
DEFINE_TLM_CLASS(persistent_tlmo, "persistent", 0,
		 S_IFREG | S_IRUSR, &sa_u32_ro_fops, NULL);
DEFINE_TLM_CLASS(value_tlmo, "value", 0,
		 S_IFREG | S_IRUSR, &de_read_fops, NULL);

static int scmi_telemetry_de_populate(struct super_block *sb,
				      struct scmi_tlm_setup *tsp,
				      struct dentry *parent,
				      const struct scmi_telemetry_de *de,
				      bool fully_enumerated)
{
	struct scmi_tlm_de_info *dei = de->info;

	stlmfs_create_dentry(sb, tsp, parent, &ena_tlmo, de);
	stlmfs_create_dentry(sb, tsp, parent, &value_tlmo, de);
	if (!fully_enumerated)
		return 0;

	if (de->name_support)
		stlmfs_create_dentry(sb, tsp, parent, &name_tlmo, dei->name);

	if (de->tstamp_support) {
		stlmfs_create_dentry(sb, tsp, parent, &tstamp_ena_tlmo, de);
		stlmfs_create_dentry(sb, tsp, parent, &tstamp_exp_tlmo,
				     &dei->tstamp_exp);
	}

	stlmfs_create_dentry(sb, tsp, parent, &type_tlmo, &dei->type);
	stlmfs_create_dentry(sb, tsp, parent, &unit_tlmo, &dei->unit);
	stlmfs_create_dentry(sb, tsp, parent, &unit_exp_tlmo, &dei->unit_exp);
	stlmfs_create_dentry(sb, tsp, parent, &instance_id_tlmo, &dei->instance_id);
	stlmfs_create_dentry(sb, tsp, parent, &compo_type_tlmo, &dei->compo_type);
	stlmfs_create_dentry(sb, tsp, parent, &compo_inst_id_tlmo,
			     &dei->compo_instance_id);
	stlmfs_create_dentry(sb, tsp, parent, &persistent_tlmo, &dei->persistent);

	return 0;
}

static int
scmi_telemetry_des_lazy_enumerate(struct scmi_tlm_instance *ti,
				  const struct scmi_telemetry_res_info *rinfo)
{
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct super_block *sb = ti->sb;

	for (int i = 0; i < rinfo->num_des; i++) {
		const struct scmi_telemetry_de *de = rinfo->des[i];
		struct dentry *de_dir_dentry;
		int ret;

		struct scmi_tlm_class *de_tlm_cls __free(kfree) =
			kzalloc(sizeof(*de_tlm_cls), GFP_KERNEL);
		if (!de_tlm_cls)
			return -ENOMEM;

		de_tlm_cls->name = kasprintf(GFP_KERNEL, "0x%08X", de->info->id);
		if (!de_tlm_cls->name)
			return -ENOMEM;

		de_tlm_cls->mode = S_IFDIR | S_IRWXU;
		de_tlm_cls->flags = TLM_IS_DYNAMIC;
		de_dir_dentry = stlmfs_create_dentry(sb, tsp, ti->des_dentry,
						     de_tlm_cls, de);

		ret = scmi_telemetry_de_populate(sb, tsp, de_dir_dentry, de,
						 rinfo->fully_enumerated);
		if (ret)
			return ret;

		retain_and_null_ptr(de_tlm_cls);
	}

	ti->res_enumerated = true;

	dev_info(tsp->dev, "Found %d Telemetry DE resources.\n", rinfo->num_des);

	return 0;
}

static int scmi_telemetry_des_initialize(struct scmi_tlm_instance *ti)
{
	const struct scmi_telemetry_res_info *rinfo;

	rinfo = scmi_telemetry_res_info_get(ti->tsp);
	if (!rinfo)
		return -ENODEV;

	return scmi_telemetry_des_lazy_enumerate(ti, rinfo);
}

DEFINE_TLM_CLASS(version_tlmo, "version", 0,
		 S_IFREG | S_IRUSR, &sa_x32_ro_fops, NULL);

static int scmi_tlm_bulk_on_demand(struct scmi_tlm_setup *tsp,
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
				   "0x%08X %llu %016llX\n", samples[idx].id,
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
	struct scmi_tlm_setup *tsp = tlmi->tsp;
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
		kfree(tp->buf);
		kfree(samples);
		return ret;
	}

	/*
	 * Note that tp->buf is a scratch buffer, filled once, used to support
	 * multiple chunked read and freed in scmi_tlm_priv_release.
	 */
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

DEFINE_TLM_CLASS(data_tlmo, "des_bulk_read", 0,
		 S_IFREG | S_IRUSR, &scmi_tlm_data_fops, NULL);

static int scmi_tlm_bulk_single_read(struct scmi_tlm_setup *tsp,
				     int res_id, int *num_samples,
				     struct scmi_telemetry_de_sample *samples)
{
	return tsp->ops->des_sample_get(tsp->ph, res_id, num_samples, samples);
}

static int scmi_tlm_single_read_open(struct inode *ino, struct file *filp)
{
	return __scmi_tlm_generic_open(ino, filp, scmi_tlm_bulk_single_read);
}

static const struct file_operations scmi_tlm_single_sample_fops = {
	.owner = THIS_MODULE,
	.open = scmi_tlm_single_read_open,
	.read = scmi_tlm_generic_data_read,
	.release = scmi_tlm_priv_release,
};

DEFINE_TLM_CLASS(single_sample_tlmo, "des_single_sample_read", 0,
		 S_IFREG | S_IRUSR, &scmi_tlm_single_sample_fops, NULL);

static const struct scmi_tlm_class tlm_grps[] = {
	TLM_ANON_CLASS("enable", TLM_IS_STATE | TLM_IS_GROUP,
		       S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL),
	TLM_ANON_CLASS("tstamp_enable", TLM_IS_GROUP,
		       S_IFREG | S_IRUSR | S_IWUSR, &obj_enable_fops, NULL),
	TLM_ANON_CLASS(NULL, 0, 0, NULL, NULL),
};

DEFINE_TLM_CLASS(grp_data_tlmo, "des_bulk_read", TLM_IS_GROUP,
		 S_IFREG | S_IRUSR, &scmi_tlm_data_fops, NULL);

DEFINE_TLM_CLASS(groups_dir_cls, "groups", 0, S_IFDIR | S_IRWXU, NULL, NULL);

DEFINE_TLM_CLASS(grp_single_sample_tlmo, "des_single_sample_read", TLM_IS_GROUP,
		 S_IFREG | S_IRUSR, &scmi_tlm_single_sample_fops, NULL);

DEFINE_TLM_CLASS(grp_composing_des_tlmo, "composing_des", TLM_IS_GROUP,
		 S_IFREG | S_IRUSR, &string_ro_fops, NULL);

DEFINE_TLM_CLASS(grp_current_interval_tlmo, "current_update_interval_ms",
		 TLM_IS_GROUP, S_IFREG | S_IRUSR | S_IWUSR,
		 &current_interval_fops, NULL);

DEFINE_TLM_CLASS(grp_available_interval_tlmo, "available_update_intervals_ms",
		 TLM_IS_GROUP, S_IFREG | S_IRUSR, &available_interv_fops, NULL);

DEFINE_TLM_CLASS(grp_intervals_discrete_tlmo, "intervals_discrete",
		 TLM_IS_GROUP, S_IFREG | S_IRUSR, &intrv_discrete_fops, NULL);

static int scmi_telemetry_groups_initialize(struct scmi_tlm_instance *ti)
{
	const struct scmi_telemetry_res_info *rinfo;
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct super_block *sb = ti->sb;
	struct device *dev = tsp->dev;
	struct dentry *grp_dir_dentry;

	if (ti->info->base.num_groups == 0)
		return 0;

	rinfo = scmi_telemetry_res_info_get(tsp);
	if (!rinfo)
		return -ENODEV;

	for (int i = 0; i < rinfo->num_groups; i++) {
		const struct scmi_telemetry_group *grp = &rinfo->grps[i];

		struct scmi_tlm_class *grp_tlm_cls __free(kfree) =
			kzalloc(sizeof(*grp_tlm_cls), GFP_KERNEL);
		if (!grp_tlm_cls)
			return -ENOMEM;

		grp_tlm_cls->name = kasprintf(GFP_KERNEL, "%u", grp->info->id);
		if (!grp_tlm_cls->name)
			return -ENOMEM;

		grp_tlm_cls->mode = S_IFDIR | S_IRWXU;
		grp_tlm_cls->flags = TLM_IS_DYNAMIC;

		grp_dir_dentry = stlmfs_create_dentry(sb, tsp, ti->grps_dentry,
						      grp_tlm_cls, grp);

		for (const struct scmi_tlm_class *gto = tlm_grps; gto->name; gto++)
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry, gto, grp);

		stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
				     &grp_composing_des_tlmo, grp->des_str);

		stlmfs_create_dentry(sb, tsp, grp_dir_dentry, &grp_data_tlmo, grp);
		stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
				     &grp_single_sample_tlmo, grp);

		if (ti->info->per_group_config_support) {
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
					     &grp_current_interval_tlmo, grp);
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
					     &grp_available_interval_tlmo, grp);
			stlmfs_create_dentry(sb, tsp, grp_dir_dentry,
					     &grp_intervals_discrete_tlmo, grp);
		}

		retain_and_null_ptr(grp_tlm_cls);
	}

	dev_info(dev, "Found %d Telemetry GROUPS resources.\n",
		 rinfo->num_groups);

	return 0;
}

static struct scmi_tlm_instance *scmi_tlm_init(struct scmi_tlm_setup *tsp,
					       int instance_id)
{
	struct device *dev = tsp->dev;
	struct scmi_tlm_instance *ti;

	ti = devm_kzalloc(dev, sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return ERR_PTR(-ENOMEM);

	ti->info = tsp->ops->info_get(tsp->ph);
	if (!ti->info)
		return dev_err_ptr_probe(dev,
					 -EINVAL, "invalid Telemetry info !\n");

	ti->id = instance_id;
	ti->tsp = tsp;

	return ti;
}

static int scmi_telemetry_probe(struct scmi_device *sdev)
{
	const struct scmi_handle *handle = sdev->handle;
	struct scmi_protocol_handle *ph;
	struct device *dev = &sdev->dev;
	struct scmi_tlm_instance *ti;
	struct scmi_tlm_setup *tsp;
	struct super_block *sb;
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

	mutex_lock(&stlmfs_mtx);
	list_add(&ti->node, &scmi_telemetry_instances);
	sb = stlmfs_sb;
	mutex_unlock(&stlmfs_mtx);

	/*
	 * If the file system was already mounted by the time this
	 * instance was probed, register explicitly, since the list
	 * has been scanned already.
	 */
	if (sb) {
		int ret;

		ret = scmi_telemetry_instance_register(sb, ti);
		if (ret) {
			dev_err(dev, "Failed to register instance %u at probe.\n",
				ti->id);
			return ret;
		}
	}

	return 0;
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_TELEMETRY, "telemetry" },
	{ }
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_telemetry_driver = {
	.name = "scmi-telemetry-driver",
	.probe = scmi_telemetry_probe,
	.id_table = scmi_id_table,
};

static struct inode *stlmfs_alloc_inode(struct super_block *sb)
{
	struct scmi_tlm_inode *tlmi;

	tlmi = alloc_inode_sb(sb, stlmfs_inode_cachep, GFP_KERNEL);
	if (!tlmi)
		return NULL;

	tlmi->cls = NULL;

	return &tlmi->vfs_inode;
}

static void stlmfs_free_inode(struct inode *inode)
{
	struct scmi_tlm_inode *tlmi = to_tlm_inode(inode);

	if (tlmi->cls && IS_DYNAMIC(tlmi->cls->flags)) {
		kfree(tlmi->cls->name);
		kfree(tlmi->cls);
	}

	kmem_cache_free(stlmfs_inode_cachep, tlmi);
}

static const struct super_operations tlm_sops = {
	.statfs = simple_statfs,
	.alloc_inode = stlmfs_alloc_inode,
	.free_inode = stlmfs_free_inode,
};

static struct dentry *stlmfs_create_root_dentry(struct super_block *sb)
{
	struct dentry *dentry;
	struct inode *inode;

	inode = stlmfs_get_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFDIR | S_IRWXU);

	dentry = d_make_root(inode);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	return dentry;
}

static int scmi_tlm_root_dentries_initialize(struct scmi_tlm_instance *ti)
{
	struct scmi_tlm_setup *tsp = ti->tsp;
	struct super_block *sb = ti->sb;

	scnprintf(ti->name, MAX_INST_NAME, "tlm_%d", ti->id);

	/* Allocate top instance node */
	ti->top_cls.name = ti->name;
	ti->top_cls.mode = S_IFDIR | S_IRWXU;

	/* Create the root of this instance */
	ti->top_dentry = stlmfs_create_dentry(sb, tsp, sb->s_root, &ti->top_cls, NULL);
	for (const struct scmi_tlm_class *tlmo = tlm_tops; tlmo->name; tlmo++)
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, tlmo, ti->info);

	if (ti->info->reset_support)
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, &reset_tlmo, NULL);

	stlmfs_create_dentry(sb, tsp, ti->top_dentry, &version_tlmo,
			     &ti->info->base.version);
	stlmfs_create_dentry(sb, tsp, ti->top_dentry, &data_tlmo, ti->info);
	stlmfs_create_dentry(sb, tsp, ti->top_dentry, &single_sample_tlmo, ti->info);
	ti->des_dentry =
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, &des_dir_cls, NULL);
	ti->grps_dentry =
		stlmfs_create_dentry(sb, tsp, ti->top_dentry, &groups_dir_cls, NULL);

	return 0;
}

static int scmi_telemetry_instance_register(struct super_block *sb,
					    struct scmi_tlm_instance *ti)
{
	int ret;

	ti->sb = sb;
	ret = scmi_tlm_root_dentries_initialize(ti);
	if (ret)
		return ret;

	ret = scmi_telemetry_des_initialize(ti);
	if (ret)
		return ret;

	ret = scmi_telemetry_groups_initialize(ti);
	if (ret) {
		dev_warn(ti->tsp->dev,
			 "Failed to initialize groups for instance %s.\n",
			 ti->top_cls.name);
	}

	return 0;
}

static int stlmfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct scmi_tlm_instance *ti;
	struct dentry *root_dentry;
	int ret;

	sb->s_magic = TLM_FS_MAGIC;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_op = &tlm_sops;

	root_dentry = stlmfs_create_root_dentry(sb);
	if (IS_ERR(root_dentry))
		return PTR_ERR(root_dentry);

	sb->s_root = root_dentry;

	mutex_lock(&stlmfs_mtx);
	list_for_each_entry(ti, &scmi_telemetry_instances, node) {
		mutex_unlock(&stlmfs_mtx);
		ret = scmi_telemetry_instance_register(sb, ti);
		if (ret)
			dev_err(ti->tsp->dev,
				"Failed to register instance %u.\n", ti->id);
		mutex_lock(&stlmfs_mtx);
	}
	stlmfs_sb = sb;
	mutex_unlock(&stlmfs_mtx);

	return 0;
}

static int stlmfs_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, stlmfs_fill_super);
}

static const struct fs_context_operations stlmfs_fc_ops = {
	.get_tree = stlmfs_get_tree,
};

static int stlmfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &stlmfs_fc_ops;

	return 0;
}

static void stlmfs_kill_sb(struct super_block *sb)
{
	kill_anon_super(sb);
}

static struct file_system_type scmi_telemetry_fs = {
	.owner = THIS_MODULE,
	.name = TLM_FS_NAME,
	.kill_sb = stlmfs_kill_sb,
	.init_fs_context = stlmfs_init_fs_context,
	.fs_flags = 0,
};

static void stlmfs_init_once(void *arg)
{
	struct scmi_tlm_inode *tlmi = arg;

	inode_init_once(&tlmi->vfs_inode);
}

static int __init scmi_telemetry_init(void)
{
	int ret;

	ret = sysfs_create_mount_point(fs_kobj, TLM_FS_MNT);
	if (ret && ret != -EEXIST)
		return ret;

	stlmfs_inode_cachep = kmem_cache_create("stlmfs_inode_cache",
						sizeof(struct scmi_tlm_inode), 0,
						SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
						stlmfs_init_once);
	if (!stlmfs_inode_cachep) {
		ret = -ENOMEM;
		goto out_mnt;
	}

	ret = register_filesystem(&scmi_telemetry_fs);
	if (ret)
		goto out_kmem;

	ret = scmi_register(&scmi_telemetry_driver);
	if (ret)
		goto out_reg;

	return 0;

out_reg:
	unregister_filesystem(&scmi_telemetry_fs);
out_kmem:
	kmem_cache_destroy(stlmfs_inode_cachep);
out_mnt:
	sysfs_remove_mount_point(fs_kobj, TLM_FS_MNT);

	return ret;
}
module_init(scmi_telemetry_init);

static void __exit scmi_telemetry_exit(void)
{
	int ret;

	scmi_unregister(&scmi_telemetry_driver);
	ret = unregister_filesystem(&scmi_telemetry_fs);
	if (ret)
		pr_err("Failed to unregister %s\n", TLM_FS_NAME);

	sysfs_remove_mount_point(fs_kobj, TLM_FS_MNT);
	kmem_cache_destroy(stlmfs_inode_cachep);
}
module_exit(scmi_telemetry_exit);

MODULE_AUTHOR("Cristian Marussi <cristian.marussi@arm.com>");
MODULE_DESCRIPTION("ARM SCMI Telemetry Driver");
MODULE_LICENSE("GPL");
