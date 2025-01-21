// SPDX-License-Identifier: GPL-2.0-only
/*
 * A sample kernel module showing how to build a pseudo-filesystem on top of
 * kernfs.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/kernfs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define SAMPLE_KERNFS_MAGIC 0x8d000ff0

/**
 * struct sample_kernfs_directory - Represents a directory in the pseudo-filesystem
 * @count: Holds the current count in the counter file.
 * @inc: Amount to increment count by. Value of inc file.
 * @subdirs: Holds the list of this directory's subdirectories.
 * @siblings: Used to add this dir to parent's subdirs list.
 */
struct sample_kernfs_directory {
	atomic64_t count;
	atomic64_t inc;
	struct list_head subdirs;
	struct list_head siblings;
};

static struct sample_kernfs_directory *sample_kernfs_create_dir(void)
{
	struct sample_kernfs_directory *dir;

	dir = kzalloc(sizeof(struct sample_kernfs_directory), GFP_KERNEL);
	if (!dir)
		return NULL;

	atomic64_set(&dir->inc, 1);
	INIT_LIST_HEAD(&dir->subdirs);
	INIT_LIST_HEAD(&dir->siblings);

	return dir;
}

static struct sample_kernfs_directory *kernfs_of_to_dir(struct kernfs_open_file *of)
{
	struct kernfs_node *dir_kn = kernfs_get_parent(of->kn);
	struct sample_kernfs_directory *dir = dir_kn->priv;

	/* kernfs_get_parent adds a reference; drop it with kernfs_put */
	kernfs_put(dir_kn);

	return dir;
}

static int sample_kernfs_counter_seq_show(struct seq_file *sf, void *v)
{
	struct kernfs_open_file *of = sf->private;
	struct sample_kernfs_directory *counter_dir = kernfs_of_to_dir(of);
	u64 inc = atomic64_read(&counter_dir->inc);
	u64 count = atomic64_add_return(inc, &counter_dir->count);

	seq_printf(sf, "%llu\n", count);

	return 0;
}

static ssize_t sample_kernfs_counter_write(struct kernfs_open_file *of, char *buf,
					   size_t nbytes, loff_t off)
{
	struct sample_kernfs_directory *counter_dir = kernfs_of_to_dir(of);
	int ret;
	u64 new_value;

	ret = kstrtou64(strstrip(buf), 10, &new_value);
	if (ret)
		return ret;

	atomic64_set(&counter_dir->count, new_value);

	return nbytes;
}

static struct kernfs_ops counter_kf_ops = {
	.seq_show	= sample_kernfs_counter_seq_show,
	.write		= sample_kernfs_counter_write,
};

static int sample_kernfs_inc_seq_show(struct seq_file *sf, void *v)
{
	struct kernfs_open_file *of = sf->private;
	struct sample_kernfs_directory *counter_dir = kernfs_of_to_dir(of);
	u64 inc = atomic64_read(&counter_dir->inc);

	seq_printf(sf, "%llu\n", inc);

	return 0;
}

static ssize_t sample_kernfs_inc_write(struct kernfs_open_file *of, char *buf,
					   size_t nbytes, loff_t off)
{
	struct sample_kernfs_directory *counter_dir = kernfs_of_to_dir(of);
	int ret;
	u64 new_value;

	ret = kstrtou64(strstrip(buf), 10, &new_value);
	if (ret)
		return ret;

	atomic64_set(&counter_dir->inc, new_value);

	return nbytes;
}

static struct kernfs_ops inc_kf_ops = {
	.seq_show	= sample_kernfs_inc_seq_show,
	.write		= sample_kernfs_inc_write,
};

static int sample_kernfs_add_file(struct kernfs_node *dir_kn, const char *name,
				  struct kernfs_ops *ops)
{
	struct kernfs_node *kn;

	kn = __kernfs_create_file(dir_kn, name, 0666, current_fsuid(),
				  current_fsgid(), 0, ops, NULL, NULL, NULL);

	if (IS_ERR(kn))
		return PTR_ERR(kn);

	return 0;
}

static int sample_kernfs_populate_dir(struct kernfs_node *dir_kn)
{
	int err;

	err = sample_kernfs_add_file(dir_kn, "counter", &counter_kf_ops);
	if (err)
		return err;

	err = sample_kernfs_add_file(dir_kn, "inc", &inc_kf_ops);
	if (err)
		return err;

	return 0;
}

static void sample_kernfs_remove_subtree(struct sample_kernfs_directory *dir)
{
	struct sample_kernfs_directory *child, *tmp;

	/*
	 * Recursively remove children. This approach is acceptable for this
	 * sample since we expect the tree depth to remain small and manageable.
	 * For real-world filesystems, an iterative approach should be used to
	 * avoid stack overflows.
	 *
	 * Also, we could be more careful with locking our lists, but kernfs
	 * holds a tree-wide lock before calling our rmdir, so we should be
	 * safe.
	 */
	list_for_each_entry_safe(child, tmp, &dir->subdirs, siblings) {
		sample_kernfs_remove_subtree(child);
	}

	/* Remove this directory from its parent's subdirs list */
	list_del(&dir->siblings);

	kfree(dir);
}

static int sample_kernfs_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode)
{
	struct kernfs_node *dir_kn;
	struct sample_kernfs_directory *dir, *parent_dir;
	int ret;

	dir = sample_kernfs_create_dir();
	if (!dir)
		return -ENOMEM;

	/* dir gets stored in dir_kn->priv so we can access it later. */
	dir_kn = kernfs_create_dir_ns(parent_kn, name, mode, current_fsuid(),
				      current_fsgid(), dir, NULL);

	if (IS_ERR(dir_kn)) {
		ret = PTR_ERR(dir_kn);
		goto err_free_dir;
	}

	ret = sample_kernfs_populate_dir(dir_kn);
	if (ret)
		goto err_free_dir_kn;

	/* Add directory to parent->subdirs */
	parent_dir = parent_kn->priv;
	list_add(&dir->siblings, &parent_dir->subdirs);

	return 0;

err_free_dir_kn:
	kernfs_remove(dir_kn);
err_free_dir:
	sample_kernfs_remove_subtree(dir);
	return ret;
}

static int sample_kernfs_rmdir(struct kernfs_node *kn)
{
	struct sample_kernfs_directory *dir = kn->priv;

	/*
	 * kernfs_remove_self avoids a deadlock by breaking active protection;
	 * see kernfs_break_active_protection(). This is required since
	 * kernfs_iop_rmdir() holds a tree-wide lock.
	 */
	kernfs_remove_self(kn);

	sample_kernfs_remove_subtree(dir);

	return 0;
}

static struct kernfs_syscall_ops sample_kernfs_kf_syscall_ops = {
	.mkdir		= sample_kernfs_mkdir,
	.rmdir		= sample_kernfs_rmdir,
};

static void sample_kernfs_fs_context_free(struct fs_context *fc)
{
	struct kernfs_fs_context *kfc = fc->fs_private;

	kernfs_free_fs_context(fc);
	kfree(kfc);
}

static const struct fs_context_operations sample_kernfs_fs_context_ops = {
	.get_tree	= kernfs_get_tree,
	.free		= sample_kernfs_fs_context_free,
};

static int sample_kernfs_init_fs_context(struct fs_context *fc)
{
	struct kernfs_fs_context *kfc;
	struct sample_kernfs_directory *root_dir;
	struct kernfs_root *root;
	int err;

	kfc = kzalloc(sizeof(struct kernfs_fs_context), GFP_KERNEL);
	if (!kfc)
		return -ENOMEM;

	root_dir = sample_kernfs_create_dir();
	if (!root_dir) {
		err = -ENOMEM;
		goto err_free_kfc;
	}

	/* dir gets stored in root->priv so we can access it later. */
	root = kernfs_create_root(&sample_kernfs_kf_syscall_ops, 0, root_dir);
	if (IS_ERR(root)) {
		err = PTR_ERR(root);
		goto err_free_dir;
	}

	kfc->root = root;
	kfc->magic = SAMPLE_KERNFS_MAGIC;
	fc->fs_private = kfc;
	fc->ops = &sample_kernfs_fs_context_ops;
	fc->global = true;

	err = sample_kernfs_populate_dir(kernfs_root_to_node(root));
	if (err)
		goto err_free_root;

	return 0;

err_free_root:
	kernfs_destroy_root(root);
err_free_dir:
	sample_kernfs_remove_subtree(root_dir);
err_free_kfc:
	kfree(kfc);
	return err;
}

static void sample_kernfs_kill_sb(struct super_block *sb)
{
	struct kernfs_root *root = kernfs_root_from_sb(sb);
	struct kernfs_node *root_kn = kernfs_root_to_node(root);
	struct sample_kernfs_directory *root_dir = root_kn->priv;

	kernfs_kill_sb(sb);
	kernfs_destroy_root(root);
	sample_kernfs_remove_subtree(root_dir);
}

static struct file_system_type sample_kernfs_fs_type = {
	.name			= "sample_kernfs",
	.init_fs_context	= sample_kernfs_init_fs_context,
	.kill_sb		= sample_kernfs_kill_sb,
	.fs_flags		= FS_USERNS_MOUNT,
};

static int __init sample_kernfs_init(void)
{
	int err;

	err = register_filesystem(&sample_kernfs_fs_type);
	if (err)
		return err;

	return 0;
}

module_init(sample_kernfs_init)
MODULE_DESCRIPTION("Sample kernel module showing how to use kernfs");
MODULE_LICENSE("GPL");
