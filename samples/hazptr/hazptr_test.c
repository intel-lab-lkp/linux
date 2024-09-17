#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/hazptr.h>

struct foo {
	int i;
	struct callback_head head;
};

static void simple_func(struct callback_head *head)
{
	struct foo *ptr = container_of(head, struct foo, head);

	printk("callback called %px, i is %d\n", ptr, ptr->i);
	kfree(ptr);
}

static void simple(void)
{
	struct hazptr_context ctx;
	struct foo *dummy, *tmp, *other;
	hazptr_t *hptr;
	hazptr_t *hptr2;

	dummy = kzalloc(sizeof(*dummy), GFP_KERNEL);
	dummy->i = 42;

	other = kzalloc(sizeof(*dummy), GFP_KERNEL);
	other->i = 43;

	if (!dummy || !other) {
		printk("allocation failed, skip test\n");
		return;
	}

	init_hazptr_context(&ctx);
	hptr = hazptr_alloc(&ctx);
	BUG_ON(!hptr);

	// Get a second hptr.
	hptr2 = hazptr_alloc(&ctx);
	BUG_ON(!hptr2);

	// No one is modifying 'dummy', protection must succeed.
	BUG_ON(!hazptr_tryprotect(hptr, dummy, head));

	// Simulate changing a global pointer.
	tmp = dummy;
	WRITE_ONCE(dummy, other);

	// Callback will run after no active readers.
	printk("callback added, %px\n", tmp);

	call_hazptr(&tmp->head, simple_func);

	// No one is modifying 'dummy', protection must succeed.
	tmp = hazptr_protect(hptr2, dummy, head);

	printk("reader2 got %px, i is %d\n", tmp, tmp->i);

	// The above callback should run after this.
	hazptr_clear(hptr);
	printk("first reader is out\n");

	for (int i = 0; i < 10; i++)
		schedule(); // yield a few times.

	// Simulate freeing a global pointer.
	tmp = dummy;
	WRITE_ONCE(dummy, NULL);
	printk("callback added, %px\n", tmp);
	call_hazptr(&tmp->head, simple_func);

	cleanup_hazptr_context(&ctx);
	printk("no reader here\n");

	for (int i = 0; i < 10; i++)
		schedule(); // yield a few times.
}

static int hazptr_test(void)
{
	simple();
	printk("test ends\n");
	return 0;
}
module_init(hazptr_test);
