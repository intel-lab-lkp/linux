// SPDX-License-Identifier: GPL-2.0

/*
 * Helpers for C Circular doubly linked list implementation.
 */

#include <linux/list.h>

void rust_helper_list_add_tail(struct list_head *new, struct list_head *head)
{
	list_add_tail(new, head);
}
