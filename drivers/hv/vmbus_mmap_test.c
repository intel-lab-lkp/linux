// SPDX-License-Identifier: GPL-2.0
/* Included beside the production sysfs wrapper; no VMBus device is touched. */
#include <kunit/test.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

struct vmbus_mmap_test_context {
	struct vmbus_channel channel;
	struct page *pages[3];
	struct page *blocker;
};

static void vmbus_mmap_test_put_page(void *page)
{
	__free_page(page);
}

static struct page *vmbus_mmap_test_page(struct kunit *test, u8 value)
{
	struct page *page = alloc_page(GFP_KERNEL);

	if (!page)
		return NULL;
	if (kunit_add_action_or_reset(test, vmbus_mmap_test_put_page, page))
		return NULL;
	memset(page_address(page), value, PAGE_SIZE);
	return page;
}

static int vmbus_mmap_test_prepare(struct vmbus_channel *channel,
				   struct vm_area_desc *desc)
{
	struct vmbus_buffer_pin *pin;
	int ret;

	pin = kzalloc_obj(*pin);
	if (!pin)
		return -ENOMEM;
	ret = vmbus_buffer_pin_pages(&channel->ringbuffer, pin);
	if (ret) {
		kfree(pin);
		return ret;
	}
	mmap_action_map_kernel_pages(desc, desc->start, pin->pages,
				     pin->page_count);
	desc->vm_ops = NULL;
	desc->private_data = pin;
	return 0;
}

static struct vmbus_mmap_test_context *
vmbus_mmap_test_context(struct kunit *test)
{
	struct vmbus_mmap_test_context *ctx;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(ctx->pages); i++) {
		ctx->pages[i] = vmbus_mmap_test_page(test, 0x61 + i);
		if (!ctx->pages[i])
			return NULL;
	}
	ctx->blocker = vmbus_mmap_test_page(test, 0xcc);
	if (!ctx->blocker)
		return NULL;
	ctx->channel.mmap_prepare_ring_buffer = vmbus_mmap_test_prepare;
	ctx->channel.ringbuffer.pages = ctx->pages;
	ctx->channel.ringbuffer.page_cnt = ARRAY_SIZE(ctx->pages);
	return ctx;
}

static void vmbus_mmap_test_action(struct kunit *test, bool partial)
{
	struct vmbus_mmap_test_context *ctx = vmbus_mmap_test_context(test);
	struct vm_area_struct *vma;
	unsigned long start;
	unsigned int i;
	u8 value = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	start = kunit_vm_mmap(test, NULL, 0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_ANONYMOUS, 0);
	KUNIT_ASSERT_NE(test, start, 0UL);
	KUNIT_ASSERT_LT(test, start, (unsigned long)TASK_SIZE);

	mmap_write_lock(current->mm);
	vma = find_vma(current->mm, start);
	if (!vma) {
		mmap_write_unlock(current->mm);
		KUNIT_FAIL(test, "managed VMA missing");
		return;
	}
	if (partial) {
		ret = vm_insert_page(vma, start + PAGE_SIZE, ctx->blocker);
		if (ret) {
			mmap_write_unlock(current->mm);
			KUNIT_FAIL(test, "owned blocking PTE could not be installed");
			return;
		}
	}
	ret = hv_mmap_ring_buffer_wrapper(vma->vm_file, &ctx->channel.kobj,
					  NULL, vma);
	KUNIT_EXPECT_EQ(test, ret, partial ? -EBUSY : 0);
	KUNIT_EXPECT_NULL(test, vma->vm_private_data);
	KUNIT_EXPECT_NULL(test, vma->vm_ops);
	for (i = 0; i < ARRAY_SIZE(ctx->pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(ctx->pages[i]),
				partial && i ? 1 : 2);
	KUNIT_EXPECT_EQ(test, page_ref_count(ctx->blocker), partial ? 2 : 1);
	mmap_write_unlock(current->mm);

	/* The first insertion really happened, including on the partial error. */
	KUNIT_EXPECT_EQ(test, copy_from_user(&value, (void __user *)start, 1), 0UL);
	KUNIT_EXPECT_EQ(test, value, (u8)0x61);
	KUNIT_ASSERT_EQ(test, vm_munmap(start, 3 * PAGE_SIZE), 0);
	for (i = 0; i < ARRAY_SIZE(ctx->pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(ctx->pages[i]), 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(ctx->blocker), 1);
}

static void vmbus_sysfs_mmap_success_test(struct kunit *test)
{
	vmbus_mmap_test_action(test, false);
}

static void vmbus_sysfs_mmap_partial_insert_test(struct kunit *test)
{
	vmbus_mmap_test_action(test, true);
}

static struct kunit_case vmbus_mmap_test_cases[] = {
	KUNIT_CASE(vmbus_sysfs_mmap_success_test),
	KUNIT_CASE(vmbus_sysfs_mmap_partial_insert_test),
	{}
};

static struct kunit_suite vmbus_mmap_test_suite = {
	.name = "hyperv-vmbus-mmap",
	.test_cases = vmbus_mmap_test_cases,
};

kunit_test_suite(vmbus_mmap_test_suite);
