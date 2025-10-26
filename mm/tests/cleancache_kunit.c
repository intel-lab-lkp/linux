// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit test for the Cleancache.
 *
 * Copyright (C) 2025, Google LLC.
 * Author: Suren Baghdasaryan <surenb@google.com>
 */
#include <kunit/test.h>

#include <linux/cleancache.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

#include "../internal.h"

#define INODE_COUNT		5
#define FOLIOS_PER_INODE	4
#define FOLIO_COUNT		(INODE_COUNT * FOLIOS_PER_INODE)

static const u32 TEST_CONTENT = 0xBADCAB32;

struct inode_data {
	struct address_space mapping;
	struct inode inode;
	struct folio *folios[FOLIOS_PER_INODE];
};

static struct test_data {
	/* Mock a fs */
	struct super_block sb;
	struct inode_data inodes[INODE_COUNT];
	/* Folios donated to the cleancache pools */
	struct folio *pool_folios[FOLIO_COUNT];
	/* Auxiliary folio */
	struct folio *aux_folio;
	int pool_id;
} test_data;

static void set_folio_content(struct folio *folio, u32 value)
{
	u32 *data;

	data = kmap_local_folio(folio, 0);
	*data = value;
	kunmap_local(data);
}

static u32 get_folio_content(struct folio *folio)
{
	unsigned long value;
	u32 *data;

	data = kmap_local_folio(folio, 0);
	value = *data;
	kunmap_local(data);

	return value;
}

static void fill_cleancache(struct kunit *test)
{
	struct inode_data *inode_data;
	struct folio *folio;

	/* Store inode folios into cleancache */
	for (int inode = 0; inode < INODE_COUNT; inode++) {
		inode_data = &test_data.inodes[inode];
		for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
			folio = inode_data->folios[fidx];
			KUNIT_EXPECT_NOT_NULL(test, folio);
			folio_lock(folio); /* Folio has to be locked */
			folio_set_workingset(folio);
			KUNIT_EXPECT_TRUE(test, cleancache_store_folio(&inode_data->inode, folio));
			folio_unlock(folio);
		}
	}
}

static int cleancache_suite_init(struct kunit_suite *suite)
{
	LIST_HEAD(pool_folios);

	/* Add a fake fs superblock */
	cleancache_add_fs(&test_data.sb);

	/* Initialize fake inodes */
	for (int inode = 0; inode < INODE_COUNT; inode++) {
		struct inode_data *inode_data = &test_data.inodes[inode];

		inode_data->inode.i_sb = &test_data.sb;
		inode_data->inode.i_ino = inode;
		inode_data->inode.i_mapping = &inode_data->mapping;
		inode_data->mapping.host = &inode_data->inode;

		/* Allocate folios for the inode  */
		for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
			struct folio *folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

			if (!folio)
				return -ENOMEM;

			set_folio_content(folio, (u32)fidx);
			folio->mapping = &inode_data->mapping;
			folio->index = PAGE_SIZE * fidx;
			inode_data->folios[fidx] = folio;
		}
	}

	/* Register new cleancache pool and donate test folios */
	test_data.pool_id = cleancache_backend_register_pool("kunit_pool");
	if (test_data.pool_id < 0)
		return -EINVAL;

	/* Allocate folios and put them to cleancache  */
	for (int fidx = 0; fidx < FOLIO_COUNT; fidx++) {
		struct folio *folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

		if (!folio)
			return -ENOMEM;

		folio_ref_freeze(folio, 1);
		test_data.pool_folios[fidx] = folio;
		list_add(&folio->lru, &pool_folios);
	}

	cleancache_backend_put_folios(test_data.pool_id, &pool_folios);

	/* Allocate auxiliary folio for testing  */
	test_data.aux_folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!test_data.aux_folio)
		return -ENOMEM;

	return 0;
}

static void cleancache_suite_exit(struct kunit_suite *suite)
{
	/* Take back donated folios and free them */
	for (int fidx = 0; fidx < FOLIO_COUNT; fidx++) {
		struct folio *folio = test_data.pool_folios[fidx];

		if (folio) {
			if (!cleancache_backend_get_folio(test_data.pool_id,
							  folio))
				set_page_refcounted(&folio->page);
			folio_put(folio);
		}
	}

	/* Free the auxiliary folio */
	if (test_data.aux_folio) {
		test_data.aux_folio->mapping = NULL;
		folio_put(test_data.aux_folio);
	}

	/* Free inode folios */
	for (int inode = 0; inode < INODE_COUNT; inode++) {
		for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
			struct folio *folio = test_data.inodes[inode].folios[fidx];

			if (folio) {
				folio->mapping = NULL;
				folio_put(folio);
			}
		}
	}

	cleancache_remove_fs(&test_data.sb);
}

static int cleancache_test_init(struct kunit *test)
{
	/* Pass pool_id to cleancache to restrict pools that can be used for tests */
	test->priv = &test_data.pool_id;

	return 0;
}

static void cleancache_restore_test(struct kunit *test)
{
	struct inode_data *inode_data;
	struct folio *folio;

	/* Store inode folios into cleancache */
	fill_cleancache(test);

	/* Restore and validate folios stored in cleancache */
	for (int inode = 0; inode < INODE_COUNT; inode++) {
		inode_data = &test_data.inodes[inode];
		for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
			folio = inode_data->folios[fidx];
			test_data.aux_folio->mapping = folio->mapping;
			test_data.aux_folio->index = folio->index;
			KUNIT_EXPECT_TRUE(test, cleancache_restore_folio(&inode_data->inode,
									 test_data.aux_folio));
			KUNIT_EXPECT_EQ(test, get_folio_content(test_data.aux_folio),
					get_folio_content(folio));
		}
	}
}

static void cleancache_walk_and_restore_test(struct kunit *test)
{
	struct cleancache_inode *ccinode;
	struct inode_data *inode_data;
	struct folio *folio;

	/* Store inode folios into cleancache */
	fill_cleancache(test);

	/* Restore and validate folios stored in the first inode */
	inode_data = &test_data.inodes[0];
	ccinode = cleancache_start_inode_walk(&inode_data->inode, FOLIOS_PER_INODE);
	KUNIT_EXPECT_NOT_NULL(test, ccinode);
	for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
		folio = inode_data->folios[fidx];
		test_data.aux_folio->mapping = folio->mapping;
		test_data.aux_folio->index = folio->index;
		KUNIT_EXPECT_TRUE(test, cleancache_restore_from_inode(ccinode,
								      test_data.aux_folio));
		KUNIT_EXPECT_EQ(test, get_folio_content(test_data.aux_folio),
				get_folio_content(folio));
	}
	cleancache_end_inode_walk(ccinode);
}

static void cleancache_invalidate_test(struct kunit *test)
{
	struct inode_data *inode_data;
	struct folio *folio;

	/* Store inode folios into cleancache */
	fill_cleancache(test);

	/* Invalidate one folio */
	inode_data = &test_data.inodes[0];
	folio = inode_data->folios[0];
	test_data.aux_folio->mapping = folio->mapping;
	test_data.aux_folio->index = folio->index;
	KUNIT_EXPECT_TRUE(test, cleancache_restore_folio(&inode_data->inode,
							 test_data.aux_folio));
	folio_lock(folio); /* Folio has to be locked */
	KUNIT_EXPECT_TRUE(test, cleancache_invalidate_folio(&inode_data->inode,
							    inode_data->folios[0]));
	folio_unlock(folio);
	KUNIT_EXPECT_FALSE(test, cleancache_restore_folio(&inode_data->inode,
							  test_data.aux_folio));

	/* Invalidate one node */
	inode_data = &test_data.inodes[1];
	KUNIT_EXPECT_TRUE(test, cleancache_invalidate_inode(&inode_data->inode));

	/* Verify results */
	for (int inode = 0; inode < INODE_COUNT; inode++) {
		inode_data = &test_data.inodes[inode];
		for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
			folio = inode_data->folios[fidx];
			test_data.aux_folio->mapping = folio->mapping;
			test_data.aux_folio->index = folio->index;
			if (inode == 0 && fidx == 0) {
				/* Folio should be missing */
				KUNIT_EXPECT_FALSE(test,
					cleancache_restore_folio(&inode_data->inode,
								 test_data.aux_folio));
				continue;
			}
			if (inode == 1) {
				/* Folios in the node should be missing */
				KUNIT_EXPECT_FALSE(test,
					cleancache_restore_folio(&inode_data->inode,
								 test_data.aux_folio));
				continue;
			}
			KUNIT_EXPECT_TRUE(test,
					cleancache_restore_folio(&inode_data->inode,
								 test_data.aux_folio));
			KUNIT_EXPECT_EQ(test, get_folio_content(test_data.aux_folio),
					get_folio_content(folio));
		}
	}
}

static void cleancache_reclaim_test(struct kunit *test)
{
	struct inode_data *inode_data;
	struct inode_data *inode_new;
	unsigned long new_index;
	struct folio *folio;

	/* Store inode folios into cleancache */
	fill_cleancache(test);

	/*
	 * Store one extra new folio. There should be no free folios, so the
	 * oldest folio will be reclaimed to store new folio. Add it into the
	 * last node at the next unoccupied offset.
	 */
	inode_new = &test_data.inodes[INODE_COUNT - 1];
	new_index = inode_new->folios[FOLIOS_PER_INODE - 1]->index + PAGE_SIZE;

	test_data.aux_folio->mapping = &inode_new->mapping;
	test_data.aux_folio->index = new_index;
	set_folio_content(test_data.aux_folio, TEST_CONTENT);
	folio_lock(test_data.aux_folio); /* Folio has to be locked */
	folio_set_workingset(test_data.aux_folio);
	KUNIT_EXPECT_TRUE(test, cleancache_store_folio(&inode_new->inode, test_data.aux_folio));
	folio_unlock(test_data.aux_folio);

	/* Verify results */
	for (int inode = 0; inode < INODE_COUNT; inode++) {
		inode_data = &test_data.inodes[inode];
		for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
			folio = inode_data->folios[fidx];
			test_data.aux_folio->mapping = folio->mapping;
			test_data.aux_folio->index = folio->index;
			/*
			 * The first folio of the first node was added first,
			 * so it's the oldest and must have been reclaimed.
			 */
			if (inode == 0 && fidx == 0) {
				/* Reclaimed folio should be missing */
				KUNIT_EXPECT_FALSE_MSG(test,
						cleancache_restore_folio(&inode_data->inode,
									 test_data.aux_folio),
						"inode %d, folio %d is invalid\n", inode, fidx);
				continue;
			}
			KUNIT_EXPECT_TRUE_MSG(test,
					cleancache_restore_folio(&inode_data->inode,
								 test_data.aux_folio),
								"inode %d, folio %d is invalid\n",
								inode, fidx);
			KUNIT_EXPECT_EQ_MSG(test, get_folio_content(test_data.aux_folio),
					    get_folio_content(folio),
					    "inode %d, folio %d content is invalid\n",
					    inode, fidx);
		}
	}

	/* Auxiliary folio should be stored */
	test_data.aux_folio->mapping = &inode_new->mapping;
	test_data.aux_folio->index = new_index;
	KUNIT_EXPECT_TRUE_MSG(test,
			      cleancache_restore_folio(&inode_new->inode, test_data.aux_folio),
			      "inode %lu, folio %ld is invalid\n",
			      inode_new->inode.i_ino, new_index);
	KUNIT_EXPECT_EQ_MSG(test, get_folio_content(test_data.aux_folio), TEST_CONTENT,
			    "inode %lu, folio %ld content is invalid\n",
			    inode_new->inode.i_ino, new_index);
}

static void cleancache_backend_api_test(struct kunit *test)
{
	struct folio *folio;
	LIST_HEAD(folios);
	int used = 0;

	/* Store inode folios into cleancache */
	fill_cleancache(test);

	/* Get all donated folios back */
	for (int fidx = 0; fidx < FOLIO_COUNT; fidx++) {
		KUNIT_EXPECT_EQ(test, cleancache_backend_get_folio(test_data.pool_id,
						test_data.pool_folios[fidx]),  0);
		set_page_refcounted(&test_data.pool_folios[fidx]->page);
	}

	/* Try putting a refcounted folio */
	KUNIT_EXPECT_NE(test, cleancache_backend_put_folio(test_data.pool_id,
					test_data.pool_folios[0]), 0);

	/* Put some of the folios back into cleancache */
	for (int fidx = 0; fidx < FOLIOS_PER_INODE; fidx++) {
		folio_ref_freeze(test_data.pool_folios[fidx], 1);
		KUNIT_EXPECT_EQ(test, cleancache_backend_put_folio(test_data.pool_id,
						test_data.pool_folios[fidx]), 0);
	}

	/* Put the rest back into cleancache but keep half of folios still refcounted */
	for (int fidx = FOLIOS_PER_INODE; fidx < FOLIO_COUNT; fidx++) {
		if (fidx % 2)
			folio_ref_freeze(test_data.pool_folios[fidx], 1);
		else
			used++;
		list_add(&test_data.pool_folios[fidx]->lru, &folios);
	}
	KUNIT_EXPECT_NE(test, cleancache_backend_put_folios(test_data.pool_id,
					&folios), 0);
	/* Used folios should be still in the list */
	KUNIT_EXPECT_EQ(test, list_count_nodes(&folios), used);

	/* Release refcounts and put the remaining folios into cleancache */
	list_for_each_entry(folio, &folios, lru)
		folio_ref_freeze(folio, 1);
	KUNIT_EXPECT_EQ(test, cleancache_backend_put_folios(test_data.pool_id,
					&folios), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&folios));
}

static struct kunit_case cleancache_test_cases[] = {
	KUNIT_CASE(cleancache_restore_test),
	KUNIT_CASE(cleancache_walk_and_restore_test),
	KUNIT_CASE(cleancache_invalidate_test),
	KUNIT_CASE(cleancache_reclaim_test),
	KUNIT_CASE(cleancache_backend_api_test),
	{},
};

static struct kunit_suite hashtable_test_module = {
	.name = "cleancache",
	.init = cleancache_test_init,
	.suite_init = cleancache_suite_init,
	.suite_exit = cleancache_suite_exit,
	.test_cases = cleancache_test_cases,
};

kunit_test_suites(&hashtable_test_module);

MODULE_DESCRIPTION("KUnit test for the Kernel Cleancache");
MODULE_LICENSE("GPL");
