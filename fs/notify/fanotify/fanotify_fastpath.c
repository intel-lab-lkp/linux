// SPDX-License-Identifier: GPL-2.0
#include <linux/fanotify.h>
#include <linux/module.h>
#include <linux/bpf.h>

#include "fanotify.h"

extern struct srcu_struct fsnotify_mark_srcu;

static DEFINE_SPINLOCK(fp_list_lock);
static LIST_HEAD(fp_list);

static struct fanotify_fastpath_ops *fanotify_fastpath_find(const char *name)
{
	struct fanotify_fastpath_ops *ops;

	list_for_each_entry(ops, &fp_list, list) {
		if (!strcmp(ops->name, name))
			return ops;
	}
	return NULL;
}


/*
 * fanotify_fastpath_register - Register a new fastpath handler.
 *
 * Add a fastpath handler to the fp_list. These fastpath handlers are
 * available for all users in the system.
 *
 * @ops:	pointer to fanotify_fastpath_ops to add.
 *
 * Returns:
 *	0	- on success;
 *	-EEXIST	- fastpath handler of the same name already exists.
 */
int fanotify_fastpath_register(struct fanotify_fastpath_ops *ops)
{
	spin_lock(&fp_list_lock);
	if (fanotify_fastpath_find(ops->name)) {
		/* cannot register two handlers with the same name */
		spin_unlock(&fp_list_lock);
		return -EEXIST;
	}
	list_add_tail(&ops->list, &fp_list);
	spin_unlock(&fp_list_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(fanotify_fastpath_register);

/*
 * fanotify_fastpath_unregister - Unregister a new fastpath handler.
 *
 * Remove a fastpath handler from fp_list.
 *
 * @ops:	pointer to fanotify_fastpath_ops to remove.
 */
void fanotify_fastpath_unregister(struct fanotify_fastpath_ops *ops)
{
	spin_lock(&fp_list_lock);
	list_del_init(&ops->list);
	spin_unlock(&fp_list_lock);
}
EXPORT_SYMBOL_GPL(fanotify_fastpath_unregister);

/*
 * fanotify_fastpath_add - Add a fastpath handler to fsnotify_group.
 *
 * Add a fastpath handler from fp_list to a fsnotify_group.
 *
 * @group:	fsnotify_group that will have add
 * @argp:	fanotify_fastpath_args that specifies the fastpath handler
 *		and the init arguments of the fastpath handler.
 *
 * Returns:
 *	0	- on success;
 *	-EEXIST	- fastpath handler of the same name already exists.
 */
int fanotify_fastpath_add(struct fsnotify_group *group,
			  struct fanotify_fastpath_args __user *argp)
{
	struct fanotify_fastpath_hook *fp_hook;
	struct fanotify_fastpath_ops *fp_ops;
	struct fanotify_fastpath_args args;
	int ret = 0;

	ret = copy_from_user(&args, argp, sizeof(args));
	if (ret)
		return -EFAULT;

	if (args.version != 1 || args.flags || args.init_args_len > FAN_FP_ARGS_MAX)
		return -EINVAL;

	args.name[FAN_FP_NAME_MAX - 1] = '\0';

	fsnotify_group_lock(group);

	if (rcu_access_pointer(group->fanotify_data.fp_hook)) {
		fsnotify_group_unlock(group);
		return -EBUSY;
	}

	fp_hook = kzalloc(sizeof(*fp_hook), GFP_KERNEL);
	if (!fp_hook) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock(&fp_list_lock);
	fp_ops = fanotify_fastpath_find(args.name);
	if (!fp_ops || !bpf_try_module_get(fp_ops, fp_ops->owner)) {
		spin_unlock(&fp_list_lock);
		ret = -ENOENT;
		goto err_free_hook;
	}
	spin_unlock(&fp_list_lock);

	if (fp_ops->fp_init) {
		char *init_args = NULL;

		if (args.init_args_len) {
			init_args = strndup_user(u64_to_user_ptr(args.init_args),
						 args.init_args_len);
			if (IS_ERR(init_args)) {
				ret = PTR_ERR(init_args);
				if (ret == -EINVAL)
					ret = -E2BIG;
				goto err_module_put;
			}
		}
		ret = fp_ops->fp_init(fp_hook, init_args);
		kfree(init_args);
		if (ret)
			goto err_module_put;
	}
	fp_hook->ops = fp_ops;
	rcu_assign_pointer(group->fanotify_data.fp_hook, fp_hook);

out:
	fsnotify_group_unlock(group);
	return ret;

err_module_put:
	bpf_module_put(fp_ops, fp_ops->owner);
err_free_hook:
	kfree(fp_hook);
	goto out;
}

void fanotify_fastpath_hook_free(struct fanotify_fastpath_hook *fp_hook)
{
	if (fp_hook->ops->fp_free)
		fp_hook->ops->fp_free(fp_hook);

	bpf_module_put(fp_hook->ops, fp_hook->ops->owner);
}

void fanotify_fastpath_del(struct fsnotify_group *group)
{
	struct fanotify_fastpath_hook *fp_hook;

	fsnotify_group_lock(group);
	fp_hook = group->fanotify_data.fp_hook;
	if (!fp_hook)
		goto out;

	rcu_assign_pointer(group->fanotify_data.fp_hook, NULL);
	fanotify_fastpath_hook_free(fp_hook);

out:
	fsnotify_group_unlock(group);
}

__bpf_kfunc_start_defs();

/**
 * bpf_fanotify_data_inode - get inode from fanotify_fastpath_event
 *
 * @event: fanotify_fastpath_event to get inode from
 *
 * Get referenced inode from fanotify_fastpath_event.
 *
 * Return: A refcounted inode or NULL.
 *
 */
__bpf_kfunc struct inode *bpf_fanotify_data_inode(struct fanotify_fastpath_event *event)
{
	struct inode *inode = fsnotify_data_inode(event->data, event->data_type);

	return inode ? igrab(inode) : NULL;
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(bpf_fanotify_kfunc_set_ids)
BTF_ID_FLAGS(func, bpf_fanotify_data_inode,
	     KF_ACQUIRE | KF_TRUSTED_ARGS | KF_RET_NULL)
BTF_KFUNCS_END(bpf_fanotify_kfunc_set_ids)

static const struct btf_kfunc_id_set bpf_fanotify_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &bpf_fanotify_kfunc_set_ids,
};

static const struct bpf_func_proto *
bpf_fanotify_fastpath_get_func_proto(enum bpf_func_id func_id,
				     const struct bpf_prog *prog)
{
	return tracing_prog_func_proto(func_id, prog);
}

static bool bpf_fanotify_fastpath_is_valid_access(int off, int size,
						  enum bpf_access_type type,
						  const struct bpf_prog *prog,
						  struct bpf_insn_access_aux *info)
{
	if (!bpf_tracing_btf_ctx_access(off, size, type, prog, info))
		return false;

	return true;
}

static int bpf_fanotify_fastpath_btf_struct_access(struct bpf_verifier_log *log,
						   const struct bpf_reg_state *reg,
						   int off, int size)
{
	return 0;
}

static const struct bpf_verifier_ops bpf_fanotify_fastpath_verifier_ops = {
	.get_func_proto		= bpf_fanotify_fastpath_get_func_proto,
	.is_valid_access	= bpf_fanotify_fastpath_is_valid_access,
	.btf_struct_access	= bpf_fanotify_fastpath_btf_struct_access,
};

static int bpf_fanotify_fastpath_reg(void *kdata, struct bpf_link *link)
{
	return fanotify_fastpath_register(kdata);
}

static void bpf_fanotify_fastpath_unreg(void *kdata, struct bpf_link *link)
{
	fanotify_fastpath_unregister(kdata);
}

static int bpf_fanotify_fastpath_init(struct btf *btf)
{
	return 0;
}

static int bpf_fanotify_fastpath_init_member(const struct btf_type *t,
					     const struct btf_member *member,
					     void *kdata, const void *udata)
{
	const struct fanotify_fastpath_ops *uops;
	struct fanotify_fastpath_ops *ops;
	u32 moff;
	int ret;

	uops = (const struct fanotify_fastpath_ops *)udata;
	ops = (struct fanotify_fastpath_ops *)kdata;

	moff = __btf_member_bit_offset(t, member) / 8;
	switch (moff) {
	case offsetof(struct fanotify_fastpath_ops, name):
		ret = bpf_obj_name_cpy(ops->name, uops->name,
				       sizeof(ops->name));
		if (ret <= 0)
			return -EINVAL;
		return 1;
	}

	return 0;
}

static int __bpf_fan_fp_handler(struct fsnotify_group *group,
				struct fanotify_fastpath_hook *fp_hook,
				struct fanotify_fastpath_event *fp_event)
{
	return 0;
}

static int __bpf_fan_fp_init(struct fanotify_fastpath_hook *hook, const char *args)
{
	return 0;
}

static void __bpf_fan_fp_free(struct fanotify_fastpath_hook *hook)
{
}

/* For bpf_struct_ops->cfi_stubs */
static struct fanotify_fastpath_ops __bpf_fanotify_fastpath_ops = {
	.fp_handler = __bpf_fan_fp_handler,
	.fp_init = __bpf_fan_fp_init,
	.fp_free = __bpf_fan_fp_free,
};

static struct bpf_struct_ops bpf_fanotify_fastpath_ops = {
	.verifier_ops = &bpf_fanotify_fastpath_verifier_ops,
	.reg = bpf_fanotify_fastpath_reg,
	.unreg = bpf_fanotify_fastpath_unreg,
	.init = bpf_fanotify_fastpath_init,
	.init_member = bpf_fanotify_fastpath_init_member,
	.name = "fanotify_fastpath_ops",
	.cfi_stubs = &__bpf_fanotify_fastpath_ops,
	.owner = THIS_MODULE,
};

static int __init bpf_fanotify_fastpath_struct_ops_init(void)
{
	int ret;

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &bpf_fanotify_kfunc_set);
	ret = ret ?: register_bpf_struct_ops(&bpf_fanotify_fastpath_ops, fanotify_fastpath_ops);
	return ret;
}
late_initcall(bpf_fanotify_fastpath_struct_ops_init);
