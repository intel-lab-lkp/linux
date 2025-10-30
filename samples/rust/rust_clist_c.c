// SPDX-License-Identifier: GPL-2.0

#include <linux/list.h>
#include <linux/slab.h>

/* Sample item with embedded C list_head */
struct sample_item_c {
	int value;
	struct list_head link;
};

/* Create a list_head and populate it with items */
struct list_head *clist_sample_create(int count)
{
	struct list_head *head;
	int i;

	head = kmalloc(sizeof(*head), GFP_KERNEL);
	if (!head)
		return NULL;

	INIT_LIST_HEAD(head);

	/* Populate with items */
	for (i = 0; i < count; i++) {
		struct sample_item_c *item = kmalloc(sizeof(*item), GFP_KERNEL);

		if (!item)
			continue;

		item->value = i * 10;
		INIT_LIST_HEAD(&item->link);
		list_add_tail(&item->link, head);
	}

	return head;
}

/* Free the list_head and all items */
void clist_sample_free(struct list_head *head)
{
	struct sample_item_c *item, *tmp;

	if (!head)
		return;

	/* Free all items in the list */
	list_for_each_entry_safe(item, tmp, head, link) {
		list_del(&item->link);
		kfree(item);
	}

	kfree(head);
}
