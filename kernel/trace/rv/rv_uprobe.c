// SPDX-License-Identifier: GPL-2.0
/*
 * Generic uprobe infrastructure for RV monitors.
 *
 */
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/uprobes.h>
#include <rv/rv_uprobe.h>

/*
 * Private extension of struct rv_uprobe.  Allocated by rv_uprobe_attach*()
 * and returned to callers as &impl->pub.
 */
struct rv_uprobe_impl {
	struct rv_uprobe	pub;	/* must be first; callers hold &pub */
	struct uprobe_consumer	uc;
	struct uprobe		*uprobe;
	int (*entry_fn)(struct rv_uprobe *p, struct pt_regs *regs, __u64 *data);
	int (*ret_fn)(struct rv_uprobe *p, unsigned long func,
			struct pt_regs *regs, __u64 *data);
};

static int rv_uprobe_handler(struct uprobe_consumer *uc,
			     struct pt_regs *regs, __u64 *data)
{
	struct rv_uprobe_impl *impl = container_of(uc, struct rv_uprobe_impl, uc);

	if (impl->entry_fn)
		return impl->entry_fn(&impl->pub, regs, data);
	return 0;
}

static int rv_uprobe_ret_handler(struct uprobe_consumer *uc,
				 unsigned long func,
				 struct pt_regs *regs, __u64 *data)
{
	struct rv_uprobe_impl *impl = container_of(uc, struct rv_uprobe_impl, uc);

	if (impl->ret_fn)
		return impl->ret_fn(&impl->pub, func, regs, data);
	return 0;
}

static struct rv_uprobe *
__rv_uprobe_attach(struct inode *inode, struct path *path, loff_t offset,
		   int (*entry_fn)(struct rv_uprobe *p, struct pt_regs *regs, __u64 *data),
		   int (*ret_fn)(struct rv_uprobe *p, unsigned long func,
				   struct pt_regs *regs, __u64 *data),
		   void *priv)
{
	struct rv_uprobe_impl *impl;
	int ret;

	if (!entry_fn && !ret_fn)
		return ERR_PTR(-EINVAL);

	impl = kzalloc_obj(*impl, GFP_KERNEL);
	if (!impl)
		return ERR_PTR(-ENOMEM);

	impl->pub.offset = offset;
	impl->pub.priv   = priv;
	impl->entry_fn   = entry_fn;
	impl->ret_fn     = ret_fn;
	path_get(path);
	impl->pub.path   = *path;

	if (entry_fn)
		impl->uc.handler     = rv_uprobe_handler;
	if (ret_fn)
		impl->uc.ret_handler = rv_uprobe_ret_handler;

	impl->uprobe = uprobe_register(inode, offset, 0, &impl->uc);
	if (IS_ERR(impl->uprobe)) {
		ret = PTR_ERR(impl->uprobe);
		path_put(&impl->pub.path);
		kfree(impl);
		return ERR_PTR(ret);
	}

	return &impl->pub;
}

/**
 * rv_uprobe_attach_path - register an uprobe given an already-resolved path
 */
struct rv_uprobe *rv_uprobe_attach_path(struct path *path, loff_t offset,
	int (*entry_fn)(struct rv_uprobe *p, struct pt_regs *regs, __u64 *data),
	int (*ret_fn)(struct rv_uprobe *p, unsigned long func,
			struct pt_regs *regs, __u64 *data),
	void *priv)
{
	struct inode *inode = d_real_inode(path->dentry);

	return __rv_uprobe_attach(inode, path, offset, entry_fn, ret_fn, priv);
}
EXPORT_SYMBOL_GPL(rv_uprobe_attach_path);

/**
 * rv_uprobe_attach - resolve binpath and register an uprobe
 */
struct rv_uprobe *rv_uprobe_attach(const char *binpath, loff_t offset,
	int (*entry_fn)(struct rv_uprobe *p, struct pt_regs *regs, __u64 *data),
	int (*ret_fn)(struct rv_uprobe *p, unsigned long func,
			struct pt_regs *regs, __u64 *data),
	void *priv)
{
	struct rv_uprobe *p;
	struct path path;
	int ret;

	ret = kern_path(binpath, LOOKUP_FOLLOW, &path);
	if (ret)
		return ERR_PTR(ret);

	if (!d_is_reg(path.dentry)) {
		path_put(&path);
		return ERR_PTR(-EINVAL);
	}

	p = rv_uprobe_attach_path(&path, offset, entry_fn, ret_fn, priv);
	path_put(&path);
	return p;
}
EXPORT_SYMBOL_GPL(rv_uprobe_attach);

/**
 * rv_uprobe_detach - synchronously unregister an uprobe and free it
 */
void rv_uprobe_detach(struct rv_uprobe *p)
{
	if (!p)
		return;

	rv_uprobe_unregister_nosync(p);
	/*
	 * uprobe_unregister_sync() is a global barrier: it waits for all
	 * in-flight uprobe handlers across the entire system to complete,
	 * not just handlers for this probe.  This is intentional — it
	 * guarantees that no handler touching impl->pub.priv is running by
	 * the time we return, even if the caller immediately frees priv.
	 */
	rv_uprobe_sync();
	rv_uprobe_free(p);
}
EXPORT_SYMBOL_GPL(rv_uprobe_detach);

/**
 * rv_uprobe_unregister_nosync - dequeue an uprobe without waiting
 */
void rv_uprobe_unregister_nosync(struct rv_uprobe *p)
{
	struct rv_uprobe_impl *impl;

	if (!p)
		return;

	impl = container_of(p, struct rv_uprobe_impl, pub);
	uprobe_unregister_nosync(impl->uprobe, &impl->uc);
}
EXPORT_SYMBOL_GPL(rv_uprobe_unregister_nosync);

/**
 * rv_uprobe_sync - wait for all in-flight uprobe handlers to complete
 */
void rv_uprobe_sync(void)
{
	uprobe_unregister_sync();
}
EXPORT_SYMBOL_GPL(rv_uprobe_sync);

/**
 * rv_uprobe_free - release resources of a previously deregistered probe
 */
void rv_uprobe_free(struct rv_uprobe *p)
{
	struct rv_uprobe_impl *impl;

	if (!p)
		return;

	impl = container_of(p, struct rv_uprobe_impl, pub);
	path_put(&p->path);
	kfree(impl);
}
EXPORT_SYMBOL_GPL(rv_uprobe_free);
