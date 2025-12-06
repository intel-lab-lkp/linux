// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Copyright (C) 2025 Pratyush Yadav <pratyush@kernel.org>
 */

/* The documentation for this is in mm/memfd_luo.c */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/liveupdate.h>
#include <linux/kexec_handover.h>
#include <linux/hugetlb.h>
#include <linux/kho/abi/hugetlb.h>
#include <linux/spinlock.h>

#include "hugetlb_internal.h"

struct hugetlb_flb_obj {
	/* Serializes access to ser and its hstates. */
	spinlock_t lock;
	struct hugetlb_ser *ser;
};

static int hugetlb_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_flb_obj *obj;
	u8 nr_hstates = 0;
	struct hstate *h;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	hugetlb_ser = kho_alloc_preserve(sizeof(*hugetlb_ser));
	if (!hugetlb_ser) {
		kfree(obj);
		return -ENOMEM;
	}

	spin_lock_init(&obj->lock);
	obj->ser = hugetlb_ser;

	for_each_hstate(h) {
		struct hugetlb_hstate_ser *hser = &hugetlb_ser->hstates[nr_hstates];

		hser->nr_pages = 0;
		hser->order = h->order;
		nr_hstates++;
	}

	hugetlb_ser->nr_hstates = nr_hstates;

	args->obj = obj;
	args->data = virt_to_phys(hugetlb_ser);

	return 0;
}

static void hugetlb_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	kho_unpreserve_free(phys_to_virt(args->data));
	kfree(args->obj);
}

static void hugetlb_flb_finish(struct liveupdate_flb_op_args *args)
{
	/* No live state on the retrieve side. */
}

static int hugetlb_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	/*
	 * The FLB is only needed for boot-time calculation of how many
	 * hugepages are needed. This is done by early boot handlers already.
	 * Free the serialized state now.
	 */
	kho_restore_free(phys_to_virt(args->data));

	/*
	 * HACK: But since LUO FLB still needs an obj, use ZERO_SIZE_PTR to
	 * satisfy it.
	 */
	args->obj = ZERO_SIZE_PTR;
	return 0;
}

static struct liveupdate_flb_ops hugetlb_luo_flb_ops = {
	.preserve = hugetlb_flb_preserve,
	.unpreserve = hugetlb_flb_unpreserve,
	.finish = hugetlb_flb_finish,
	.retrieve = hugetlb_flb_retrieve,
};

static struct liveupdate_flb hugetlb_luo_flb = {
	.ops = &hugetlb_luo_flb_ops,
	.compatible = HUGETLB_FLB_COMPATIBLE,
};

static struct hugetlb_hstate_ser
*hugetlb_flb_get_hser(struct hugetlb_ser *hugetlb_ser, unsigned int order)
{
	for (u8 i = 0; i < hugetlb_ser->nr_hstates; i++) {
		if (hugetlb_ser->hstates[i].order == order)
			return &hugetlb_ser->hstates[i];
	}

	return NULL;
}

static int hugetlb_flb_add_folio(struct hstate *h)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_flb_obj *obj;
	int err;

	err = liveupdate_flb_get_outgoing(&hugetlb_luo_flb, (void **)&obj);
	if (err)
		return err;

	hugetlb_ser = obj->ser;

	guard(spinlock)(&obj->lock);
	hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
	if (!hser)
		return -ENOENT;

	hser->nr_pages++;
	return 0;
}

static int hugetlb_flb_del_folio(struct hstate *h)
{
	struct hugetlb_ser *hugetlb_ser;
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_flb_obj *obj;
	int err;

	err = liveupdate_flb_get_outgoing(&hugetlb_luo_flb, (void **)&obj);
	if (err)
		return err;

	hugetlb_ser = obj->ser;

	guard(spinlock)(&obj->lock);
	hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
	if (!hser)
		return -ENOENT;

	hser->nr_pages--;
	return 0;
}

unsigned long __init hstate_liveupdate_pages(struct hstate *h)
{
	struct hugetlb_hstate_ser *hser;
	struct hugetlb_ser *hugetlb_ser;
	u64 data;
	int err;

	err = liveupdate_flb_incoming_early(&hugetlb_luo_flb, &data);
	if (err)
		/* If FLB can't be fetched, assume no pages from liveupdate. */
		return 0;

	hugetlb_ser = phys_to_virt(data);

	/* NOTE: No need for locking since this is read-only on incoming side. */
	hser = hugetlb_flb_get_hser(hugetlb_ser, h->order);
	return hser ? hser->nr_pages : 0;
}

void __init hugetlb_luo_init(void)
{
	if (!liveupdate_enabled())
		return;
}
