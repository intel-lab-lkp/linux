// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the UIO Hyper-V generic mmap paths.
 *
 * This file is included from uio_hv_generic.c so the cases can reach the
 * static helpers that select, validate and pin the mapped pages.
 *
 * Two ownership contracts are under test:
 *   - the /dev/uioN character-device path installs hv_uio_pin_vm_ops
 *     (.mapped + .close) and a struct hv_uio_pin;
 *   - the sysfs "ring" bin attribute cannot, because kernfs_fop_mmap()
 *     rejects a vm_operations_struct that carries .close. That path
 *     installs no vm_ops and hands a bare struct vmbus_buffer_pin to its
 *     synchronous .mmap wrapper, which is the single release site.
 */
#include <kunit/test.h>

/*
 * Release exactly what hv_mmap_ring_buffer_wrapper() releases after
 * __compat_vma_mmap() returns: the bare pin's references, then the pin.
 */
static void sysfs_pin_release(struct vmbus_buffer_pin *pin)
{
	vmbus_buffer_unpin_pages(pin);
	kfree(pin);
}

static void hv_uio_ring_mmap_range_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, hv_uio_mmap_range_valid(8, 0, 8));
	KUNIT_EXPECT_TRUE(test, hv_uio_mmap_range_valid(8, 4, 4));
	KUNIT_EXPECT_FALSE(test, hv_uio_mmap_range_valid(8, 8, 1));
	KUNIT_EXPECT_FALSE(test, hv_uio_mmap_range_valid(8, 7, 2));
	KUNIT_EXPECT_FALSE(test, hv_uio_mmap_range_valid(8, 0, 0));
	KUNIT_EXPECT_FALSE(test,
			   hv_uio_mmap_range_valid(8, U64_MAX, 1));
}

static void hv_uio_ring_mmap_prepare_test(struct kunit *test)
{
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
		.ringbuffer = {
			.page_cnt = 8,
		},
	};
	struct page *pages[8] = {};
	struct page *chunks[1] = {};
	struct vmbus_buffer_pin *pin;
	struct vm_area_desc desc = {
		.start = PAGE_SIZE,
		.end = 5 * PAGE_SIZE,
		.pgoff = 2,
		.page_prot = PAGE_SHARED,
	};
	unsigned int i;
	int ret;

	/* Mapping pins real pages; the array cannot hold NULL entries. */
	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}

	channel.ringbuffer.pages = pages;
	channel.ringbuffer.chunks = chunks;
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	ret = hv_uio_ring_mmap_prepare(&channel, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, desc.action.type, MMAP_MAP_KERNEL_PAGES);
	KUNIT_EXPECT_PTR_EQ(test, desc.action.map_kernel.pages, &pages[2]);
	KUNIT_EXPECT_EQ(test, desc.action.map_kernel.nr_pages, 4UL);
	KUNIT_EXPECT_TRUE(test, vma_desc_test(&desc, VMA_DONTEXPAND_BIT));
	KUNIT_EXPECT_TRUE(test, vma_desc_test(&desc, VMA_DONTDUMP_BIT));
	KUNIT_EXPECT_EQ(test, pgprot_val(desc.page_prot),
			pgprot_val(pgprot_decrypted(PAGE_SHARED)));

	/*
	 * The sysfs surface installs no vm_ops at all: see the kernfs
	 * contract in the comment at the top of this file. The wrapper
	 * holds the pin and is the only thing that releases it.
	 */
	KUNIT_EXPECT_NULL(test, desc.vm_ops);
	pin = desc.private_data;
	KUNIT_ASSERT_NOT_NULL(test, pin);
	KUNIT_EXPECT_TRUE(test, pin->decrypted);
	KUNIT_EXPECT_EQ(test, pin->page_count, ARRAY_SIZE(pages));
	sysfs_pin_release(pin);

	desc.vma_flags = EMPTY_VMA_FLAGS;
	KUNIT_EXPECT_EQ(test, hv_uio_ring_mmap_prepare(&channel, &desc), -EINVAL);
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);

	desc.start = PAGE_SIZE;
	desc.end = 3 * PAGE_SIZE;
	desc.pgoff = 7;
	KUNIT_EXPECT_EQ(test, hv_uio_ring_mmap_prepare(&channel, &desc), -EINVAL);

	channel.state = CHANNEL_OPEN_STATE;
	KUNIT_EXPECT_EQ(test, hv_uio_ring_mmap_prepare(&channel, &desc), -ENODEV);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

static void hv_uio_mmap_region_select_test(struct kunit *test)
{
	struct hv_uio_private_data pdata = {};
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
	};
	struct hv_device dev = {
		.channel = &channel,
	};
	struct hv_uio_mmap_region region;
	struct page *ring_pages[4] = {};
	struct page *recv_pages[3] = {};
	struct page *send_pages[2] = {};
	struct page *chunks[1] = {};
	int ret;

	pdata.device = &dev;
	pdata.info.mem[TXRX_RING_MAP].size =
		ARRAY_SIZE(ring_pages) * PAGE_SIZE;
	pdata.info.mem[INT_PAGE_MAP].size = HV_HYP_PAGE_SIZE;
	pdata.info.mem[MON_PAGE_MAP].size = HV_HYP_PAGE_SIZE;
	pdata.info.mem[RECV_BUF_MAP].size =
		ARRAY_SIZE(recv_pages) * PAGE_SIZE;
	pdata.info.mem[SEND_BUF_MAP].size =
		ARRAY_SIZE(send_pages) * PAGE_SIZE;
	channel.ringbuffer.pages = ring_pages;
	channel.ringbuffer.page_cnt = ARRAY_SIZE(ring_pages);
	channel.ringbuffer.chunks = chunks;
	pdata.int_pages[0] = ring_pages[0];
	pdata.monitor_pages[0] = ring_pages[1];
	pdata.recv_buffer.pages = recv_pages;
	pdata.recv_buffer.page_cnt = ARRAY_SIZE(recv_pages);
	pdata.recv_buffer.chunks = chunks;
	pdata.send_buffer.pages = send_pages;
	pdata.send_buffer.page_cnt = ARRAY_SIZE(send_pages);

	/*
	 * Buffer-backed maps are described only by their owning buffer.
	 * The page array and the protection state are deliberately absent
	 * here: reading them outside vmbus_buffer_owners_lock can describe
	 * a buffer a concurrent release has already cleared. The locked
	 * pin snapshot is the only safe source.
	 */
	ret = hv_uio_mmap_get_region(&pdata, TXRX_RING_MAP, &region);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, region.buffer, &channel.ringbuffer);
	KUNIT_EXPECT_NULL(test, region.pages);
	KUNIT_EXPECT_EQ(test, region.page_count, 0UL);
	KUNIT_EXPECT_FALSE(test, region.decrypted);

	ret = hv_uio_mmap_get_region(&pdata, INT_PAGE_MAP, &region);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_NULL(test, region.buffer);
	KUNIT_EXPECT_PTR_EQ(test, region.pages, &pdata.int_pages[0]);
	KUNIT_EXPECT_FALSE(test, region.decrypted);

	ret = hv_uio_mmap_get_region(&pdata, MON_PAGE_MAP, &region);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_NULL(test, region.buffer);
	KUNIT_EXPECT_PTR_EQ(test, region.pages, &pdata.monitor_pages[0]);
	KUNIT_EXPECT_TRUE(test, region.decrypted);

	ret = hv_uio_mmap_get_region(&pdata, RECV_BUF_MAP, &region);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, region.buffer, &pdata.recv_buffer);
	KUNIT_EXPECT_NULL(test, region.pages);
	KUNIT_EXPECT_EQ(test, region.page_count, 0UL);
	KUNIT_EXPECT_FALSE(test, region.decrypted);

	ret = hv_uio_mmap_get_region(&pdata, SEND_BUF_MAP, &region);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, region.buffer, &pdata.send_buffer);
	KUNIT_EXPECT_NULL(test, region.pages);
	KUNIT_EXPECT_EQ(test, region.page_count, 0UL);
	KUNIT_EXPECT_FALSE(test, region.decrypted);

	KUNIT_EXPECT_EQ(test,
			hv_uio_mmap_get_region(&pdata, MAX_UIO_MAPS, &region),
			-EINVAL);
	channel.state = CHANNEL_OPEN_STATE;
	KUNIT_EXPECT_EQ(test,
			hv_uio_mmap_get_region(&pdata, TXRX_RING_MAP, &region),
			-ENODEV);
}

static void hv_uio_mmap_prepare_test(struct kunit *test)
{
	struct hv_uio_private_data *pdata;
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
	};
	struct hv_device dev = {
		.channel = &channel,
	};
	struct page *pages[4] = {};
	struct page *chunks[1] = {};
	struct vm_area_desc desc = {
		.start = PAGE_SIZE,
		.end = 3 * PAGE_SIZE,
		.pgoff = RECV_BUF_MAP,
		.page_prot = PAGE_SHARED,
	};
	unsigned int i;
	int ret;

	/* Mapping pins real pages; the array cannot hold NULL entries. */
	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}

	pdata = kunit_kzalloc(test, sizeof(*pdata), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdata);
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	pdata->device = &dev;
	pdata->info.priv = pdata;
	pdata->info.mem[RECV_BUF_MAP].size = ARRAY_SIZE(pages) * PAGE_SIZE;
	pdata->recv_buffer.pages = pages;
	pdata->recv_buffer.page_cnt = ARRAY_SIZE(pages);
	pdata->recv_buffer.chunks = chunks;

	ret = hv_uio_mmap_prepare(&pdata->info, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, desc.action.type, MMAP_MAP_KERNEL_PAGES);
	KUNIT_EXPECT_PTR_EQ(test, desc.action.map_kernel.pages, &pages[0]);
	KUNIT_EXPECT_EQ(test, desc.action.map_kernel.nr_pages, 2UL);
	/*
	 * The protection state comes from the locked pin snapshot, not
	 * from a second read of the buffer descriptor. chunks is set, so
	 * the snapshot says decrypted.
	 */
	KUNIT_EXPECT_EQ(test, pgprot_val(desc.page_prot),
			pgprot_val(pgprot_decrypted(PAGE_SHARED)));
	KUNIT_EXPECT_TRUE(test, vma_desc_test(&desc, VMA_DONTEXPAND_BIT));
	KUNIT_EXPECT_TRUE(test, vma_desc_test(&desc, VMA_DONTDUMP_BIT));
	/*
	 * Character-device maps can carry an abandon path: kernfs is not
	 * in this path, so .close is legal here and illegal on "ring".
	 */
	KUNIT_EXPECT_PTR_EQ(test, desc.vm_ops, &hv_uio_pin_vm_ops);
	KUNIT_ASSERT_NOT_NULL(test, desc.vm_ops->mapped);
	KUNIT_ASSERT_NOT_NULL(test, desc.vm_ops->close);
	/* Buffer-backed maps leave the pin held for mapped()/close(). */
	KUNIT_ASSERT_NOT_NULL(test, desc.private_data);
	hv_uio_pin_release(desc.private_data);
	desc.private_data = NULL;

	desc.vma_flags = EMPTY_VMA_FLAGS;
	KUNIT_EXPECT_EQ(test, hv_uio_mmap_prepare(&pdata->info, &desc), -EINVAL);

	desc.pgoff = INT_PAGE_MAP;
	pdata->info.mem[INT_PAGE_MAP].size = HV_HYP_PAGE_SIZE;
	pdata->int_pages[0] = pages[0];
	desc.end = 2 * PAGE_SIZE;
	desc.page_prot = PAGE_SHARED;
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	ret = hv_uio_mmap_prepare(&pdata->info, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, desc.action.map_kernel.pages,
			    &pdata->int_pages[0]);
	KUNIT_EXPECT_EQ(test, pgprot_val(desc.page_prot),
			pgprot_val(PAGE_SHARED));
	/* int_pages is not a vmbus_buffer, so nothing is pinned. */
	KUNIT_EXPECT_NULL(test, desc.private_data);

	desc.pgoff = MON_PAGE_MAP;
	pdata->info.mem[MON_PAGE_MAP].size = HV_HYP_PAGE_SIZE;
	pdata->monitor_pages[0] = pages[1];
	desc.page_prot = PAGE_SHARED;
	ret = hv_uio_mmap_prepare(&pdata->info, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, desc.action.map_kernel.pages,
			    &pdata->monitor_pages[0]);
	KUNIT_EXPECT_EQ(test, pgprot_val(desc.page_prot),
			pgprot_val(pgprot_decrypted(PAGE_SHARED)));
	KUNIT_EXPECT_NULL(test, desc.private_data);

	desc.pgoff = SEND_BUF_MAP;
	pdata->info.mem[SEND_BUF_MAP].size = PAGE_SIZE;
	pdata->send_buffer.pages = pages;
	pdata->send_buffer.page_cnt = ARRAY_SIZE(pages);
	desc.page_prot = PAGE_SHARED;
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	ret = hv_uio_mmap_prepare(&pdata->info, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* send_buffer has no chunks, so the snapshot says encrypted. */
	KUNIT_EXPECT_EQ(test, pgprot_val(desc.page_prot),
			pgprot_val(PAGE_SHARED));
	KUNIT_ASSERT_NOT_NULL(test, desc.private_data);
	hv_uio_pin_release(desc.private_data);
	desc.private_data = NULL;

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

/*
 * The reclaim race this driver closes is a lifetime question: the pin has
 * to span the gap between mmap_prepare() returning and insert_pages()
 * taking its own folio references. Dropping it inside the prepare hook
 * leaves the pages freeable before the mapping exists.
 *
 * This is the character-device contract: mapped() and close() share the
 * release, and release is one-shot.
 */
static void hv_uio_pin_lifetime_test(struct kunit *test)
{
	struct hv_uio_private_data *pdata;
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
	};
	struct hv_device dev = {
		.channel = &channel,
	};
	struct page *pages[4] = {};
	struct page *chunks[1] = {};
	struct vm_area_desc desc = {
		.start = 0,
		.end = 4 * PAGE_SIZE,
		.pgoff = RECV_BUF_MAP,
		.page_prot = PAGE_SHARED,
	};
	struct vm_area_struct *vma;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}

	pdata = kunit_kzalloc(test, sizeof(*pdata), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdata);
	pdata->device = &dev;
	pdata->info.priv = pdata;
	pdata->info.mem[RECV_BUF_MAP].size = ARRAY_SIZE(pages) * PAGE_SIZE;
	pdata->recv_buffer.pages = pages;
	pdata->recv_buffer.page_cnt = ARRAY_SIZE(pages);
	pdata->recv_buffer.chunks = chunks;

	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	ret = hv_uio_mmap_prepare(&pdata->info, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * Prepare must hold a reference on every page: the mapping is not
	 * installed until insert_pages() runs after the hook returns.
	 */
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 2);

	KUNIT_EXPECT_PTR_EQ(test, desc.vm_ops, &hv_uio_pin_vm_ops);
	KUNIT_ASSERT_NOT_NULL(test, desc.private_data);

	/* mapped(): the mapping holds its own refs, so the pin is released. */
	ret = desc.vm_ops->mapped(desc.start, desc.end, 0, NULL,
				  &desc.private_data);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_NULL(test, desc.private_data);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	/* close() after mapped() must not unpin again. */
	vma = kunit_kzalloc(test, sizeof(*vma), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vma);
	vma->vm_ops = desc.vm_ops;
	vma->vm_private_data = desc.private_data;
	desc.vm_ops->close(vma);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

/*
 * The abandonment path: when the mapping is torn down before mapped() ran
 * (insert_pages() failure, or a merge that never calls mapped()), close()
 * must still drop the pin. A foreign private_data must be left alone.
 */
static void hv_uio_pin_close_before_mapped_test(struct kunit *test)
{
	struct hv_uio_private_data *pdata;
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
	};
	struct hv_device dev = {
		.channel = &channel,
	};
	struct page *pages[2] = {};
	struct page *chunks[1] = {};
	struct vm_area_desc desc = {
		.start = 0,
		.end = 2 * PAGE_SIZE,
		.pgoff = RECV_BUF_MAP,
		.page_prot = PAGE_SHARED,
	};
	struct vm_area_struct *vma;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}

	pdata = kunit_kzalloc(test, sizeof(*pdata), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdata);
	pdata->device = &dev;
	pdata->info.priv = pdata;
	pdata->info.mem[RECV_BUF_MAP].size = ARRAY_SIZE(pages) * PAGE_SIZE;
	pdata->recv_buffer.pages = pages;
	pdata->recv_buffer.page_cnt = ARRAY_SIZE(pages);
	pdata->recv_buffer.chunks = chunks;

	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	ret = hv_uio_mmap_prepare(&pdata->info, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, desc.private_data);

	vma = kunit_kzalloc(test, sizeof(*vma), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vma);
	vma->vm_ops = desc.vm_ops;
	vma->vm_private_data = desc.private_data;
	desc.vm_ops->close(vma);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	/* A second close is a no-op, not a double unpin. */
	desc.vm_ops->close(vma);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	/* A pointer this driver did not install must not be freed. */
	vma->vm_private_data = &pages[0];
	desc.vm_ops->close(vma);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

/*
 * The kernfs contract, named. fs/kernfs/file.c returns -EINVAL for a VMA
 * whose vm_ops carry .close, and it performs that check after the .mmap
 * hook has already succeeded. A successful sysfs mapping therefore cannot
 * carry an abandon path in vm_ops at all.
 */
static void sysfs_ring_mmap_ops_have_no_close(struct kunit *test)
{
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
		.ringbuffer = {
			.page_cnt = 1,
		},
	};
	struct page *pages[1] = {};
	struct page *chunks[1] = {};
	struct vmbus_buffer_pin *pin;
	struct vm_area_desc desc = {
		.start = 0,
		.end = PAGE_SIZE,
		.pgoff = 0,
		.page_prot = PAGE_SHARED,
	};
	int ret;

	pages[0] = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	channel.ringbuffer.pages = pages;
	channel.ringbuffer.chunks = chunks;
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);

	ret = hv_uio_ring_mmap_prepare(&channel, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* The exact predicate kernfs_fop_mmap() applies must be false. */
	KUNIT_EXPECT_NULL(test, desc.vm_ops);
	KUNIT_EXPECT_FALSE(test, desc.vm_ops && desc.vm_ops->close);

	pin = desc.private_data;
	KUNIT_ASSERT_NOT_NULL(test, pin);
	sysfs_pin_release(pin);

	__free_page(pages[0]);
}

/*
 * The character-device counterpart: a buffer-backed map prepares and
 * installs the ops that carry the abandon path, then mapped() releases.
 */
static void uio_buffer_mmap_success(struct kunit *test)
{
	struct hv_uio_private_data *pdata;
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
	};
	struct hv_device dev = {
		.channel = &channel,
	};
	struct page *pages[2] = {};
	struct page *chunks[1] = {};
	struct vm_area_desc desc = {
		.start = 0,
		.end = 2 * PAGE_SIZE,
		.pgoff = RECV_BUF_MAP,
		.page_prot = PAGE_SHARED,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}

	pdata = kunit_kzalloc(test, sizeof(*pdata), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdata);
	pdata->device = &dev;
	pdata->info.priv = pdata;
	pdata->info.mem[RECV_BUF_MAP].size = ARRAY_SIZE(pages) * PAGE_SIZE;
	pdata->recv_buffer.pages = pages;
	pdata->recv_buffer.page_cnt = ARRAY_SIZE(pages);
	pdata->recv_buffer.chunks = chunks;

	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	ret = hv_uio_mmap_prepare(&pdata->info, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, desc.action.type, MMAP_MAP_KERNEL_PAGES);
	KUNIT_EXPECT_PTR_EQ(test, desc.vm_ops, &hv_uio_pin_vm_ops);
	KUNIT_ASSERT_NOT_NULL(test, desc.private_data);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 2);

	ret = desc.vm_ops->mapped(desc.start, desc.end, 0, NULL,
				  &desc.private_data);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_NULL(test, desc.private_data);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

/*
 * A prepare that refuses must not leave references behind. Each failure
 * mode is checked against the page reference count, which is the only
 * externally visible record of a leaked pin.
 */
static void sysfs_mmap_prepare_failure_releases_pin(struct kunit *test)
{
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
		.ringbuffer = {
			.page_cnt = 2,
		},
	};
	struct page *pages[2] = {};
	struct page *chunks[1] = {};
	struct vm_area_desc desc = {
		.start = 0,
		.end = 2 * PAGE_SIZE,
		.pgoff = 0,
		.page_prot = PAGE_SHARED,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}
	channel.ringbuffer.pages = pages;
	channel.ringbuffer.chunks = chunks;

	/* Not shared: refused before anything is pinned. */
	KUNIT_EXPECT_EQ(test, hv_uio_ring_mmap_prepare(&channel, &desc), -EINVAL);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	/* pgoff past the end of the pin: refused before anything is pinned. */
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);
	desc.pgoff = 2;
	KUNIT_EXPECT_EQ(test, hv_uio_ring_mmap_prepare(&channel, &desc), -EINVAL);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	/* Channel not open: refused before anything is pinned. */
	desc.pgoff = 0;
	channel.state = CHANNEL_OPEN_STATE;
	KUNIT_EXPECT_EQ(test, hv_uio_ring_mmap_prepare(&channel, &desc), -ENODEV);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

/*
 * Prepare succeeded and the pin is installed, but the mapping never
 * becomes established (insert_pages() fails part-way, or the attempt is
 * otherwise abandoned). The wrapper is the only owner on this surface and
 * releases with one unpin plus one free, whether or not any VMA ever held
 * the pages. This proves the object it is handed is fully releasable that
 * way, and that releasing twice is not possible to observe.
 */
static void sysfs_mmap_partial_insert_failure_releases_pin(struct kunit *test)
{
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
		.ringbuffer = {
			.page_cnt = 3,
		},
	};
	struct page *pages[3] = {};
	struct page *chunks[1] = {};
	struct vmbus_buffer_pin *pin;
	struct vm_area_desc desc = {
		.start = 0,
		.end = 3 * PAGE_SIZE,
		.pgoff = 0,
		.page_prot = PAGE_SHARED,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}
	channel.ringbuffer.pages = pages;
	channel.ringbuffer.chunks = chunks;
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);

	ret = hv_uio_ring_mmap_prepare(&channel, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	pin = desc.private_data;
	KUNIT_ASSERT_NOT_NULL(test, pin);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 2);

	/* The wrapper's abandon path: one unpin, one free. */
	sysfs_pin_release(pin);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);

	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
}

/*
 * The window the pin exists to close. Between the snapshot and the point
 * where the mapping holds its own references, a concurrent
 * vmbus_release_buffer() can drop the buffer's reference to each page.
 * The pin must keep the pages alive for the whole window, and must be the
 * thing that drops the last reference.
 */
static void sysfs_mmap_release_during_insert_keeps_pages_alive(struct kunit *test)
{
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
		.ringbuffer = {
			.page_cnt = 2,
		},
	};
	struct page *pages[2] = {};
	struct page *chunks[1] = {};
	struct vmbus_buffer_pin *pin;
	struct vm_area_desc desc = {
		.start = 0,
		.end = 2 * PAGE_SIZE,
		.pgoff = 0,
		.page_prot = PAGE_SHARED,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
	}
	channel.ringbuffer.pages = pages;
	channel.ringbuffer.chunks = chunks;
	vma_desc_set_flags(&desc, VMA_SHARED_BIT);

	ret = hv_uio_ring_mmap_prepare(&channel, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
	pin = desc.private_data;
	KUNIT_ASSERT_NOT_NULL(test, pin);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 2);

	/*
	 * Concurrent release: the buffer drops its own reference to each
	 * page. The pages must still be alive, held only by the pin, so a
	 * mapping that is mid-insert cannot walk freed memory.
	 */
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		__free_page(pages[i]);
	for (i = 0; i < ARRAY_SIZE(pages); i++)
		KUNIT_EXPECT_EQ(test, page_ref_count(pages[i]), 1);
	KUNIT_EXPECT_PTR_EQ(test, pin->pages, &pages[0]);
	KUNIT_EXPECT_EQ(test, pin->page_count, ARRAY_SIZE(pages));

	/* The pin is what drops the last reference. */
	sysfs_pin_release(pin);
}

/*
 * KCA-23: the protection state has to travel with the pin. The snapshot is
 * taken in the same critical section as the page array, so a later
 * vmbus_release_buffer() clearing the descriptor cannot change what this
 * mapping is told about the pages it holds.
 */
static void ring_mmap_release_preserves_protection_snapshot(struct kunit *test)
{
	struct vmbus_channel channel = {
		.state = CHANNEL_OPENED_STATE,
		.ringbuffer = {
			.page_cnt = 1,
		},
	};
	struct page *pages[1] = {};
	struct page *chunks[1] = {};
	struct vmbus_buffer_pin pin = {};
	int ret;

	pages[0] = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pages[0]);
	channel.ringbuffer.pages = pages;
	channel.ringbuffer.chunks = chunks;

	ret = vmbus_buffer_pin_pages(&channel.ringbuffer, &pin);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, pin.decrypted);
	KUNIT_EXPECT_EQ(test, pin.page_count, 1UL);

	/*
	 * A concurrent release clears the descriptor. The pin must still
	 * describe the pages it holds, not the cleared descriptor.
	 */
	channel.ringbuffer.chunks = NULL;
	channel.ringbuffer.pages = NULL;
	channel.ringbuffer.page_cnt = 0;
	KUNIT_EXPECT_TRUE(test, pin.decrypted);
	KUNIT_EXPECT_PTR_EQ(test, pin.pages, &pages[0]);
	KUNIT_EXPECT_EQ(test, pin.page_count, 1UL);

	vmbus_buffer_unpin_pages(&pin);
	KUNIT_EXPECT_NULL(test, pin.pages);
	KUNIT_EXPECT_EQ(test, pin.page_count, 0UL);

	/* A buffer that never took the shared-page path is not decrypted. */
	channel.ringbuffer.pages = pages;
	channel.ringbuffer.page_cnt = 1;
	channel.ringbuffer.chunks = NULL;
	ret = vmbus_buffer_pin_pages(&channel.ringbuffer, &pin);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, pin.decrypted);
	vmbus_buffer_unpin_pages(&pin);

	__free_page(pages[0]);
}

static unsigned int hv_uio_disconnect_calls;
static int hv_uio_disconnect_result;

static int hv_uio_test_disconnect(struct vmbus_channel *channel)
{
	hv_uio_disconnect_calls++;
	channel->state = CHANNEL_OPEN_STATE;
	return hv_uio_disconnect_result;
}

static void hv_uio_remove_disconnects_open_channel_test(struct kunit *test)
{
	struct vmbus_channel channel = { .state = CHANNEL_OPEN_STATE };

	hv_uio_disconnect_calls = 0;
	hv_uio_disconnect_result = 0;
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_if_open(&channel,
							hv_uio_test_disconnect), 0);
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_calls, 0U);
	channel.state = CHANNEL_OPENED_STATE;
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_if_open(&channel,
							hv_uio_test_disconnect), 0);
	KUNIT_EXPECT_EQ(test, channel.state, CHANNEL_OPEN_STATE);
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_calls, 1U);
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_if_open(&channel,
							hv_uio_test_disconnect), 0);
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_calls, 1U);

	channel.state = CHANNEL_OPENED_STATE;
	hv_uio_disconnect_result = -EIO;
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_if_open(&channel,
							hv_uio_test_disconnect), -EIO);
	KUNIT_EXPECT_EQ(test, hv_uio_disconnect_calls, 2U);
}

static int hv_uio_test_reader_wake(struct wait_queue_entry *wait,
				   unsigned int mode, int flags, void *key)
{
	unsigned int *wakes = wait->private;

	(*wakes)++;
	return 1;
}

static void hv_uio_unregister_wakes_reader_test(struct kunit *test)
{
	struct uio_info info = {
		.name = "hv-uio-withdrawal-test",
		.version = "1",
		.irq = UIO_IRQ_CUSTOM,
	};
	struct uio_device *idev;
	struct device *parent;
	wait_queue_entry_t wait;
	unsigned int wakes = 0;
	int ret;

	parent = root_device_register("hv-uio-withdrawal-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(parent));
	ret = uio_register_device(parent, &info);
	if (ret) {
		root_device_unregister(parent);
		KUNIT_FAIL(test, "UIO registration failed: %d", ret);
		return;
	}
	idev = info.uio_dev;
	get_device(&idev->dev);
	init_waitqueue_func_entry(&wait, hv_uio_test_reader_wake);
	wait.private = &wakes;
	add_wait_queue(&idev->wait, &wait);

	uio_unregister_device(&info);
	KUNIT_EXPECT_PTR_EQ(test, idev->info, NULL);
	KUNIT_EXPECT_EQ(test, wakes, 1U);
	remove_wait_queue(&idev->wait, &wait);
	put_device(&idev->dev);
	root_device_unregister(parent);
}

static struct kunit_case hv_uio_ring_mmap_test_cases[] = {
	KUNIT_CASE(hv_uio_unregister_wakes_reader_test),
	KUNIT_CASE(hv_uio_remove_disconnects_open_channel_test),
	KUNIT_CASE(hv_uio_ring_mmap_range_test),
	KUNIT_CASE(hv_uio_ring_mmap_prepare_test),
	KUNIT_CASE(hv_uio_mmap_region_select_test),
	KUNIT_CASE(hv_uio_mmap_prepare_test),
	KUNIT_CASE(hv_uio_pin_lifetime_test),
	KUNIT_CASE(hv_uio_pin_close_before_mapped_test),
	KUNIT_CASE(sysfs_ring_mmap_ops_have_no_close),
	KUNIT_CASE(uio_buffer_mmap_success),
	KUNIT_CASE(sysfs_mmap_prepare_failure_releases_pin),
	KUNIT_CASE(sysfs_mmap_partial_insert_failure_releases_pin),
	KUNIT_CASE(sysfs_mmap_release_during_insert_keeps_pages_alive),
	KUNIT_CASE(ring_mmap_release_preserves_protection_snapshot),
	{}
};

static struct kunit_suite hv_uio_ring_mmap_test_suite = {
	.name = "hyperv-uio-mmap",
	.test_cases = hv_uio_ring_mmap_test_cases,
};

kunit_test_suite(hv_uio_ring_mmap_test_suite);
