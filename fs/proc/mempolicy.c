// SPDX-License-Identifier: GPL-2.0
#ifdef CONFIG_NUMA
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/mempolicy.h>
#include <linux/uaccess.h>
#include <linux/nodemask.h>

#include "internal.h"

#define MPOL_STR_SIZE 4096
static ssize_t mempolicy_read_proc(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct task_struct *task;
	struct mempolicy *policy;
	char *buffer;
	ssize_t rv = 0;
	size_t outlen;

	buffer = kzalloc(MPOL_STR_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	task = get_proc_task(file_inode(file));
	if (!task) {
		rv = -ESRCH;
		goto freebuf;
	}

	task_lock(task);
	policy = get_task_policy(task);
	mpol_get(policy);
	task_unlock(task);

	if (!policy)
		goto out;

	mpol_to_str(buffer, MPOL_STR_SIZE, policy);

	buffer[MPOL_STR_SIZE-1] = '\0';
	outlen = strlen(buffer);
	if (outlen < MPOL_STR_SIZE - 1) {
		buffer[outlen] = '\n';
		buffer[outlen + 1] = '\0';
		outlen++;
	}
	rv = simple_read_from_buffer(buf, count, ppos, buffer, outlen);
	mpol_put(policy);
out:
	put_task_struct(task);
freebuf:
	kfree(buffer);
	return rv;
}

static ssize_t mempolicy_write_proc(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct task_struct *task;
	struct mempolicy *new_policy = NULL;
	char *mempolicy_str, *nl;
	nodemask_t nodes;
	int err;

	mempolicy_str = kmalloc(count + 1, GFP_KERNEL);
	if (!mempolicy_str)
		return -ENOMEM;

	if (copy_from_user(mempolicy_str, buf, count)) {
		kfree(mempolicy_str);
		return -EFAULT;
	}
	mempolicy_str[count] = '\0';

	/* strip new line characters for simplicity of handling by parser */
	nl = strchr(mempolicy_str, '\n');
	if (nl)
		*nl = '\0';
	nl = strchr(mempolicy_str, '\r');
	if (nl)
		*nl = '\0';

	err = mpol_parse_str(mempolicy_str, &new_policy);
	if (err) {
		kfree(mempolicy_str);
		return err;
	}

	/* If no error and no policy, it was 'default', clear node list */
	if (new_policy)
		nodes = new_policy->nodes;
	else
		nodes_clear(nodes);

	task = get_proc_task(file_inode(file));
	if (!task) {
		mpol_put(new_policy);
		kfree(mempolicy_str);
		return -ESRCH;
	}

	err = replace_mempolicy(task, new_policy, &nodes);

	put_task_struct(task);
	kfree(mempolicy_str);

	return err ? err : count;
}

const struct file_operations proc_mempolicy_operations = {
	.read = mempolicy_read_proc,
	.write = mempolicy_write_proc,
	.llseek = noop_llseek,
};
#endif /* CONFIG_NUMA */
