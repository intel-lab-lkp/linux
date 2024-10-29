// SPDX-License-Identifier: GPL-2.0-only
#include <linux/fsnotify.h>
#include <linux/fanotify.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/string.h>

struct prefix_item {
	const char *prefix;
	struct list_head list;
};

struct sample_fp_data {
	/*
	 * str_table contains all the prefixes to ignore. For example,
	 * "prefix1\0prefix2\0prefix3"
	 */
	char *str_table;

	/* item->prefix points to different prefixes in the str_table. */
	struct list_head item_list;
};

static int sample_fp_handler(struct fsnotify_group *group,
			     struct fanotify_fastpath_hook *fp_hook,
			     struct fanotify_fastpath_event *fp_event)
{
	const struct qstr *file_name = fp_event->file_name;
	struct sample_fp_data *fp_data;
	struct prefix_item *item;

	if (!file_name)
		return FAN_FP_RET_SEND_TO_USERSPACE;
	fp_data = fp_hook->data;

	list_for_each_entry(item, &fp_data->item_list, list) {
		if (strstr(file_name->name, item->prefix) == (char *)file_name->name)
			return FAN_FP_RET_SKIP_EVENT;
	}

	return FAN_FP_RET_SEND_TO_USERSPACE;
}

static int add_item(struct sample_fp_data *fp_data, const char *prev)
{
	struct prefix_item *item;

	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;
	item->prefix = prev;
	list_add_tail(&item->list, &fp_data->item_list);
	return 0;
}

static void free_sample_fp_data(struct sample_fp_data *fp_data)
{
	struct prefix_item *item, *tmp;

	list_for_each_entry_safe(item, tmp, &fp_data->item_list, list) {
		list_del_init(&item->list);
		kfree(item);
	}
	kfree(fp_data->str_table);
	kfree(fp_data);
}

static int sample_fp_init(struct fanotify_fastpath_hook *fp_hook, const char *args)
{
	struct sample_fp_data *fp_data = kzalloc(sizeof(struct sample_fp_data), GFP_KERNEL);
	char *p, *prev;
	int ret;

	if (!fp_data)
		return -ENOMEM;

	/* Make a copy of the list of prefix to ignore */
	fp_data->str_table = kstrndup(args, FAN_FP_ARGS_MAX, GFP_KERNEL);
	if (!fp_data->str_table) {
		ret = -ENOMEM;
		goto err_out;
	}

	INIT_LIST_HEAD(&fp_data->item_list);
	prev = fp_data->str_table;
	p = fp_data->str_table;

	/* Update the list replace ',' with '\n'*/
	while ((p = strchr(p, ',')) != NULL) {
		*p = '\0';
		ret = add_item(fp_data, prev);
		if (ret)
			goto err_out;
		p = p + 1;
		prev = p;
	}

	ret = add_item(fp_data, prev);
	if (ret)
		goto err_out;

	fp_hook->data = fp_data;

	return 0;

err_out:
	free_sample_fp_data(fp_data);
	return ret;
}

static void sample_fp_free(struct fanotify_fastpath_hook *fp_hook)
{
	free_sample_fp_data(fp_hook->data);
}

static struct fanotify_fastpath_ops fan_fp_ignore_a_ops = {
	.fp_handler = sample_fp_handler,
	.fp_init = sample_fp_init,
	.fp_free = sample_fp_free,
	.name = "ignore-prefix",
	.owner = THIS_MODULE,
};

static int __init fanotify_fastpath_sample_init(void)
{
	return fanotify_fastpath_register(&fan_fp_ignore_a_ops);
}
static void __exit fanotify_fastpath_sample_exit(void)
{
	fanotify_fastpath_unregister(&fan_fp_ignore_a_ops);
}

module_init(fanotify_fastpath_sample_init);
module_exit(fanotify_fastpath_sample_exit);

MODULE_AUTHOR("Song Liu");
MODULE_DESCRIPTION("Example fanotify fastpath handler");
MODULE_LICENSE("GPL");
