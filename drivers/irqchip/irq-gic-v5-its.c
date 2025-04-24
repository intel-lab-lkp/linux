// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2025 ARM Limited, All Rights Reserved.
 */

#define pr_fmt(fmt)	"GICv5 ITS: " fmt

#include <linux/bitmap.h>
#include <linux/iommu.h>
#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/msi.h>

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#include "irq-gic-v5.h"
#include "irq-gic-its-msi-parent.h"
#include "irq-msi-lib.h"

#define ITS_FLAGS_NON_COHERENT		BIT(0)

static LIST_HEAD(its_nodes);

static u32 its_readl_relaxed(struct gicv5_its_chip_data *its_node,
			     const u64 reg_offset)
{
	return readl_relaxed(its_node->its_base + reg_offset);
}

static void its_writel_relaxed(struct gicv5_its_chip_data *its_node,
			       const u32 val, const u64 reg_offset)
{
	writel_relaxed(val, its_node->its_base + reg_offset);
}

static void its_writeq_relaxed(struct gicv5_its_chip_data *its_node,
			       const u64 val, const u64 reg_offset)
{
	writeq_relaxed(val, its_node->its_base + reg_offset);
}

static void its_write_table_entry(struct gicv5_its_chip_data *its,
				  __le64 *entry, u64 val)
{
	WRITE_ONCE(*entry, val);
	if (its->flags & ITS_FLAGS_NON_COHERENT)
		dcache_clean_inval_poc((unsigned long)entry,
				       (unsigned long)entry + sizeof(*entry));
	else
		dsb(ishst);
}

#define gicv5_its_wait_for_op(base, reg, mask)				\
	({								\
		int ret;						\
									\
		ret = gicv5_wait_for_op(base, reg, mask, NULL);		\
		if (unlikely(ret == -ETIMEDOUT))			\
			pr_err_ratelimited(#reg" timeout...\n");	\
		ret;							\
	 })

static int gicv5_its_wait_for_invalidation(struct gicv5_its_chip_data *its)
{
	return gicv5_its_wait_for_op(its->its_base, GICV5_ITS_STATUSR,
				     GICV5_ITS_STATUSR_IDLE);
}

static void gicv5_its_syncr(struct gicv5_its_chip_data *its,
			    struct gicv5_its_dev *its_dev)
{
	u64 syncr;

	syncr = FIELD_PREP(GICV5_ITS_SYNCR_SYNC, 1) |
		FIELD_PREP(GICV5_ITS_SYNCR_DEVICEID, its_dev->device_id);

	its_writeq_relaxed(its, syncr, GICV5_ITS_SYNCR);

	gicv5_its_wait_for_op(its->its_base, GICV5_ITS_SYNC_STATUSR,
			      GICV5_ITS_SYNC_STATUSR_IDLE);
}

static int gicv5_its_l2sz_to_l2_bits(unsigned int sz)
{
	switch (sz) {
	case GICV5_ITS_DT_ITT_CFGR_L2SZ_4k:
		return 9;
	case GICV5_ITS_DT_ITT_CFGR_L2SZ_16k:
		return 11;
	case GICV5_ITS_DT_ITT_CFGR_L2SZ_64k:
		return 13;
	default:
		return -EINVAL;
	}
}

static int gicv5_its_itt_cache_inv(struct gicv5_its_chip_data *its,
				   u32 device_id, u16 event_id)
{
	u32 eventr, eidr;
	u64 didr;

	didr = FIELD_PREP(GICV5_ITS_DIDR_DEVICEID, device_id);
	eidr = FIELD_PREP(GICV5_ITS_EIDR_EVENTID, event_id);
	eventr = FIELD_PREP(GICV5_ITS_INV_EVENTR_I, 0x1);

	its_writeq_relaxed(its, didr, GICV5_ITS_DIDR);
	its_writel_relaxed(its, eidr, GICV5_ITS_EIDR);
	its_writel_relaxed(its, eventr, GICV5_ITS_INV_EVENTR);

	return gicv5_its_wait_for_invalidation(its);
}

static void gicv5_its_free_itt_linear(struct gicv5_its_dev *its_dev)
{
	kfree(its_dev->itt_cfg.linear.itt);
}

static void gicv5_its_free_itt_two_level(struct gicv5_its_dev *its_dev)
{
	unsigned int i, num_ents = its_dev->itt_cfg.l2.num_l1_ents;

	for (i = 0; i < num_ents; i++)
		kfree(its_dev->itt_cfg.l2.l2ptrs[i]);

	kfree(its_dev->itt_cfg.l2.l2ptrs);
	kfree(its_dev->itt_cfg.l2.l1itt);
}

static void gicv5_its_free_itt(struct gicv5_its_dev *its_dev)
{
	if (!its_dev->itt_cfg.l2itt)
		gicv5_its_free_itt_linear(its_dev);
	else
		gicv5_its_free_itt_two_level(its_dev);
}

static int gicv5_its_create_itt_linear(struct gicv5_its_chip_data *its,
				       struct gicv5_its_dev *its_dev,
				       unsigned int event_id_bits)
{
	unsigned int num_ents = BIT(event_id_bits);
	__le64 *itt;

	itt = kcalloc(num_ents, sizeof(*itt), GFP_KERNEL);
	if (!itt)
		return -ENOMEM;

	its_dev->itt_cfg.linear.itt = itt;
	its_dev->itt_cfg.linear.num_ents = num_ents;
	its_dev->itt_cfg.l2itt = false;
	its_dev->itt_cfg.event_id_bits = event_id_bits;

	if (its->flags & ITS_FLAGS_NON_COHERENT)
		dcache_clean_inval_poc((unsigned long)itt,
				(unsigned long)itt + num_ents * sizeof(*itt));
	else
		dsb(ishst);

	return 0;
}

/*
 * Allocate a two-level ITT. All ITT entries are allocated in one go, unlike
 * with the device table. Span may be used to limit the second level table
 * size, where possible.
 */
static int gicv5_its_create_itt_two_level(struct gicv5_its_chip_data *its,
					  struct gicv5_its_dev *its_dev,
					  unsigned int event_id_bits,
					  unsigned int itt_l2sz,
					  unsigned int num_events)
{
	unsigned int l1_bits, l2_bits, span, events_per_l2_table,
		     complete_tables, final_span, num_ents;
	__le64 *itt_l1, *itt_l2, **l2ptrs;
	size_t l1sz;
	int ret, i;
	u64 val;

	ret = gicv5_its_l2sz_to_l2_bits(itt_l2sz);
	if (ret < 0 || ret >= event_id_bits) {
		pr_debug("Incorrect l2sz (0x%x) for %u EventID bits. Cannot allocate ITT\n",
			 itt_l2sz, event_id_bits);
		return -EINVAL;
	}

	l2_bits = ret;

	l1_bits = event_id_bits - l2_bits;

	num_ents = BIT(l1_bits);

	itt_l1 = kcalloc(num_ents, sizeof(*itt_l1), GFP_KERNEL);
	if (!itt_l1)
		return -ENOMEM;

	l2ptrs = kcalloc(num_ents, sizeof(*l2ptrs), GFP_KERNEL);
	if (!l2ptrs) {
		kfree(itt_l1);
		return -ENOMEM;
	}

	its_dev->itt_cfg.l2.l2ptrs = l2ptrs;

	its_dev->itt_cfg.l2.l2sz = itt_l2sz;
	its_dev->itt_cfg.l2.l1itt = itt_l1;
	its_dev->itt_cfg.l2.num_l1_ents = num_ents;
	its_dev->itt_cfg.l2itt = true;
	its_dev->itt_cfg.event_id_bits = event_id_bits;

	/*
	 * Need to determine how many entries there are per L2 - this is based
	 * on the number of bits in the table.
	 */
	events_per_l2_table = BIT(l2_bits);
	complete_tables = num_events / events_per_l2_table;
	final_span = order_base_2(num_events % events_per_l2_table);

	for (i = 0; i < num_ents; i++) {
		size_t l2sz;

		span = i == complete_tables ? final_span : l2_bits;

		itt_l2 = kcalloc(BIT(span), sizeof(*itt_l2), GFP_KERNEL);
		if (!itt_l2) {
			ret = -ENOMEM;
			goto out_free;
		}

		its_dev->itt_cfg.l2.l2ptrs[i] = itt_l2;

		l2sz = BIT(span) * sizeof(*itt_l2);

		if (its->flags & ITS_FLAGS_NON_COHERENT)
			dcache_clean_inval_poc((unsigned long)itt_l2,
					       (unsigned long)itt_l2 + l2sz);

		val = (virt_to_phys(itt_l2) & GICV5_ITTL1E_L2_ADDR_MASK) |
		       FIELD_PREP(GICV5_ITTL1E_SPAN, span)		 |
		       FIELD_PREP(GICV5_ITTL1E_VALID, 0x1);

		WRITE_ONCE(itt_l1[i], cpu_to_le64(val));
	}

	if (its->flags & ITS_FLAGS_NON_COHERENT) {
		l1sz = num_ents * sizeof(*itt_l1);
		dcache_clean_inval_poc((unsigned long)itt_l1,
				       (unsigned long)itt_l1 + l1sz);
	} else {
		dsb(ishst);
	}

	return 0;
out_free:
	for (i = i - 1; i >= 0; i--)
		kfree(its_dev->itt_cfg.l2.l2ptrs[i]);

	kfree(its_dev->itt_cfg.l2.l2ptrs);
	kfree(itt_l1);
	return ret;
}

/*
 * Function to check whether the device table or ITT table support
 * a two-level table and if so depending on the number of id_bits
 * requested, determine whether a two-level table is required.
 *
 * Return the 2-level size value if a two level table is deemed
 * necessary.
 */
static bool gicv5_its_l2sz_two_level(bool devtab, u32 its_idr1, u8 id_bits,
				     u8 *sz)
{
	int l2_bits, l2_sz = -EINVAL;

	if (devtab && !FIELD_GET(GICV5_ITS_IDR1_DT_LEVELS, its_idr1))
		return false;

	if (!devtab && !FIELD_GET(GICV5_ITS_IDR1_ITT_LEVELS, its_idr1))
		return false;

	/*
	 * Pick an L2 size that matches the pagesize; if a match
	 * is not found, go for the smallest supported l2 size granule.
	 *
	 * This ensures that we will always be able to allocate
	 * contiguous memory at L2.
	 */
	switch (PAGE_SIZE) {
	case SZ_64K:
		if (GICV5_ITS_IDR1_L2SZ_SUPPORT_64KB(its_idr1)) {
			l2_sz = GICV5_ITS_DT_ITT_CFGR_L2SZ_64k;
			break;
		}
		fallthrough;
	case SZ_16K:
		if (GICV5_ITS_IDR1_L2SZ_SUPPORT_16KB(its_idr1)) {
			l2_sz = GICV5_ITS_DT_ITT_CFGR_L2SZ_16k;
			break;
		}
		fallthrough;
	case SZ_4K:
		if (GICV5_ITS_IDR1_L2SZ_SUPPORT_4KB(its_idr1)) {
			l2_sz = GICV5_ITS_DT_ITT_CFGR_L2SZ_4k;
			break;
		}
		if (GICV5_ITS_IDR1_L2SZ_SUPPORT_16KB(its_idr1)) {
			l2_sz = GICV5_ITS_DT_ITT_CFGR_L2SZ_16k;
			break;
		}
		if (GICV5_ITS_IDR1_L2SZ_SUPPORT_64KB(its_idr1)) {
			l2_sz = GICV5_ITS_DT_ITT_CFGR_L2SZ_64k;
			break;
		}
		break;
	}

	l2_bits = gicv5_its_l2sz_to_l2_bits(l2_sz);

	if (l2_bits < 0 || l2_bits > id_bits)
		return false;

	*sz = l2_sz;

	return true;
}

static int gicv5_its_device_get_itte_ref(struct gicv5_its_dev *its_dev,
					 __le64 **itte, u16 event_id)
{
	if (!its_dev->itt_cfg.l2itt) {
		__le64 *itt = its_dev->itt_cfg.linear.itt;
		*itte = &itt[event_id];
	} else {
		__le64 *l2_itt, *l1_itt = its_dev->itt_cfg.l2.l1itt;
		unsigned int l1_idx, l2_idx, l2_size, l2_bits;
		int ret;

		ret = gicv5_its_l2sz_to_l2_bits(its_dev->itt_cfg.l2.l2sz);
		if (ret < 0)
			return ret;
		l2_bits = ret;

		l1_idx = event_id >> l2_bits;

		if (!FIELD_GET(GICV5_ITTL1E_VALID,
			       le64_to_cpu(l1_itt[l1_idx]))) {
			pr_debug("L1 ITT entry is not valid.\n");
			return -EINVAL;
		}

		l2_idx = event_id & GENMASK(l2_bits - 1, 0);

		l2_size = BIT(FIELD_GET(GICV5_ITTL1E_SPAN,
					le64_to_cpu(l1_itt[l1_idx])));

		// Sanity check our indexing
		if (l2_idx >= l2_size) {
			pr_debug("L2 ITT index (%u) exceeds L2 table size (%u)!\n",
			       l2_idx, l2_size);
			return -EINVAL;
		}
		l2_itt = its_dev->itt_cfg.l2.l2ptrs[l1_idx];
		*itte = &l2_itt[l2_idx];
	}

	return 0;
}

static int gicv5_its_device_cache_inv(struct gicv5_its_chip_data *its,
				      struct gicv5_its_dev *its_dev)
{
	u32 devicer;
	u64 didr;

	didr = FIELD_PREP(GICV5_ITS_DIDR_DEVICEID, its_dev->device_id);
	devicer = FIELD_PREP(GICV5_ITS_INV_DEVICER_I, 0x1)	|
		  FIELD_PREP(GICV5_ITS_INV_DEVICER_EVENTID_BITS,
			     its_dev->itt_cfg.event_id_bits)	|
		  FIELD_PREP(GICV5_ITS_INV_DEVICER_L1, 0x0);
	its_writeq_relaxed(its, didr, GICV5_ITS_DIDR);
	its_writel_relaxed(its, devicer, GICV5_ITS_INV_DEVICER);

	return gicv5_its_wait_for_invalidation(its);
}

/*
 * Allocate a level 2 device table entry, update L1 parent to reference it.
 * Only used for 2-level device tables, and it is called on demand.
 */
static int gicv5_its_alloc_l2_devtab(struct gicv5_its_chip_data *its,
				     unsigned int l1_index)
{
	__le64 *l2devtab, *l1devtab = its->devtab_cfgr.l2.l1devtab;
	u8 span, l2sz, l2_bits;
	u64 l1dte;
	int ret;

	if (FIELD_GET(GICV5_DTL1E_VALID, le64_to_cpu(l1devtab[l1_index])))
		return 0;

	span = FIELD_GET(GICV5_DTL1E_SPAN, le64_to_cpu(l1devtab[l1_index]));
	l2sz = FIELD_GET(GICV5_ITS_DT_CFGR_L2SZ, its->devtab_cfgr.cfgr);

	ret = gicv5_its_l2sz_to_l2_bits(l2sz);
	if (ret < 0)
		return ret;

	l2_bits = ret;

	/*
	 * Span allows us to create a smaller L2 device table.
	 * If it is too large, use the number of allowed L2 bits.
	 */
	if (span > l2_bits)
		span = l2_bits;

	l2devtab = kcalloc(BIT(span), sizeof(*l2devtab), GFP_KERNEL);
	if (!l2devtab)
		return -ENOMEM;

	its->devtab_cfgr.l2.l2ptrs[l1_index] = l2devtab;

	l1dte = FIELD_PREP(GICV5_DTL1E_SPAN, span)			|
		(virt_to_phys(l2devtab) & GICV5_DTL1E_L2_ADDR_MASK)	|
		FIELD_PREP(GICV5_DTL1E_VALID, 0x1);
	its_write_table_entry(its, &l1devtab[l1_index], cpu_to_le64(l1dte));

	return 0;
}

static int gicv5_its_devtab_get_dte_ref(struct gicv5_its_chip_data *its,
					__le64 **dte, u32 device_id,
					bool alloc)
{
	u8 str = FIELD_GET(GICV5_ITS_DT_CFGR_STRUCTURE, its->devtab_cfgr.cfgr);
	unsigned int l2sz, l2_bits, l1_idx, l2_idx;
	__le64 *l1devtab, *l2devtab;
	int ret;

	if (str == GICV5_ITS_DT_ITT_CFGR_STRUCTURE_LINEAR) {
		l2devtab = its->devtab_cfgr.linear.devtab;
		*dte = &l2devtab[device_id];
	} else {
		l2sz = FIELD_GET(GICV5_ITS_DT_CFGR_L2SZ, its->devtab_cfgr.cfgr);
		l1devtab = its->devtab_cfgr.l2.l1devtab;

		ret = gicv5_its_l2sz_to_l2_bits(l2sz);
		if (ret < 0)
			return -EINVAL;

		l2_bits = ret;
		l1_idx = device_id >> l2_bits;
		l2_idx = device_id & GENMASK(l2_bits - 1, 0);

		if (alloc) {
			/*
			 * Allocate a new L2 device table here before
			 * continuing. We make the assumption that the span in
			 * the L1 table has been set correctly, and blindly use
			 * that value.
			 */
			ret = gicv5_its_alloc_l2_devtab(its, l1_idx);
			if (ret)
				return ret;
		} else {
			if (!FIELD_GET(GICV5_DTL1E_VALID,
				       le64_to_cpu(l1devtab[l1_idx])))
				return -EINVAL;
		}

		l2devtab = its->devtab_cfgr.l2.l2ptrs[l1_idx];
		*dte = &l2devtab[l2_idx];
	}

	return 0;
}

/*
 * Register a new device in the device table. Allocate an ITT and
 * program the L2DTE entry according to the ITT structure that
 * was chosen.
 */
static int gicv5_its_device_register(struct gicv5_its_chip_data *its,
				     struct gicv5_its_dev *its_dev)
{
	u8 event_id_bits, device_id_bits, itt_struct, itt_l2sz;
	phys_addr_t itt_phys_base;
	bool two_level_itt;
	u32 idr1, idr2;
	__le64 *dte;
	u64 val;
	int ret;

	device_id_bits = FIELD_GET(GICV5_ITS_DT_CFGR_DEVICEID_BITS,
				   its->devtab_cfgr.cfgr);

	if (its_dev->device_id >= BIT(device_id_bits)) {
		pr_err("Supplied DeviceID (%u) outside of Device Table range (%u)!",
		       its_dev->device_id, (u32)GENMASK(device_id_bits - 1, 0));
		return -EINVAL;
	}

	ret = gicv5_its_devtab_get_dte_ref(its, &dte, its_dev->device_id, true);
	if (ret)
		return ret;

	if (FIELD_GET(GICV5_DTL2E_VALID, le64_to_cpu(*dte)))
		return -EBUSY;

	/*
	 * Determine how many bits we need, validate those against the max.
	 * Based on these, determine if we should go for a 1- or 2-level ITT.
	 */
	event_id_bits = order_base_2(its_dev->num_events);

	idr2 = its_readl_relaxed(its, GICV5_ITS_IDR2);

	if (event_id_bits > FIELD_GET(GICV5_ITS_IDR2_EVENTID_BITS, idr2)) {
		pr_err("Required EventID bits (%u) larger than supported bits (%u)!",
		       event_id_bits,
		       (u8)FIELD_GET(GICV5_ITS_IDR2_EVENTID_BITS, idr2));
		return -EINVAL;
	}

	idr1 = its_readl_relaxed(its, GICV5_ITS_IDR1);

	/*
	 * L2 ITT size is programmed into the L2DTE regardless of
	 * whether a two-level or linear ITT is built, init it.
	 */
	itt_l2sz = 0;

	two_level_itt = gicv5_its_l2sz_two_level(false, idr1, event_id_bits,
						  &itt_l2sz);
	if (two_level_itt)
		ret = gicv5_its_create_itt_two_level(its, its_dev, event_id_bits,
						     itt_l2sz,
						     its_dev->num_events);
	else
		ret = gicv5_its_create_itt_linear(its, its_dev, event_id_bits);
	if (ret)
		return ret;

	itt_phys_base = two_level_itt ? virt_to_phys(its_dev->itt_cfg.l2.l1itt) :
					virt_to_phys(its_dev->itt_cfg.linear.itt);

	itt_struct = two_level_itt ? GICV5_ITS_DT_ITT_CFGR_STRUCTURE_TWO_LEVEL :
				     GICV5_ITS_DT_ITT_CFGR_STRUCTURE_LINEAR;

	val = FIELD_PREP(GICV5_DTL2E_EVENT_ID_BITS, event_id_bits)	|
	      FIELD_PREP(GICV5_DTL2E_ITT_STRUCTURE, itt_struct)		|
	      (itt_phys_base & GICV5_DTL2E_ITT_ADDR_MASK)		|
	      FIELD_PREP(GICV5_DTL2E_ITT_L2SZ, itt_l2sz)		|
	      FIELD_PREP(GICV5_DTL2E_VALID, 0x1);

	its_write_table_entry(its, dte, cpu_to_le64(val));

	ret = gicv5_its_device_cache_inv(its, its_dev);
	if (ret) {
		gicv5_its_free_itt(its_dev);
		its_write_table_entry(its, dte, 0);
		return ret;
	}

	return 0;
}

/*
 * Unregister a device in the device table. Lookup the device by ID, free the
 * corresponding ITT, mark the device as invalid in the device table.
 */
static int gicv5_its_device_unregister(struct gicv5_its_chip_data *its,
				       struct gicv5_its_dev *its_dev)
{
	__le64 *dte;
	int ret;

	ret = gicv5_its_devtab_get_dte_ref(its, &dte, its_dev->device_id, false);
	if (ret) {
		pr_debug("Failed to find DTE for DeviceID 0x%x\n", its_dev->device_id);
		return -EINVAL;
	}

	if (!FIELD_GET(GICV5_DTL2E_VALID, le64_to_cpu(*dte))) {
		pr_debug("Device table entry for DeviceID 0x%x is not valid. Nothing to clean up!",
			 its_dev->device_id);
		return -EINVAL;
	}

	gicv5_its_free_itt(its_dev);

	/* Zero everything - make it clear that this is an invalid entry */
	its_write_table_entry(its, dte, 0);

	return gicv5_its_device_cache_inv(its, its_dev);
}

/*
 * Allocate a 1-level device table. All entries are allocated, but marked
 * invalid.
 */
static int gicv5_its_alloc_devtab_linear(struct gicv5_its_chip_data *its,
					u8 device_id_bits)
{
	__le64 *devtab;
	size_t sz;
	u64 baser;
	u32 cfgr;

	/*
	 * We expect a GICv5 implementation requiring a large number of
	 * deviceID bits to support a 2-level device table. If that's not
	 * the case, cap the number of deviceIDs supported according to the
	 * kmalloc limits so that the system can chug along with a linear
	 * device table.
	 */
	sz = BIT_ULL(device_id_bits) * sizeof(*devtab);
	if (sz > KMALLOC_MAX_SIZE) {
		u8 device_id_cap = ilog2(KMALLOC_MAX_SIZE/sizeof(*devtab));

		pr_warn("Limiting device ID bits from %u to %u\n",
			device_id_bits, device_id_cap);
		device_id_bits = device_id_cap;
	}

	devtab = kcalloc(BIT(device_id_bits), sizeof(*devtab), GFP_KERNEL);
	if (!devtab)
		return -ENOMEM;

	if (its->flags & ITS_FLAGS_NON_COHERENT)
		dcache_clean_inval_poc((unsigned long)devtab,
				       (unsigned long)devtab + sz);
	else
		dsb(ishst);

	cfgr = FIELD_PREP(GICV5_ITS_DT_CFGR_STRUCTURE,
			  GICV5_ITS_DT_ITT_CFGR_STRUCTURE_LINEAR)	|
	       FIELD_PREP(GICV5_ITS_DT_CFGR_L2SZ, 0)			|
	       FIELD_PREP(GICV5_ITS_DT_CFGR_DEVICEID_BITS, device_id_bits);
	its_writel_relaxed(its, cfgr, GICV5_ITS_DT_CFGR);

	baser = virt_to_phys(devtab) & GICV5_ITS_DT_BASER_ADDR_MASK;
	its_writeq_relaxed(its, baser, GICV5_ITS_DT_BASER);

	its->devtab_cfgr.cfgr = cfgr;
	its->devtab_cfgr.linear.devtab = devtab;

	return 0;
}

/*
 * Allocate a 2-level device table. L2 entries are not allocated,
 * they are allocated on-demand.
 */
static int gicv5_its_alloc_devtab_two_level(struct gicv5_its_chip_data *its,
					    u8 device_id_bits,
					    u8 devtab_l2sz)
{
	unsigned int l1_bits, l2_bits, i;
	__le64 *l1devtab, **l2ptrs;
	size_t l1_sz;
	u64 baser;
	u32 cfgr;
	int ret;

	ret = gicv5_its_l2sz_to_l2_bits(devtab_l2sz);
	if (ret < 0)
		return ret;

	l2_bits = ret;

	l1_bits = device_id_bits - l2_bits;
	l1_sz = BIT(l1_bits) * sizeof(*l1devtab);
	/*
	 * With 2-level device table support it is highly unlikely
	 * that we are not able to allocate the required amount of
	 * device table memory to cover deviceID space; cap the
	 * deviceID space if we encounter such set-up.
	 * If this ever becomes a problem we could revisit the policy
	 * behind level 2 size selection to reduce level-1 deviceID bits.
	 */
	if (l1_sz > KMALLOC_MAX_SIZE) {
		l1_bits = ilog2(KMALLOC_MAX_SIZE/sizeof(*l1devtab));

		pr_warn("Limiting device ID bits from %u to %u\n",
			device_id_bits, l1_bits + l2_bits);
		device_id_bits = l1_bits + l2_bits;
		l1_sz = KMALLOC_MAX_SIZE;
	}

	l1devtab = kcalloc(BIT(l1_bits), sizeof(*l1devtab), GFP_KERNEL);
	if (!l1devtab)
		return -ENOMEM;

	l2ptrs = kcalloc(BIT(l1_bits), sizeof(*l2ptrs), GFP_KERNEL);
	if (!l2ptrs) {
		kfree(l1devtab);
		return -ENOMEM;
	}

	for (i = 0; i < BIT(l1_bits); i++)
		l1devtab[i] = cpu_to_le64(FIELD_PREP(GICV5_DTL1E_SPAN, l2_bits));

	if (its->flags & ITS_FLAGS_NON_COHERENT)
		dcache_clean_inval_poc((unsigned long)l1devtab,
				       (unsigned long)l1devtab + l1_sz);
	else
		dsb(ishst);

	cfgr = FIELD_PREP(GICV5_ITS_DT_CFGR_STRUCTURE,
			  GICV5_ITS_DT_ITT_CFGR_STRUCTURE_TWO_LEVEL)	|
	       FIELD_PREP(GICV5_ITS_DT_CFGR_L2SZ, devtab_l2sz)		|
	       FIELD_PREP(GICV5_ITS_DT_CFGR_DEVICEID_BITS, device_id_bits);
	its_writel_relaxed(its, cfgr, GICV5_ITS_DT_CFGR);

	baser = virt_to_phys(l1devtab) & GICV5_ITS_DT_BASER_ADDR_MASK;
	its_writeq_relaxed(its, baser, GICV5_ITS_DT_BASER);

	its->devtab_cfgr.cfgr = cfgr;
	its->devtab_cfgr.l2.l1devtab = l1devtab;
	its->devtab_cfgr.l2.l2ptrs = l2ptrs;

	return 0;
}

/*
 * Initialise the device table as either 1- or 2-level depending on what is
 * supported by the hardware.
 */
static int gicv5_its_init_devtab(struct gicv5_its_chip_data *its)
{
	u8 device_id_bits, devtab_l2sz;
	bool two_level_devtab;
	u32 idr1;

	idr1 = its_readl_relaxed(its, GICV5_ITS_IDR1);

	device_id_bits = FIELD_GET(GICV5_ITS_IDR1_DEVICEID_BITS, idr1);
	two_level_devtab = gicv5_its_l2sz_two_level(true, idr1, device_id_bits,
						     &devtab_l2sz);
	if (two_level_devtab)
		return gicv5_its_alloc_devtab_two_level(its, device_id_bits,
						       devtab_l2sz);
	else
		return gicv5_its_alloc_devtab_linear(its, device_id_bits);
}

static void gicv5_its_compose_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct gicv5_its_dev *its_dev = irq_data_get_irq_chip_data(d);
	struct gicv5_its_chip_data *its = its_dev->its_node;
	u64 addr;

	addr = its->its_trans_phys_base;

	msg->data = FIELD_GET(GICV5_ITS_HWIRQ_EVENT_ID, d->hwirq);
	msi_msg_set_addr(irq_data_get_msi_desc(d), msg, addr);
}

static const struct irq_chip gicv5_its_irq_chip = {
	.name			= "GICv5-ITS-MSI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_get_irqchip_state	= irq_chip_get_parent_state,
	.irq_set_irqchip_state	= irq_chip_set_parent_state,
	.irq_compose_msi_msg	= gicv5_its_compose_msi_msg,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE	  |
				  IRQCHIP_MASK_ON_SUSPEND
};

static struct gicv5_its_dev *gicv5_its_find_device(struct gicv5_its_chip_data *its,
						   u32 device_id)
{
	struct gicv5_its_dev *dev = xa_load(&its->its_devices, device_id);

	return dev ? dev : ERR_PTR(-ENODEV);
}

static struct gicv5_its_dev *gicv5_its_alloc_device(
				struct gicv5_its_chip_data *its, int nvec,
				u32 dev_id)
{
	struct gicv5_its_dev *its_dev;
	int ret;

	its_dev = gicv5_its_find_device(its, dev_id);
	if (!IS_ERR(its_dev)) {
		pr_debug("A device with this DeviceID (0x%x) has already been registered.\n",
			 dev_id);

		if (nvec > its_dev->num_events) {
			pr_debug("Requesting more ITT entries than allocated\n");
			return ERR_PTR(-ENXIO);
		}

		its_dev->shared = true;

		return its_dev;
	}

	its_dev = kzalloc(sizeof(*its_dev), GFP_KERNEL);
	if (!its_dev)
		return ERR_PTR(-ENOMEM);

	its_dev->device_id = dev_id;
	its_dev->num_events = nvec;
	its_dev->num_mapped_events = 0;

	ret = gicv5_its_device_register(its, its_dev);
	if (ret) {
		pr_debug("Failed to register the device\n");
		kfree(its_dev);
		return ERR_PTR(ret);
	}

	gicv5_its_device_cache_inv(its, its_dev);

	/*
	 * This is the first time we have seen this device. Hence, it is not
	 * shared.
	 */
	its_dev->shared = false;

	its_dev->its_node = its;

	its_dev->event_map =
		(unsigned long *)bitmap_zalloc(its_dev->num_events, GFP_KERNEL);
	if (!its_dev->event_map) {
		gicv5_its_device_unregister(its, its_dev);
		kfree(its_dev);
		return ERR_PTR(-ENOMEM);
	}

	xa_store(&its->its_devices, dev_id, its_dev, GFP_KERNEL);

	return its_dev;
}

static int gicv5_its_msi_prepare(struct irq_domain *domain, struct device *dev,
				 int nvec, msi_alloc_info_t *info)
{
	u32 dev_id = info->scratchpad[0].ul;
	struct msi_domain_info *msi_info;
	struct gicv5_its_chip_data *its;
	struct gicv5_its_dev *its_dev;

	msi_info = msi_get_domain_info(domain);
	its = msi_info->data;

	guard(mutex)(&its->dev_alloc_lock);

	its_dev = gicv5_its_alloc_device(its, nvec, dev_id);
	if (IS_ERR(its_dev))
		return PTR_ERR(its_dev);

	if (info->flags & MSI_ALLOC_FLAGS_PROXY_DEVICE)
		its_dev->shared = true;

	return 0;
}

static struct msi_domain_ops its_msi_domain_ops = {
	.msi_prepare	= gicv5_its_msi_prepare,
};

static int gicv5_its_alloc_event(struct gicv5_its_dev *its_dev, u16 event_id,
				 u32 lpi)
{
	struct gicv5_its_chip_data *its = its_dev->its_node;
	u64 itt_entry;
	__le64 *itte;
	int ret;

	if (event_id >= its_dev->num_events) {
		pr_debug("EventID 0x%x outside of ITT range (0x%x)\n", event_id,
		       its_dev->num_events);
		return -EINVAL;
	}

	if (WARN(its_dev->num_mapped_events == its_dev->num_events,
		"Reached maximum number of events\n"))
		return -EINVAL;

	ret = gicv5_its_device_get_itte_ref(its_dev, &itte, event_id);
	if (ret)
		return ret;

	if (FIELD_GET(GICV5_ITTL2E_VALID, *itte))
		return -EEXIST;

	itt_entry = FIELD_PREP(GICV5_ITTL2E_LPI_ID, lpi) |
		    FIELD_PREP(GICV5_ITTL2E_VALID, 0x1);

	its_write_table_entry(its, itte, cpu_to_le64(itt_entry));

	gicv5_its_itt_cache_inv(its, its_dev->device_id, event_id);

	its_dev->num_mapped_events += 1;

	return 0;
}

static void gicv5_its_free_event(struct gicv5_its_dev *its_dev, u16 event_id)
{
	struct gicv5_its_chip_data *its = its_dev->its_node;
	u64 itte_val;
	__le64 *itte;
	int ret;

	if (WARN(!its_dev->num_mapped_events, "No mapped events\n"))
		return;

	ret = gicv5_its_device_get_itte_ref(its_dev, &itte, event_id);
	if (ret) {
		pr_debug("Failed to get the ITTE!\n");
		return;
	}

	itte_val = le64_to_cpu(*itte);
	itte_val &= ~GICV5_ITTL2E_VALID;

	its_write_table_entry(its, itte, cpu_to_le64(itte_val));

	gicv5_its_itt_cache_inv(its, its_dev->device_id, event_id);

	its_dev->num_mapped_events -= 1;
}

static int gicv5_its_alloc_eventid(struct gicv5_its_dev *its_dev,
				   unsigned int nr_irqs, u32 *eventid)
{
	int ret;

	ret = bitmap_find_free_region(its_dev->event_map,
				      its_dev->num_events,
				      get_count_order(nr_irqs));

	if (ret < 0)
		return ret;

	*eventid = ret;

	return 0;
}

static void gicv5_its_free_eventid(struct gicv5_its_dev *its_dev,
				   u32 event_id_base,
				   unsigned int nr_irqs)
{
	bitmap_release_region(its_dev->event_map, event_id_base,
				      get_count_order(nr_irqs));
}

static int gicv5_its_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				      unsigned int nr_irqs, void *arg)
{
	u32 device_id, event_id_base, lpi;
	struct msi_domain_info *msi_info;
	struct gicv5_its_chip_data *its;
	struct gicv5_its_dev *its_dev;
	msi_alloc_info_t *info = arg;
	irq_hw_number_t hwirq;
	struct irq_data *irqd;
	int ret, i;

	device_id = info->scratchpad[0].ul;

	msi_info = msi_get_domain_info(domain);
	its = msi_info->data;

	mutex_lock(&its->dev_alloc_lock);

	its_dev = gicv5_its_find_device(its, device_id);
	if (IS_ERR(its_dev)) {
		mutex_unlock(&its->dev_alloc_lock);
		return PTR_ERR(its_dev);
	}

	ret = gicv5_its_alloc_eventid(its_dev, nr_irqs, &event_id_base);
	if (ret) {
		mutex_unlock(&its->dev_alloc_lock);
		return ret;
	}

	mutex_unlock(&its->dev_alloc_lock);

	ret = iommu_dma_prepare_msi(info->desc, its->its_trans_phys_base);
	if (ret)
		goto out_eventid;

	for (i = 0; i < nr_irqs; i++) {
		lpi = gicv5_alloc_lpi();
		if (ret < 0) {
			pr_debug("Failed to find free LPI!\n");
			goto out_eventid;
		}

		ret = irq_domain_alloc_irqs_parent(domain, virq + i, 1, &lpi);
		if (ret)
			goto out_free_lpi;

		/*
		 * Store eventid and deviceid into the hwirq for later use.
		 *
		 *	hwirq  = event_id << 32 | device_id
		 */
		hwirq = FIELD_PREP(GICV5_ITS_HWIRQ_DEVICE_ID, device_id) |
			FIELD_PREP(GICV5_ITS_HWIRQ_EVENT_ID, (u64)event_id_base + i);
		irq_domain_set_info(domain, virq + i, hwirq,
				    &gicv5_its_irq_chip, its_dev,
				    handle_fasteoi_irq, NULL, NULL);

		irqd = irq_get_irq_data(virq + i);
		irqd_set_single_target(irqd);
		irqd_set_affinity_on_activate(irqd);
		irqd_set_resend_when_in_progress(irqd);
	}

	return 0;
out_free_lpi:
	gicv5_free_lpi(lpi);
out_eventid:
	gicv5_its_free_eventid(its_dev, event_id_base, nr_irqs);

	return ret;
}

static void gicv5_its_irq_domain_free(struct irq_domain *domain, unsigned int virq,
				      unsigned int nr_irqs)
{
	struct msi_domain_info *msi_info;
	struct gicv5_its_chip_data *its;
	struct gicv5_its_dev *its_dev;
	struct irq_data *d;
	u16 event_id_base;
	bool free_device;
	u32 device_id;
	int i;

	msi_info = msi_get_domain_info(domain);
	its = msi_info->data;

	d = irq_domain_get_irq_data(domain, virq);
	device_id = FIELD_GET(GICV5_ITS_HWIRQ_DEVICE_ID, d->hwirq);
	event_id_base = FIELD_GET(GICV5_ITS_HWIRQ_EVENT_ID, d->hwirq);

	guard(mutex)(&its->dev_alloc_lock);

	its_dev = gicv5_its_find_device(its, device_id);
	if (IS_ERR(its_dev)) {
		pr_debug("Couldn't find the ITS device!\n");
		return;
	}

	bitmap_release_region(its_dev->event_map, event_id_base,
			      get_count_order(nr_irqs));

	free_device = !its_dev->shared && bitmap_empty(its_dev->event_map,
						       its_dev->num_events);

	/*  Hierarchically free irq data */
	for (i = 0; i < nr_irqs; i++) {
		d = irq_domain_get_irq_data(domain, virq + i);

		gicv5_free_lpi(d->parent_data->hwirq);
		irq_domain_reset_irq_data(d);
	}
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);

	gicv5_its_syncr(its, its_dev);
	gicv5_irs_syncr();

	if (free_device) {
		gicv5_its_device_unregister(its, its_dev);
		bitmap_free(its_dev->event_map);
		xa_erase(&its->its_devices, device_id);
		kfree(its_dev);
	}
}

static int gicv5_its_irq_domain_activate(struct irq_domain *domain,
					 struct irq_data *d, bool reserve)
{
	struct gicv5_its_dev *its_dev = irq_data_get_irq_chip_data(d);
	u16 event_id;
	u32 lpi;

	event_id = FIELD_GET(GICV5_ITS_HWIRQ_EVENT_ID, d->hwirq);
	lpi = d->parent_data->hwirq;

	return gicv5_its_alloc_event(its_dev, event_id, lpi);
}

static void gicv5_its_irq_domain_deactivate(struct irq_domain *domain,
					    struct irq_data *d)
{
	struct gicv5_its_dev *its_dev = irq_data_get_irq_chip_data(d);
	u16 event_id;

	event_id = FIELD_GET(GICV5_ITS_HWIRQ_EVENT_ID, d->hwirq);

	gicv5_its_free_event(its_dev, event_id);
}
static const struct irq_domain_ops gicv5_its_irq_domain_ops = {
	.alloc		= gicv5_its_irq_domain_alloc,
	.free		= gicv5_its_irq_domain_free,
	.activate	= gicv5_its_irq_domain_activate,
	.deactivate	= gicv5_its_irq_domain_deactivate,
	.select		= msi_lib_irq_domain_select,
};

static int gicv5_its_wait_for_cr0(struct gicv5_its_chip_data *its)
{
	return gicv5_its_wait_for_op(its->its_base, GICV5_ITS_CR0,
				     GICV5_ITS_CR0_IDLE);
}

static void gicv5_its_print_info(struct gicv5_its_chip_data *its_node)
{
	bool devtab_linear;
	u8 device_id_bits;
	u8 str;

	device_id_bits = FIELD_GET(GICV5_ITS_DT_CFGR_DEVICEID_BITS,
			 its_node->devtab_cfgr.cfgr);

	str = FIELD_GET(GICV5_ITS_DT_CFGR_STRUCTURE, its_node->devtab_cfgr.cfgr);
	devtab_linear = (str == GICV5_ITS_DT_ITT_CFGR_STRUCTURE_LINEAR);

	pr_info("ITS %s enabled using %s device table device_id_bits %u\n",
		fwnode_get_name(its_node->fwnode),
		devtab_linear ? "linear" : "2-level",
		device_id_bits);
}

static int __init gicv5_its_init_bases(phys_addr_t its_trans_base,
				       void __iomem *its_base,
				       struct fwnode_handle *handle,
				       struct irq_domain *parent_domain)
{
	struct device_node *np = to_of_node(handle);
	struct gicv5_its_chip_data *its_node;
	struct msi_domain_info *info;
	struct irq_domain *d;
	u32 cr0, cr1;
	bool enabled;
	int ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	its_node = kzalloc(sizeof(*its_node), GFP_KERNEL);
	if (!its_node) {
		kfree(info);
		return -ENOMEM;
	}

	info->ops = &its_msi_domain_ops;
	info->data = its_node;

	mutex_init(&its_node->dev_alloc_lock);
	xa_init(&its_node->its_devices);
	its_node->fwnode = handle;
	its_node->its_base = its_base;
	its_node->its_trans_phys_base = its_trans_base;

	d = irq_domain_create_hierarchy(parent_domain, IRQ_DOMAIN_FLAG_ISOLATED_MSI,
					0, handle, &gicv5_its_irq_domain_ops, info);
	its_node->domain = d;
	irq_domain_update_bus_token(its_node->domain, DOMAIN_BUS_NEXUS);

	its_node->domain->msi_parent_ops = &gic_its_msi_parent_ops;
	its_node->domain->flags |= IRQ_DOMAIN_FLAG_MSI_PARENT;

	cr0 = its_readl_relaxed(its_node, GICV5_ITS_CR0);
	enabled = FIELD_GET(GICV5_ITS_CR0_ITSEN, cr0);
	if (WARN(enabled, "ITS %s enabled, disabling it before proceeding\n",
		 np->full_name)) {
		cr0 = FIELD_PREP(GICV5_ITS_CR0_ITSEN, 0x0);
		its_writel_relaxed(its_node, cr0, GICV5_ITS_CR0);
		ret = gicv5_its_wait_for_cr0(its_node);
		if (ret) {
			irq_domain_remove(its_node->domain);
			kfree(info);
			kfree(its_node);
			return ret;
		}
	}

	if (of_property_read_bool(np, "dma-noncoherent")) {
		/*
		 * A non-coherent ITS implies that some cache levels cannot be
		 * used coherently by the cores and GIC. Our only option is to mark
		 * memory attributes for the GIC as non-cacheable; by default,
		 * non-cacheable memory attributes imply outer-shareable
		 * shareability, the value written into ITS_CR1_SH is ignored.
		 */
		cr1 = FIELD_PREP(GICV5_ITS_CR1_ITT_RA, GICV5_NO_READ_ALLOC)	|
		      FIELD_PREP(GICV5_ITS_CR1_DT_RA, GICV5_NO_READ_ALLOC)	|
		      FIELD_PREP(GICV5_ITS_CR1_IC, GICV5_NON_CACHE)		|
		      FIELD_PREP(GICV5_ITS_CR1_OC, GICV5_NON_CACHE);
		its_node->flags |= ITS_FLAGS_NON_COHERENT;
	} else {
		cr1 = FIELD_PREP(GICV5_ITS_CR1_ITT_RA, GICV5_READ_ALLOC)	|
		      FIELD_PREP(GICV5_ITS_CR1_DT_RA, GICV5_READ_ALLOC)		|
		      FIELD_PREP(GICV5_ITS_CR1_IC, GICV5_WB_CACHE)		|
		      FIELD_PREP(GICV5_ITS_CR1_OC, GICV5_WB_CACHE)		|
		      FIELD_PREP(GICV5_ITS_CR1_SH, GICV5_INNER_SHARE);
	}

	its_writel_relaxed(its_node, cr1, GICV5_ITS_CR1);

	ret = gicv5_its_init_devtab(its_node);
	if (ret) {
		irq_domain_remove(its_node->domain);
		kfree(info);
		kfree(its_node);
		return ret;
	}

	cr0 = FIELD_PREP(GICV5_ITS_CR0_ITSEN, 0x1);
	its_writel_relaxed(its_node, cr0, GICV5_ITS_CR0);

	ret = gicv5_its_wait_for_cr0(its_node);
	if (ret) {
		irq_domain_remove(its_node->domain);
		kfree(info);
		kfree(its_node);
		return ret;
	}

	list_add(&its_node->entry, &its_nodes);

	gicv5_its_print_info(its_node);

	return 0;
}

static int __init gicv5_its_init(struct device_node *node)
{
	void __iomem *its_base;
	struct resource res;
	int ret;

	its_base = of_io_request_and_map(node, 0, "ITS");
	if (IS_ERR(its_base)) {
		pr_err("%pOF: unable to map GICv5 ITS_CONFIG_FRAME\n", node);
		return PTR_ERR(its_base);
	}

	/*
	 * The ITS_TRANSLATE_FRAME is the second reg entry, (first is the
	 * ITS_CONFIG_FRAME) - extract it and use it to init ITS data
	 * structures.
	 */
	ret = of_address_to_resource(node, 1, &res);
	if (ret)
		goto out_unmap;

	ret = gicv5_its_init_bases(res.start, its_base, &node->fwnode,
				   gicv5_global_data.lpi_domain);
	if (ret)
		goto out_unmap;

	return 0;
out_unmap:
	iounmap(its_base);
	return ret;
}

void __init gicv5_its_of_probe(struct device_node *parent)
{
	struct device_node *np;

	for_each_available_child_of_node(parent, np) {
		if (!of_device_is_compatible(np, "arm,gic-v5-its"))
			continue;

		if (gicv5_its_init(np))
			pr_err("Failed to init ITS %s\n", np->full_name);
	}
}
