// SPDX-License-Identifier: GPL-2.0

#include <linux/list.h>

bool rust_helper_list_empty(const struct list_head *head)
{
	return list_empty(head);
}

void rust_helper_list_del(struct list_head *entry)
{
	list_del(entry);
}

void rust_helper_INIT_LIST_HEAD(struct list_head *list)
{
	INIT_LIST_HEAD(list);
}

void rust_helper_list_add(struct list_head *new, struct list_head *head)
{
	list_add(new, head);
}

void rust_helper_list_add_tail(struct list_head *new, struct list_head *head)
{
	list_add_tail(new, head);
}
