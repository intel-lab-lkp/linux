// SPDX-License-Identifier: GPL-2.0-only
/*
 * list.c - Kernel list sample code
 *
 * Copyright 2024 David Heidelberg <david@ixit.cz>
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>

#define PROC_NAME "list_sample"
#define BUFFER_SIZE 128

LIST_HEAD(data_list);

struct data_node {
	struct list_head list;
	char data[BUFFER_SIZE];
};

static struct proc_dir_entry *proc_entry;

static ssize_t proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
{
	struct data_node *new_node;

	if (count >= BUFFER_SIZE)
		count = BUFFER_SIZE - 1;

	new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
	if (!new_node)
		return -ENOMEM;

	if (strncpy_from_user(new_node->data, buffer, count) < 0) {
		kfree(new_node);
		return -EFAULT;
	}

	list_add_tail(&new_node->list, &data_list);

	return count;
}

/*
 * Several copy_to_user() is a bit less efficient
 * comparing to list_for_each_entry_safe, but makes code simpler.
 */
static ssize_t proc_read(struct file *file, char __user *buffer, size_t count, loff_t *pos)
{
	static struct list_head *start, *cur;
	struct data_node *node;
	size_t data_len, batch_len = 0;
	char temp_buf[2 * BUFFER_SIZE] = {0};

	if (*pos == 0) {
		start = &data_list;
		cur = &data_list;
	}

	if (*pos != 0 && cur->next == start)
		return 0;

	if (list_empty(cur))
		return 0;

	while (batch_len < BUFFER_SIZE) {
		node = list_entry(cur->next, struct data_node, list);
		data_len = snprintf(temp_buf + strlen(temp_buf),
				     BUFFER_SIZE - strlen(temp_buf),
				     "%s",
				     node->data);
		if (data_len + batch_len > BUFFER_SIZE)
			break;

		batch_len += data_len;

		cur = cur->next;
		if (cur->next == start)
			break;
	}

	if (copy_to_user(buffer, temp_buf, batch_len))
		return -EFAULT;

	*pos += batch_len;

	return batch_len;
}

static const struct proc_ops proc_fops = {
	.proc_read = proc_read,
	.proc_write = proc_write,
};

static int __init list_sample_init(void)
{
	proc_entry = proc_create(PROC_NAME, 0666, NULL, &proc_fops);
	if (!proc_entry) {
		pr_err("Failed to create proc entry\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit list_sample_exit(void)
{
	struct data_node *node, *tmp;

	proc_remove(proc_entry);

	list_for_each_entry_safe(node, tmp, &data_list, list) {
		list_del(&node->list);
		kfree(node);
	}
}

module_init(list_sample_init);
module_exit(list_sample_exit);

MODULE_AUTHOR("David Heidelberg");
MODULE_DESCRIPTION("List sample module.");
MODULE_LICENSE("GPL v2");
