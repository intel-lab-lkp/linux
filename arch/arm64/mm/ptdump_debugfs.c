// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/memory_hotplug.h>
#include <linux/seq_file.h>

#include <asm/ptdump.h>

static int ptdump_show(struct seq_file *m, void *v)
{
	struct ptdump_info_file_priv *f_priv = m->private;
	struct ptdump_info *info = &f_priv->info;

	get_online_mems();
	if (info->ptdump_walk)
		info->ptdump_walk(m, info);
	put_online_mems();
	return 0;
}

static int ptdump_open(struct inode *inode, struct file *file)
{
	int ret;
	struct ptdump_info *info = inode->i_private;
	struct ptdump_info_file_priv *f_priv;

	f_priv = kzalloc(sizeof(struct ptdump_info_file_priv), GFP_KERNEL);
	if (!f_priv)
		return -ENOMEM;

	memcpy(&f_priv->info, info, sizeof(*info));

	ret = single_open(file, ptdump_show, f_priv);
	if (ret) {
		kfree(f_priv);
		return ret;
	}

	if (info->ptdump_prepare_walk) {
		ret = info->ptdump_prepare_walk(f_priv);
		if (ret)
			kfree(f_priv);
	}

	return ret;
}

static int ptdump_release(struct inode *inode, struct file *file)
{
	struct seq_file *f = file->private_data;
	struct ptdump_info_file_priv *f_priv = f->private;
	struct ptdump_info *info = &f_priv->info;

	if (info->ptdump_end_walk)
		info->ptdump_end_walk(f_priv);

	kfree(f_priv);

	return single_release(inode, file);
}

static const struct file_operations ptdump_fops = {
	.owner		= THIS_MODULE,
	.open		= ptdump_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= ptdump_release,
};

void __init ptdump_debugfs_register(struct ptdump_info *info, const char *name)
{
	ptdump_debugfs_kvm_register(info, name, NULL);
}

void ptdump_debugfs_kvm_register(struct ptdump_info *info, const char *name,
				 struct dentry *d_entry)
{
	debugfs_create_file(name, 0400, d_entry, info, &ptdump_fops);
}
