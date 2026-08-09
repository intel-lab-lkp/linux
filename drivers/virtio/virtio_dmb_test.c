// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tests for the geometry of the withheld range, and for what becomes of a
 * region when a negotiation drops the feature.
 *
 * Included by virtio_dmb.c rather than compiled on its own, so that the tests
 * reach its static functions and its private structure without widening
 * either into a header for their benefit.
 *
 * The geometry cases test a property rather than a value: that whatever the
 * pool size, the area size and the virtqueue count, the withheld range holds
 * VIRTIO_MAP_RESERVE_PAGES slots that are contiguous *within one area*.  That
 * qualifier is the whole point.  An allocation cannot span two areas, so a
 * range that is merely large enough does not guarantee a chain, and a pool
 * whose last area is short can withhold a range whose two pieces are each too
 * small.  A region has to be just above a multiple of the area size to reach
 * it, which no plausible device offers and so no test on real hardware
 * exercises.
 */
#include <kunit/test.h>

/* Geometry only: virtio_dmb_reserved() reads no more of the structure. */
static void dmb_test_shape(struct kunit *test, struct virtio_dmb *dmb,
			   unsigned int nslots, unsigned int area_slots,
			   unsigned int nvqs)
{
	KUNIT_ASSERT_TRUE(test, is_power_of_2(area_slots));

	dmb->nslots = nslots;
	dmb->area_slots = area_slots;
	dmb->area_shift = ilog2(area_slots);
	dmb->nareas = DIV_ROUND_UP(nslots, area_slots);
	dmb->nvqs = nvqs;
}

static struct virtio_dmb *dmb_test_pool(struct kunit *test)
{
	struct virtio_dmb *dmb = kunit_kzalloc(test, sizeof(*dmb), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmb);

	return dmb;
}

/*
 * The longest run of withheld slots that lies inside a single area, which is
 * the largest chain the range can serve.
 */
static unsigned int dmb_test_longest_run(const struct virtio_dmb *dmb,
					 unsigned int reserved)
{
	unsigned int i, best = 0;
	unsigned int start;

	if (!reserved)
		return 0;

	start = dmb->nslots - reserved;

	for (i = 0; i < dmb->nareas; i++) {
		unsigned int base = virtio_dmb_area_base(dmb, i);
		unsigned int end = base + virtio_dmb_area_len(dmb, i);
		unsigned int lo = max(start, base);

		if (end > lo)
			best = max(best, end - lo);
	}

	return best;
}

static void dmb_test_assert_guarantees(struct kunit *test,
				       const struct virtio_dmb *dmb)
{
	unsigned int reserved = virtio_dmb_reserved(dmb);
	unsigned int run;

	if (!reserved)
		return;

	/* One virtqueue at a time can obtain a whole chain. */
	run = dmb_test_longest_run(dmb, reserved);
	KUNIT_EXPECT_GE_MSG(test, run, VIRTIO_MAP_RESERVE_PAGES,
			    "nslots=%u area_slots=%u nareas=%u nvqs=%u reserved=%u",
			    dmb->nslots, dmb->area_slots, dmb->nareas,
			    dmb->nvqs, reserved);

	/* Every virtqueue can obtain one page no running virtqueue can take. */
	KUNIT_EXPECT_GE_MSG(test, reserved,
			    min(dmb->nvqs, dmb->nslots / 2),
			    "nslots=%u nvqs=%u reserved=%u",
			    dmb->nslots, dmb->nvqs, reserved);

	/* The range never takes more than half the pool, nor all of it. */
	KUNIT_EXPECT_LE_MSG(test, reserved, dmb->nslots / 2,
			    "nslots=%u nvqs=%u reserved=%u",
			    dmb->nslots, dmb->nvqs, reserved);
}

/*
 * Every area size the allocator can derive, against pool sizes that put the
 * last area at every length a straddle needs, and virtqueue counts either
 * side of a chain's allowance.
 */
static void dmb_test_reserve_geometry(struct kunit *test)
{
	static const unsigned int area_sizes[] = { 512, 1024, 2048, 4096 };
	struct virtio_dmb *dmb = dmb_test_pool(test);
	unsigned int a, k, tail, nvqs;

	for (a = 0; a < ARRAY_SIZE(area_sizes); a++) {
		unsigned int area_slots = area_sizes[a];

		for (k = 1; k <= 3; k++) {
			for (tail = 0; tail <= 2 * VIRTIO_MAP_RESERVE_PAGES;
			     tail++) {
				unsigned int nslots = k * area_slots + tail;

				for (nvqs = 1; nvqs <= 40; nvqs++) {
					dmb_test_shape(test, dmb, nslots,
						       area_slots, nvqs);
					dmb_test_assert_guarantees(test, dmb);
				}
			}
		}
	}
}

/*
 * The geometries that showed the guarantee was conditional.  Named so that a
 * revision that reintroduces the pool-tail bound fails here rather than in
 * the sweep, where the reason is harder to read off.
 */
static void dmb_test_reserve_straddle(struct kunit *test)
{
	static const struct {
		unsigned int nslots, area_slots, nvqs;
	} cases[] = {
		{ 528, 512, 3 },	/* last area 16, pieces 18 and 16 */
		{ 527, 512, 1 },	/* last area 15, pieces 17 and 15 */
		{ 4114, 4096, 7 },	/* last area 18, pieces 20 and 18 */
		{ 4114, 1024, 7 },	/* same tail, more areas */
		{ 1048600, 512, 17 },	/* last area 24, pieces 24 and 24 */
		{ 4097, 4096, 17 },	/* last area 1, but 47 below it: fine */
		{ 2100, 512, 7 },	/* last area 52: whole range fits it */
	};
	struct virtio_dmb *dmb = dmb_test_pool(test);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		dmb_test_shape(test, dmb, cases[i].nslots, cases[i].area_slots,
			       cases[i].nvqs);
		dmb_test_assert_guarantees(test, dmb);
	}
}

/*
 * The sizing table in Documentation/driver-api/virtio/virtio-dmb.rst states a
 * withheld count for each of its rows, and a device implementer sizes against
 * it.  Every row's last area is long enough to hold the range, so the area
 * term must not change any of them.
 */
static void dmb_test_reserve_documented_sizing(struct kunit *test)
{
	static const struct {
		unsigned int nslots, nvqs, reserved;
	} rows[] = {
		{ 321, 3, 34 },
		{ 1167, 9, 40 },
		{ 2295, 17, 48 },
		{ 8524, 17, 48 },
		{ 17004, 33, 64 },
		{ 33964, 65, 96 },
		{ 67884, 129, 160 },
	};
	static const unsigned int area_sizes[] = { 512, 1024, 2048, 4096 };
	struct virtio_dmb *dmb = dmb_test_pool(test);
	unsigned int i, a;

	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		for (a = 0; a < ARRAY_SIZE(area_sizes); a++) {
			dmb_test_shape(test, dmb, rows[i].nslots,
				       area_sizes[a], rows[i].nvqs);

			KUNIT_EXPECT_EQ_MSG(test, virtio_dmb_reserved(dmb),
					    rows[i].reserved,
					    "nslots=%u nvqs=%u area_slots=%u",
					    rows[i].nslots, rows[i].nvqs,
					    area_sizes[a]);
			dmb_test_assert_guarantees(test, dmb);
		}
	}
}

/* Inert below the threshold, and before the transport reports a count. */
static void dmb_test_reserve_inert(struct kunit *test)
{
	struct virtio_dmb *dmb = dmb_test_pool(test);
	unsigned int nslots;

	dmb_test_shape(test, dmb, 4096, 512, 0);
	KUNIT_EXPECT_EQ(test, virtio_dmb_reserved(dmb), 0);

	for (nslots = 4; nslots < 8 * VIRTIO_MAP_RESERVE_PAGES; nslots++) {
		dmb_test_shape(test, dmb, nslots, 512, 3);
		KUNIT_EXPECT_EQ_MSG(test, virtio_dmb_reserved(dmb), 0,
				    "nslots=%u", nslots);
	}

	dmb_test_shape(test, dmb, 8 * VIRTIO_MAP_RESERVE_PAGES, 512, 1);
	KUNIT_EXPECT_EQ(test, virtio_dmb_reserved(dmb),
			VIRTIO_MAP_RESERVE_PAGES);
}

/*
 * What virtio_dmb_init() does with state an earlier negotiation left behind
 * when this one did not accept the feature: releases it, and refuses to
 * release it under a live virtqueue, which holds kernel addresses inside the
 * mapping.  The refusal is the only error it returns for a device it is taking
 * the region away from, so it is the one a caller turns into the FAILED status
 * bit; releasing it is not an error and sets nothing.  Nothing can reach the
 * refusal, for the reason the function itself gives, so a test is what covers
 * it.  Its dev_warn() is expected output.
 */
static void dmb_test_withdrawn_feature(struct kunit *test)
{
	struct virtio_device *vdev;
	const struct virtio_map_ops *prev;
	struct virtqueue *vq;
	struct virtio_dmb *dmb;

	vdev = kunit_kzalloc(test, sizeof(*vdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vdev);
	vq = kunit_kzalloc(test, sizeof(*vq), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vq);
	prev = kunit_kzalloc(test, sizeof(*prev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, prev);

	spin_lock_init(&vdev->vqs_list_lock);
	INIT_LIST_HEAD(&vdev->vqs);

	/* Nothing installed: nothing to release, and not an error. */
	KUNIT_EXPECT_EQ(test, virtio_dmb_init(vdev), 0);

	/* Not kunit_kzalloc(): the last call below frees this. */
	dmb = kzalloc_obj(*dmb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dmb);
	dmb->prev_map = prev;
	vdev->map = &virtio_dmb_map_ops;
	vdev->vmap.dmb = dmb;
	list_add(&vq->list, &vdev->vqs);

	/* Installed, under a virtqueue: refused, and left addressable. */
	KUNIT_EXPECT_EQ(test, virtio_dmb_init(vdev), -EBUSY);
	KUNIT_EXPECT_PTR_EQ(test, vdev->map, &virtio_dmb_map_ops);
	KUNIT_EXPECT_PTR_EQ(test, vdev->vmap.dmb, dmb);

	/* The same withdrawal with no virtqueue left: released, and no error. */
	list_del(&vq->list);
	KUNIT_EXPECT_EQ(test, virtio_dmb_init(vdev), 0);
	KUNIT_EXPECT_PTR_EQ(test, vdev->map, prev);
}

static struct kunit_case virtio_dmb_test_cases[] = {
	/* Slow: the sweep is tens of thousands of geometries. */
	KUNIT_CASE_SLOW(dmb_test_reserve_geometry),
	KUNIT_CASE(dmb_test_reserve_straddle),
	KUNIT_CASE(dmb_test_reserve_documented_sizing),
	KUNIT_CASE(dmb_test_reserve_inert),
	KUNIT_CASE(dmb_test_withdrawn_feature),
	{}
};

static struct kunit_suite virtio_dmb_test_suite = {
	.name = "virtio_dmb",
	.test_cases = virtio_dmb_test_cases,
};

kunit_test_suite(virtio_dmb_test_suite);
