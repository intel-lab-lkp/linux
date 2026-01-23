// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 */
#include <linux/device.h>
#include <linux/ndctl.h>
#include <linux/uuid.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/nd.h>
#include "nd-core.h"
#include "label.h"
#include "nd.h"

static guid_t nvdimm_btt_guid;
static guid_t nvdimm_btt2_guid;
static guid_t nvdimm_pfn_guid;
static guid_t nvdimm_dax_guid;

static uuid_t nvdimm_btt_uuid;
static uuid_t nvdimm_btt2_uuid;
static uuid_t nvdimm_pfn_uuid;
static uuid_t nvdimm_dax_uuid;

uuid_t cxl_region_uuid;
uuid_t cxl_namespace_uuid;

static const char NSINDEX_SIGNATURE[] = "NAMESPACE_INDEX\0";

static u32 best_seq(u32 a, u32 b)
{
	a &= NSINDEX_SEQ_MASK;
	b &= NSINDEX_SEQ_MASK;

	if (a == 0 || a == b)
		return b;
	else if (b == 0)
		return a;
	else if (nd_inc_seq(a) == b)
		return b;
	else
		return a;
}

unsigned sizeof_namespace_label(struct nvdimm_drvdata *ndd)
{
	return ndd->nslabel_size;
}

static size_t __sizeof_namespace_index(u32 nslot)
{
	return ALIGN(sizeof(struct nd_namespace_index) + DIV_ROUND_UP(nslot, 8),
			NSINDEX_ALIGN);
}

static int __nvdimm_num_label_slots(struct nvdimm_drvdata *ndd,
		size_t index_size)
{
	return (ndd->nsarea.config_size - index_size * 2) /
			sizeof_namespace_label(ndd);
}

int nvdimm_num_label_slots(struct nvdimm_drvdata *ndd)
{
	u32 tmp_nslot, n;

	tmp_nslot = ndd->nsarea.config_size / sizeof_namespace_label(ndd);
	n = __sizeof_namespace_index(tmp_nslot) / NSINDEX_ALIGN;

	return __nvdimm_num_label_slots(ndd, NSINDEX_ALIGN * n);
}

size_t sizeof_namespace_index(struct nvdimm_drvdata *ndd)
{
	u32 nslot, space, size;

	/*
	 * Per UEFI 2.7, the minimum size of the Label Storage Area is large
	 * enough to hold 2 index blocks and 2 labels.  The minimum index
	 * block size is 256 bytes. The label size is 128 for namespaces
	 * prior to version 1.2 and at minimum 256 for version 1.2 and later.
	 */
	nslot = nvdimm_num_label_slots(ndd);
	space = ndd->nsarea.config_size - nslot * sizeof_namespace_label(ndd);
	size = __sizeof_namespace_index(nslot) * 2;
	if (size <= space && nslot >= 2)
		return size / 2;

	dev_err(ndd->dev, "label area (%d) too small to host (%d byte) labels\n",
			ndd->nsarea.config_size, sizeof_namespace_label(ndd));
	return 0;
}

static int __nd_label_validate(struct nvdimm_drvdata *ndd)
{
	/*
	 * On media label format consists of two index blocks followed
	 * by an array of labels.  None of these structures are ever
	 * updated in place.  A sequence number tracks the current
	 * active index and the next one to write, while labels are
	 * written to free slots.
	 *
	 *     +------------+
	 *     |            |
	 *     |  nsindex0  |
	 *     |            |
	 *     +------------+
	 *     |            |
	 *     |  nsindex1  |
	 *     |            |
	 *     +------------+
	 *     |   label0   |
	 *     +------------+
	 *     |   label1   |
	 *     +------------+
	 *     |            |
	 *      ....nslot...
	 *     |            |
	 *     +------------+
	 *     |   labelN   |
	 *     +------------+
	 */
	struct nd_namespace_index *nsindex[] = {
		to_namespace_index(ndd, 0),
		to_namespace_index(ndd, 1),
	};
	const int num_index = ARRAY_SIZE(nsindex);
	struct device *dev = ndd->dev;
	bool valid[2] = { 0 };
	int i, num_valid = 0;
	u32 seq;

	for (i = 0; i < num_index; i++) {
		u32 nslot;
		u8 sig[NSINDEX_SIG_LEN];
		u64 sum_save, sum, size;
		unsigned int version, labelsize;

		memcpy(sig, nsindex[i]->sig, NSINDEX_SIG_LEN);
		if (memcmp(sig, NSINDEX_SIGNATURE, NSINDEX_SIG_LEN) != 0) {
			dev_dbg(dev, "nsindex%d signature invalid\n", i);
			continue;
		}

		/* label sizes larger than 128 arrived with v1.2 */
		version = __le16_to_cpu(nsindex[i]->major) * 100
			+ __le16_to_cpu(nsindex[i]->minor);
		if (version >= 102)
			labelsize = 1 << (7 + nsindex[i]->labelsize);
		else
			labelsize = 128;

		if (labelsize != sizeof_namespace_label(ndd)) {
			dev_dbg(dev, "nsindex%d labelsize %d invalid\n",
					i, nsindex[i]->labelsize);
			continue;
		}

		sum_save = __le64_to_cpu(nsindex[i]->checksum);
		nsindex[i]->checksum = __cpu_to_le64(0);
		sum = nd_fletcher64(nsindex[i], sizeof_namespace_index(ndd), 1);
		nsindex[i]->checksum = __cpu_to_le64(sum_save);
		if (sum != sum_save) {
			dev_dbg(dev, "nsindex%d checksum invalid\n", i);
			continue;
		}

		seq = __le32_to_cpu(nsindex[i]->seq);
		if ((seq & NSINDEX_SEQ_MASK) == 0) {
			dev_dbg(dev, "nsindex%d sequence: %#x invalid\n", i, seq);
			continue;
		}

		/* sanity check the index against expected values */
		if (__le64_to_cpu(nsindex[i]->myoff)
				!= i * sizeof_namespace_index(ndd)) {
			dev_dbg(dev, "nsindex%d myoff: %#llx invalid\n",
					i, (unsigned long long)
					__le64_to_cpu(nsindex[i]->myoff));
			continue;
		}
		if (__le64_to_cpu(nsindex[i]->otheroff)
				!= (!i) * sizeof_namespace_index(ndd)) {
			dev_dbg(dev, "nsindex%d otheroff: %#llx invalid\n",
					i, (unsigned long long)
					__le64_to_cpu(nsindex[i]->otheroff));
			continue;
		}
		if (__le64_to_cpu(nsindex[i]->labeloff)
				!= 2 * sizeof_namespace_index(ndd)) {
			dev_dbg(dev, "nsindex%d labeloff: %#llx invalid\n",
					i, (unsigned long long)
					__le64_to_cpu(nsindex[i]->labeloff));
			continue;
		}

		size = __le64_to_cpu(nsindex[i]->mysize);
		if (size > sizeof_namespace_index(ndd)
				|| size < sizeof(struct nd_namespace_index)) {
			dev_dbg(dev, "nsindex%d mysize: %#llx invalid\n", i, size);
			continue;
		}

		nslot = __le32_to_cpu(nsindex[i]->nslot);
		if (nslot * sizeof_namespace_label(ndd)
				+ 2 * sizeof_namespace_index(ndd)
				> ndd->nsarea.config_size) {
			dev_dbg(dev, "nsindex%d nslot: %u invalid, config_size: %#x\n",
					i, nslot, ndd->nsarea.config_size);
			continue;
		}
		valid[i] = true;
		num_valid++;
	}

	switch (num_valid) {
	case 0:
		break;
	case 1:
		for (i = 0; i < num_index; i++)
			if (valid[i])
				return i;
		/* can't have num_valid > 0 but valid[] = { false, false } */
		WARN_ON(1);
		break;
	default:
		/* pick the best index... */
		seq = best_seq(__le32_to_cpu(nsindex[0]->seq),
				__le32_to_cpu(nsindex[1]->seq));
		if (seq == (__le32_to_cpu(nsindex[1]->seq) & NSINDEX_SEQ_MASK))
			return 1;
		else
			return 0;
		break;
	}

	return -1;
}

static int nd_label_validate(struct nvdimm_drvdata *ndd)
{
	/*
	 * In order to probe for and validate namespace index blocks we
	 * need to know the size of the labels, and we can't trust the
	 * size of the labels until we validate the index blocks.
	 * Resolve this dependency loop by probing for known label
	 * sizes, but default to v1.2 256-byte namespace labels if
	 * discovery fails.
	 */
	int label_size[] = { 128, 256 };
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(label_size); i++) {
		ndd->nslabel_size = label_size[i];
		rc = __nd_label_validate(ndd);
		if (rc >= 0)
			return rc;
	}

	return -1;
}

static void nd_label_copy(struct nvdimm_drvdata *ndd,
			  struct nd_namespace_index *dst,
			  struct nd_namespace_index *src)
{
	/* just exit if either destination or source is NULL */
	if (!dst || !src)
		return;

	memcpy(dst, src, sizeof_namespace_index(ndd));
}

static struct nd_namespace_label *nd_label_base(struct nvdimm_drvdata *ndd)
{
	void *base = to_namespace_index(ndd, 0);

	return base + 2 * sizeof_namespace_index(ndd);
}

static unsigned long find_slot(struct nvdimm_drvdata *ndd,
			       unsigned long label)
{
	unsigned long base;

	base = (unsigned long)nd_label_base(ndd);
	return (label - base) / sizeof_namespace_label(ndd);
}

static int to_lsa_slot(struct nvdimm_drvdata *ndd,
		       union nd_lsa_label *lsa_label)
{
	return find_slot(ndd, (unsigned long) lsa_label);
}

static int to_slot(struct nvdimm_drvdata *ndd,
		   struct nd_label_ent *label_ent)
{
	if (uuid_equal(&cxl_region_uuid, &label_ent->label_uuid))
		return find_slot(ndd, (unsigned long) label_ent->region_label);
	else
		return find_slot(ndd, (unsigned long) label_ent->label);
}

static union nd_lsa_label *to_lsa_label(struct nvdimm_drvdata *ndd, int slot)
{
	unsigned long label, base;

	base = (unsigned long) nd_label_base(ndd);
	label = base + sizeof_namespace_label(ndd) * slot;

	return (union nd_lsa_label *) label;
}

#define for_each_clear_bit_le(bit, addr, size) \
	for ((bit) = find_next_zero_bit_le((addr), (size), 0);  \
	     (bit) < (size);                                    \
	     (bit) = find_next_zero_bit_le((addr), (size), (bit) + 1))

/**
 * preamble_index - common variable initialization for nd_label_* routines
 * @ndd: dimm container for the relevant label set
 * @idx: namespace_index index
 * @nsindex_out: on return set to the currently active namespace index
 * @free: on return set to the free label bitmap in the index
 * @nslot: on return set to the number of slots in the label space
 */
static bool preamble_index(struct nvdimm_drvdata *ndd, int idx,
		struct nd_namespace_index **nsindex_out,
		unsigned long **free, u32 *nslot)
{
	struct nd_namespace_index *nsindex;

	nsindex = to_namespace_index(ndd, idx);
	if (nsindex == NULL)
		return false;

	*free = (unsigned long *) nsindex->free;
	*nslot = __le32_to_cpu(nsindex->nslot);
	*nsindex_out = nsindex;

	return true;
}

char *nd_label_gen_id(struct nd_label_id *label_id, const uuid_t *uuid,
		      u32 flags)
{
	if (!label_id || !uuid)
		return NULL;
	snprintf(label_id->id, ND_LABEL_ID_SIZE, "pmem-%pUb", uuid);
	return label_id->id;
}

static bool preamble_current(struct nvdimm_drvdata *ndd,
		struct nd_namespace_index **nsindex,
		unsigned long **free, u32 *nslot)
{
	return preamble_index(ndd, ndd->ns_current, nsindex,
			free, nslot);
}

static bool preamble_next(struct nvdimm_drvdata *ndd,
		struct nd_namespace_index **nsindex,
		unsigned long **free, u32 *nslot)
{
	return preamble_index(ndd, ndd->ns_next, nsindex,
			free, nslot);
}

static bool nsl_validate_checksum(struct nvdimm_drvdata *ndd,
				  struct nd_namespace_label *nd_label)
{
	u64 sum, sum_save;

	if (!efi_namespace_label_has(ndd, checksum))
		return true;

	sum_save = nsl_get_checksum(ndd, nd_label);
	nsl_set_checksum(ndd, nd_label, 0);
	sum = nd_fletcher64(nd_label, sizeof_namespace_label(ndd), 1);
	nsl_set_checksum(ndd, nd_label, sum_save);
	return sum == sum_save;
}

static void nsl_calculate_checksum(struct nvdimm_drvdata *ndd,
				   struct nd_namespace_label *nd_label)
{
	u64 sum;

	if (!efi_namespace_label_has(ndd, checksum))
		return;
	nsl_set_checksum(ndd, nd_label, 0);
	sum = nd_fletcher64(nd_label, sizeof_namespace_label(ndd), 1);
	nsl_set_checksum(ndd, nd_label, sum);
}

static bool region_label_validate_checksum(struct nvdimm_drvdata *ndd,
				struct cxl_region_label *region_label)
{
	u64 sum, sum_save;

	sum_save = __le64_to_cpu(region_label->checksum);
	region_label->checksum = __cpu_to_le64(0);
	sum = nd_fletcher64(region_label, sizeof_namespace_label(ndd), 1);
	region_label->checksum = __cpu_to_le64(sum_save);
	return sum == sum_save;
}

static void region_label_calculate_checksum(struct nvdimm_drvdata *ndd,
				   struct cxl_region_label *region_label)
{
	u64 sum;

	region_label->checksum = __cpu_to_le64(0);
	sum = nd_fletcher64(region_label, sizeof_namespace_label(ndd), 1);
	region_label->checksum = __cpu_to_le64(sum);
}

static bool slot_valid(struct nvdimm_drvdata *ndd,
		       union nd_lsa_label *lsa_label, u32 slot)
{
	struct cxl_region_label *region_label;
	struct nd_namespace_label *nd_label;
	enum label_type type;
	bool valid;
	static const char * const label_name[] = {
		[RG_LABEL_TYPE] = "region",
		[NS_LABEL_TYPE] = "namespace",
	};

	/* check that we are written where we expect to be written */
	if (is_region_label(ndd, lsa_label)) {
		region_label = &lsa_label->region_label;
		type = RG_LABEL_TYPE;
		if (slot != __le32_to_cpu(region_label->slot))
			return false;
		valid = region_label_validate_checksum(ndd, region_label);
	} else {
		nd_label = &lsa_label->ns_label;
		type = NS_LABEL_TYPE;
		if (slot != nsl_get_slot(ndd, nd_label))
			return false;
		valid = nsl_validate_checksum(ndd, nd_label);
	}

	if (!valid)
		dev_dbg(ndd->dev, "%s label checksum fail. slot: %d\n",
			label_name[type], slot);

	return valid;
}

int nd_label_reserve_dpa(struct nvdimm_drvdata *ndd)
{
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;

	if (!preamble_current(ndd, &nsindex, &free, &nslot))
		return 0; /* no label, nothing to reserve */

	for_each_clear_bit_le(slot, free, nslot) {
		struct nd_namespace_label *nd_label;
		struct nd_region *nd_region = NULL;
		union nd_lsa_label *lsa_label;
		struct nd_label_id label_id;
		struct resource *res;
		uuid_t label_uuid;
		u32 flags;

		lsa_label = to_lsa_label(ndd, slot);
		nd_label = &lsa_label->ns_label;

		if (!slot_valid(ndd, lsa_label, slot))
			continue;

		nsl_get_uuid(ndd, nd_label, &label_uuid);
		flags = nsl_get_flags(ndd, nd_label);
		nd_label_gen_id(&label_id, &label_uuid, flags);
		res = nvdimm_allocate_dpa(ndd, &label_id,
					  nsl_get_dpa(ndd, nd_label),
					  nsl_get_rawsize(ndd, nd_label));
		nd_dbg_dpa(nd_region, ndd, res, "reserve\n");
		if (!res)
			return -EBUSY;
	}

	return 0;
}

int nd_label_data_init(struct nvdimm_drvdata *ndd)
{
	size_t config_size, read_size, max_xfer, offset;
	struct nd_namespace_index *nsindex;
	unsigned int i;
	int rc = 0;
	u32 nslot;

	if (ndd->data)
		return 0;

	if (ndd->nsarea.status || ndd->nsarea.max_xfer == 0 ||
	    ndd->nsarea.config_size == 0) {
		dev_dbg(ndd->dev, "failed to init config data area: (%u:%u)\n",
			ndd->nsarea.max_xfer, ndd->nsarea.config_size);
		return -ENXIO;
	}

	/*
	 * We need to determine the maximum index area as this is the section
	 * we must read and validate before we can start processing labels.
	 *
	 * If the area is too small to contain the two indexes and 2 labels
	 * then we abort.
	 *
	 * Start at a label size of 128 as this should result in the largest
	 * possible namespace index size.
	 */
	ndd->nslabel_size = 128;
	read_size = sizeof_namespace_index(ndd) * 2;
	if (!read_size)
		return -ENXIO;

	/* Allocate config data */
	config_size = ndd->nsarea.config_size;
	ndd->data = kvzalloc(config_size, GFP_KERNEL);
	if (!ndd->data)
		return -ENOMEM;

	/*
	 * We want to guarantee as few reads as possible while conserving
	 * memory. To do that we figure out how much unused space will be left
	 * in the last read, divide that by the total number of reads it is
	 * going to take given our maximum transfer size, and then reduce our
	 * maximum transfer size based on that result.
	 */
	max_xfer = min_t(size_t, ndd->nsarea.max_xfer, config_size);
	if (read_size < max_xfer) {
		/* trim waste */
		max_xfer -= ((max_xfer - 1) - (config_size - 1) % max_xfer) /
			    DIV_ROUND_UP(config_size, max_xfer);
		/* make certain we read indexes in exactly 1 read */
		if (max_xfer < read_size)
			max_xfer = read_size;
	}

	/* Make our initial read size a multiple of max_xfer size */
	read_size = min(DIV_ROUND_UP(read_size, max_xfer) * max_xfer,
			config_size);

	/* Read the index data */
	rc = nvdimm_get_config_data(ndd, ndd->data, 0, read_size);
	if (rc)
		goto out_err;

	/* Validate index data, if not valid assume all labels are invalid */
	ndd->ns_current = nd_label_validate(ndd);
	if (ndd->ns_current < 0)
		return 0;

	/* Record our index values */
	ndd->ns_next = nd_label_next_nsindex(ndd->ns_current);

	/* Copy "current" index on top of the "next" index */
	nsindex = to_current_namespace_index(ndd);
	nd_label_copy(ndd, to_next_namespace_index(ndd), nsindex);

	/* Determine starting offset for label data */
	offset = __le64_to_cpu(nsindex->labeloff);
	nslot = __le32_to_cpu(nsindex->nslot);

	/* Loop through the free list pulling in any active labels */
	for (i = 0; i < nslot; i++, offset += ndd->nslabel_size) {
		size_t label_read_size;

		/* zero out the unused labels */
		if (test_bit_le(i, nsindex->free)) {
			memset(ndd->data + offset, 0, ndd->nslabel_size);
			continue;
		}

		/* if we already read past here then just continue */
		if (offset + ndd->nslabel_size <= read_size)
			continue;

		/* if we haven't read in a while reset our read_size offset */
		if (read_size < offset)
			read_size = offset;

		/* determine how much more will be read after this next call. */
		label_read_size = offset + ndd->nslabel_size - read_size;
		label_read_size = DIV_ROUND_UP(label_read_size, max_xfer) *
				  max_xfer;

		/* truncate last read if needed */
		if (read_size + label_read_size > config_size)
			label_read_size = config_size - read_size;

		/* Read the label data */
		rc = nvdimm_get_config_data(ndd, ndd->data + read_size,
					    read_size, label_read_size);
		if (rc)
			goto out_err;

		/* push read_size to next read offset */
		read_size += label_read_size;
	}

	dev_dbg(ndd->dev, "len: %zu rc: %d\n", offset, rc);
out_err:
	return rc;
}

int nd_label_active_count(struct nvdimm_drvdata *ndd)
{
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;
	int count = 0;

	if (!preamble_current(ndd, &nsindex, &free, &nslot))
		return 0;

	for_each_clear_bit_le(slot, free, nslot) {
		struct cxl_region_label *region_label;
		struct nd_namespace_label *nd_label;
		union nd_lsa_label *lsa_label;
		u32 label_slot;
		u64 size, dpa;

		lsa_label = to_lsa_label(ndd, slot);

		if (!slot_valid(ndd, lsa_label, slot)) {
			if (is_region_label(ndd, lsa_label)) {
				region_label = &lsa_label->region_label;
				label_slot = __le32_to_cpu(region_label->slot);
				size = __le64_to_cpu(region_label->rawsize);
				dpa = __le64_to_cpu(region_label->dpa);
			} else {
				nd_label = &lsa_label->ns_label;
				label_slot = nsl_get_slot(ndd, nd_label);
				size = nsl_get_rawsize(ndd, nd_label);
				dpa = nsl_get_dpa(ndd, nd_label);
			}

			dev_dbg(ndd->dev,
				"slot%d invalid slot: %d dpa: %llx size: %llx\n",
					slot, label_slot, dpa, size);
			continue;
		}
		count++;
	}
	return count;
}

union nd_lsa_label *nd_label_active(struct nvdimm_drvdata *ndd, int n)
{
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;

	if (!preamble_current(ndd, &nsindex, &free, &nslot))
		return NULL;

	for_each_clear_bit_le(slot, free, nslot) {
		union nd_lsa_label *lsa_label;

		lsa_label = to_lsa_label(ndd, slot);
		if (!slot_valid(ndd, lsa_label, slot))
			continue;

		if (n-- == 0)
			return to_lsa_label(ndd, slot);
	}

	return NULL;
}

u32 nd_label_alloc_slot(struct nvdimm_drvdata *ndd)
{
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;

	if (!preamble_next(ndd, &nsindex, &free, &nslot))
		return UINT_MAX;

	WARN_ON(!is_nvdimm_bus_locked(ndd->dev));

	slot = find_next_bit_le(free, nslot, 0);
	if (slot == nslot)
		return UINT_MAX;

	clear_bit_le(slot, free);

	return slot;
}

bool nd_label_free_slot(struct nvdimm_drvdata *ndd, u32 slot)
{
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot;

	if (!preamble_next(ndd, &nsindex, &free, &nslot))
		return false;

	WARN_ON(!is_nvdimm_bus_locked(ndd->dev));

	if (slot < nslot)
		return !test_and_set_bit_le(slot, free);
	return false;
}

u32 nd_label_nfree(struct nvdimm_drvdata *ndd)
{
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot;

	WARN_ON(!is_nvdimm_bus_locked(ndd->dev));

	if (!preamble_next(ndd, &nsindex, &free, &nslot))
		return nvdimm_num_label_slots(ndd);

	return bitmap_weight(free, nslot);
}

static int nd_label_write_index(struct nvdimm_drvdata *ndd, int index, u32 seq,
		unsigned long flags)
{
	struct nd_namespace_index *nsindex;
	unsigned long offset;
	u64 checksum;
	u32 nslot;
	int rc;

	nsindex = to_namespace_index(ndd, index);
	if (flags & ND_NSINDEX_INIT)
		nslot = nvdimm_num_label_slots(ndd);
	else
		nslot = __le32_to_cpu(nsindex->nslot);

	memcpy(nsindex->sig, NSINDEX_SIGNATURE, NSINDEX_SIG_LEN);
	memset(&nsindex->flags, 0, 3);
	nsindex->labelsize = sizeof_namespace_label(ndd) >> 8;
	nsindex->seq = __cpu_to_le32(seq);
	offset = (unsigned long) nsindex
		- (unsigned long) to_namespace_index(ndd, 0);
	nsindex->myoff = __cpu_to_le64(offset);
	nsindex->mysize = __cpu_to_le64(sizeof_namespace_index(ndd));
	offset = (unsigned long) to_namespace_index(ndd,
			nd_label_next_nsindex(index))
		- (unsigned long) to_namespace_index(ndd, 0);
	nsindex->otheroff = __cpu_to_le64(offset);
	offset = (unsigned long) nd_label_base(ndd)
		- (unsigned long) to_namespace_index(ndd, 0);
	nsindex->labeloff = __cpu_to_le64(offset);
	nsindex->nslot = __cpu_to_le32(nslot);

	/* Set LSA Label Index Version */
	if (ndd->cxl) {
		/* CXL r3.2: Table 9-9 Label Index Block Layout */
		nsindex->major = __cpu_to_le16(2);
		nsindex->minor = __cpu_to_le16(1);
	} else {
		nsindex->major = __cpu_to_le16(1);
		/*
		 * NVDIMM Namespace Specification
		 * Table 2: Namespace Label Index Block Fields
		 */
		if (sizeof_namespace_label(ndd) < 256)
			nsindex->minor = __cpu_to_le16(1);
		else /* UEFI 2.7: Label Index Block Definitions */
			nsindex->minor = __cpu_to_le16(2);
	}

	nsindex->checksum = __cpu_to_le64(0);
	if (flags & ND_NSINDEX_INIT) {
		unsigned long *free = (unsigned long *) nsindex->free;
		u32 nfree = ALIGN(nslot, BITS_PER_LONG);
		int last_bits, i;

		memset(nsindex->free, 0xff, nfree / 8);
		for (i = 0, last_bits = nfree - nslot; i < last_bits; i++)
			clear_bit_le(nslot + i, free);
	}
	checksum = nd_fletcher64(nsindex, sizeof_namespace_index(ndd), 1);
	nsindex->checksum = __cpu_to_le64(checksum);
	rc = nvdimm_set_config_data(ndd, __le64_to_cpu(nsindex->myoff),
			nsindex, sizeof_namespace_index(ndd));
	if (rc < 0)
		return rc;

	if (flags & ND_NSINDEX_INIT)
		return 0;

	/* copy the index we just wrote to the new 'next' */
	WARN_ON(index != ndd->ns_next);
	nd_label_copy(ndd, to_current_namespace_index(ndd), nsindex);
	ndd->ns_current = nd_label_next_nsindex(ndd->ns_current);
	ndd->ns_next = nd_label_next_nsindex(ndd->ns_next);
	WARN_ON(ndd->ns_current == ndd->ns_next);

	return 0;
}

static unsigned long nd_label_offset(struct nvdimm_drvdata *ndd,
		union nd_lsa_label *lsa_label)
{
	return (unsigned long) lsa_label
		- (unsigned long) to_namespace_index(ndd, 0);
}

static enum nvdimm_claim_class guid_to_nvdimm_cclass(guid_t *guid)
{
	if (guid_equal(guid, &nvdimm_btt_guid))
		return NVDIMM_CCLASS_BTT;
	else if (guid_equal(guid, &nvdimm_btt2_guid))
		return NVDIMM_CCLASS_BTT2;
	else if (guid_equal(guid, &nvdimm_pfn_guid))
		return NVDIMM_CCLASS_PFN;
	else if (guid_equal(guid, &nvdimm_dax_guid))
		return NVDIMM_CCLASS_DAX;
	else if (guid_equal(guid, &guid_null))
		return NVDIMM_CCLASS_NONE;

	return NVDIMM_CCLASS_UNKNOWN;
}

/* CXL labels store UUIDs instead of GUIDs for the same data */
static enum nvdimm_claim_class uuid_to_nvdimm_cclass(uuid_t *uuid)
{
	if (uuid_equal(uuid, &nvdimm_btt_uuid))
		return NVDIMM_CCLASS_BTT;
	else if (uuid_equal(uuid, &nvdimm_btt2_uuid))
		return NVDIMM_CCLASS_BTT2;
	else if (uuid_equal(uuid, &nvdimm_pfn_uuid))
		return NVDIMM_CCLASS_PFN;
	else if (uuid_equal(uuid, &nvdimm_dax_uuid))
		return NVDIMM_CCLASS_DAX;
	else if (uuid_equal(uuid, &uuid_null))
		return NVDIMM_CCLASS_NONE;

	return NVDIMM_CCLASS_UNKNOWN;
}

static const guid_t *to_abstraction_guid(enum nvdimm_claim_class claim_class,
	guid_t *target)
{
	if (claim_class == NVDIMM_CCLASS_BTT)
		return &nvdimm_btt_guid;
	else if (claim_class == NVDIMM_CCLASS_BTT2)
		return &nvdimm_btt2_guid;
	else if (claim_class == NVDIMM_CCLASS_PFN)
		return &nvdimm_pfn_guid;
	else if (claim_class == NVDIMM_CCLASS_DAX)
		return &nvdimm_dax_guid;
	else if (claim_class == NVDIMM_CCLASS_UNKNOWN) {
		/*
		 * If we're modifying a namespace for which we don't
		 * know the claim_class, don't touch the existing guid.
		 */
		return target;
	} else
		return &guid_null;
}

/* CXL labels store UUIDs instead of GUIDs for the same data */
static const uuid_t *to_abstraction_uuid(enum nvdimm_claim_class claim_class,
					 uuid_t *target)
{
	if (claim_class == NVDIMM_CCLASS_BTT)
		return &nvdimm_btt_uuid;
	else if (claim_class == NVDIMM_CCLASS_BTT2)
		return &nvdimm_btt2_uuid;
	else if (claim_class == NVDIMM_CCLASS_PFN)
		return &nvdimm_pfn_uuid;
	else if (claim_class == NVDIMM_CCLASS_DAX)
		return &nvdimm_dax_uuid;
	else if (claim_class == NVDIMM_CCLASS_UNKNOWN) {
		/*
		 * If we're modifying a namespace for which we don't
		 * know the claim_class, don't touch the existing uuid.
		 */
		return target;
	} else
		return &uuid_null;
}

static void reap_victim(struct nd_mapping *nd_mapping,
		struct nd_label_ent *victim)
{
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	u32 slot = to_slot(ndd, victim);

	dev_dbg(ndd->dev, "free: %d\n", slot);
	nd_label_free_slot(ndd, slot);

	if (uuid_equal(&cxl_region_uuid, &victim->label_uuid))
		victim->region_label = NULL;
	else
		victim->label = NULL;
}

static void nsl_set_type_guid(struct nvdimm_drvdata *ndd,
			      struct nd_namespace_label *nd_label, guid_t *guid)
{
	if (efi_namespace_label_has(ndd, type_guid))
		guid_copy(&nd_label->efi.type_guid, guid);
}

bool nsl_validate_type_guid(struct nvdimm_drvdata *ndd,
			    struct nd_namespace_label *nd_label, guid_t *guid)
{
	if (ndd->cxl || !efi_namespace_label_has(ndd, type_guid))
		return true;
	if (!guid_equal(&nd_label->efi.type_guid, guid)) {
		dev_dbg(ndd->dev, "expect type_guid %pUb got %pUb\n", guid,
			&nd_label->efi.type_guid);
		return false;
	}
	return true;
}

static void nsl_set_claim_class(struct nvdimm_drvdata *ndd,
				struct nd_namespace_label *nd_label,
				enum nvdimm_claim_class claim_class)
{
	if (ndd->cxl) {
		uuid_t uuid;

		import_uuid(&uuid, nd_label->cxl.abstraction_uuid);
		export_uuid(nd_label->cxl.abstraction_uuid,
			    to_abstraction_uuid(claim_class, &uuid));
		return;
	}

	if (!efi_namespace_label_has(ndd, abstraction_guid))
		return;
	guid_copy(&nd_label->efi.abstraction_guid,
		  to_abstraction_guid(claim_class,
				      &nd_label->efi.abstraction_guid));
}

enum nvdimm_claim_class nsl_get_claim_class(struct nvdimm_drvdata *ndd,
					    struct nd_namespace_label *nd_label)
{
	if (ndd->cxl) {
		uuid_t uuid;

		import_uuid(&uuid, nd_label->cxl.abstraction_uuid);
		return uuid_to_nvdimm_cclass(&uuid);
	}
	if (!efi_namespace_label_has(ndd, abstraction_guid))
		return NVDIMM_CCLASS_NONE;
	return guid_to_nvdimm_cclass(&nd_label->efi.abstraction_guid);
}

static int namespace_label_update(struct nd_region *nd_region,
				  struct nd_mapping *nd_mapping,
				  struct nd_namespace_pmem *nspm,
				  int pos, u64 flags,
				  struct nd_namespace_label *ns_label,
				  struct nd_namespace_index *nsindex,
				  u32 slot)
{
	struct nd_namespace_common *ndns = &nspm->nsio.common;
	struct nd_interleave_set *nd_set = nd_region->nd_set;
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct nd_label_id label_id;
	struct resource *res;
	u64 cookie;

	cookie = nd_region_interleave_set_cookie(nd_region, nsindex);
	nd_label_gen_id(&label_id, nspm->uuid, 0);
	for_each_dpa_resource(ndd, res)
		if (strcmp(res->name, label_id.id) == 0)
			break;

	if (!res) {
		WARN_ON_ONCE(1);
		return -ENXIO;
	}

	nsl_set_type(ndd, ns_label);
	nsl_set_uuid(ndd, ns_label, nspm->uuid);
	nsl_set_name(ndd, ns_label, nspm->alt_name);
	nsl_set_flags(ndd, ns_label, flags);
	nsl_set_nlabel(ndd, ns_label, nd_region->ndr_mappings);
	nsl_set_nrange(ndd, ns_label, 1);
	nsl_set_position(ndd, ns_label, pos);
	nsl_set_isetcookie(ndd, ns_label, cookie);
	nsl_set_rawsize(ndd, ns_label, resource_size(res));
	nsl_set_lbasize(ndd, ns_label, nspm->lbasize);
	nsl_set_dpa(ndd, ns_label, res->start);
	nsl_set_slot(ndd, ns_label, slot);
	nsl_set_alignment(ndd, ns_label, 0);
	nsl_set_type_guid(ndd, ns_label, &nd_set->type_guid);
	nsl_set_region_uuid(ndd, ns_label, &nd_set->uuid);
	nsl_set_claim_class(ndd, ns_label, ndns->claim_class);
	nsl_calculate_checksum(ndd, ns_label);
	nd_dbg_dpa(nd_region, ndd, res, "\n");

	return 0;
}

static void region_label_update(struct nd_region *nd_region,
				struct cxl_region_label *region_label,
				struct nd_mapping *nd_mapping,
				int pos, u64 flags, u32 slot)
{
	struct nd_interleave_set *nd_set = nd_region->nd_set;
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);

	/* Set Region Label Format identification UUID */
	export_uuid(region_label->type, &cxl_region_uuid);

	/* Set Current Region Label UUID */
	export_uuid(region_label->uuid, &nd_set->uuid);

	region_label->flags = __cpu_to_le32(flags);
	region_label->nlabel = __cpu_to_le16(nd_region->ndr_mappings);
	region_label->position = __cpu_to_le16(pos);
	region_label->dpa = __cpu_to_le64(nd_mapping->start);
	region_label->rawsize = __cpu_to_le64(nd_mapping->size);
	region_label->hpa = __cpu_to_le64(nd_set->res->start);
	region_label->slot = __cpu_to_le32(slot);
	region_label->ig = __cpu_to_le32(nd_set->interleave_granularity);
	region_label->align = __cpu_to_le32(0);

	/* Update fletcher64 Checksum */
	region_label_calculate_checksum(ndd, region_label);
}

static bool is_label_reapable(struct nd_interleave_set *nd_set,
			       struct nd_namespace_pmem *nspm,
			       struct nvdimm_drvdata *ndd,
			       struct nd_label_ent *label_ent,
			       enum label_type ltype,
			       unsigned long *flags)
{
	switch (ltype) {
	case NS_LABEL_TYPE:
		if (test_and_clear_bit(ND_LABEL_REAP, flags) ||
		    nsl_uuid_equal(ndd, label_ent->label, nspm->uuid))
			return true;

		break;
	case RG_LABEL_TYPE:
		if (region_label_uuid_equal(label_ent->region_label,
		    &nd_set->uuid))
			return true;

		break;
	}

	return false;
}

static bool is_label_present(struct nd_label_ent *label_ent,
			     enum label_type ltype)
{
	switch (ltype) {
	case NS_LABEL_TYPE:
		if (label_ent->label)
			return true;

		break;
	case RG_LABEL_TYPE:
		if (label_ent->region_label)
			return true;

		break;
	}

	return false;
}

static int __pmem_label_update(struct nd_region *nd_region,
			       struct nd_mapping *nd_mapping,
			       struct nd_namespace_pmem *nspm,
			       int pos, unsigned long flags,
			       enum label_type ltype)
{
	struct nd_interleave_set *nd_set = nd_region->nd_set;
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct nd_namespace_index *nsindex;
	struct nd_label_ent *label_ent;
	union nd_lsa_label *lsa_label;
	unsigned long *free;
	struct device *dev;
	u32 nslot, slot;
	size_t offset;
	int rc;

	if (!preamble_next(ndd, &nsindex, &free, &nslot))
		return -ENXIO;

	/* allocate and write the label to the staging (next) index */
	slot = nd_label_alloc_slot(ndd);
	if (slot == UINT_MAX)
		return -ENXIO;
	dev_dbg(ndd->dev, "allocated: %d\n", slot);

	lsa_label = to_lsa_label(ndd, slot);
	memset(lsa_label, 0, sizeof_namespace_label(ndd));

	switch (ltype) {
	case NS_LABEL_TYPE:
		dev = &nspm->nsio.common.dev;
		rc = namespace_label_update(nd_region, nd_mapping,
				nspm, pos, flags, &lsa_label->ns_label,
				nsindex, slot);
		if (rc)
			return rc;

		break;
	case RG_LABEL_TYPE:
		dev = &nd_region->dev;
		region_label_update(nd_region, &lsa_label->region_label,
				nd_mapping, pos, flags, slot);

		break;
	}

	/* update label */
	offset = nd_label_offset(ndd, lsa_label);
	rc = nvdimm_set_config_data(ndd, offset, lsa_label,
			sizeof_namespace_label(ndd));
	if (rc < 0)
		return rc;

	/* Garbage collect the previous label */
	mutex_lock(&nd_mapping->lock);
	list_for_each_entry(label_ent, &nd_mapping->labels, list) {
		if (!is_label_present(label_ent, ltype))
			continue;

		if (is_label_reapable(nd_set, nspm, ndd, label_ent, ltype,
					&label_ent->flags))
			reap_victim(nd_mapping, label_ent);
	}

	/* update index */
	rc = nd_label_write_index(ndd, ndd->ns_next,
			nd_inc_seq(__le32_to_cpu(nsindex->seq)), 0);
	if (rc == 0) {
		list_for_each_entry(label_ent, &nd_mapping->labels, list) {
			if (is_label_present(label_ent, ltype))
				continue;

			if (ltype == NS_LABEL_TYPE)
				label_ent->label = &lsa_label->ns_label;
			else
				label_ent->region_label = &lsa_label->region_label;

			lsa_label = NULL;
			break;
		}
		dev_WARN_ONCE(dev, lsa_label, "failed to track label: %d\n",
			      to_lsa_slot(ndd, lsa_label));
		if (lsa_label)
			rc = -ENXIO;
	}
	mutex_unlock(&nd_mapping->lock);

	return rc;
}

static int init_labels(struct nd_mapping *nd_mapping, int num_labels,
		       enum label_type ltype)
{
	int i, old_num_labels = 0;
	struct nd_label_ent *label_ent;
	struct nd_namespace_index *nsindex;
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);

	mutex_lock(&nd_mapping->lock);
	list_for_each_entry(label_ent, &nd_mapping->labels, list)
		old_num_labels++;
	mutex_unlock(&nd_mapping->lock);

	/*
	 * We need to preserve all the old labels for the mapping so
	 * they can be garbage collected after writing the new labels.
	 */
	for (i = old_num_labels; i < num_labels; i++) {
		label_ent = kzalloc(sizeof(*label_ent), GFP_KERNEL);
		if (!label_ent)
			return -ENOMEM;

		switch (ltype) {
		case NS_LABEL_TYPE:
			uuid_copy(&label_ent->label_uuid, &cxl_namespace_uuid);
			break;
		case RG_LABEL_TYPE:
			uuid_copy(&label_ent->label_uuid, &cxl_region_uuid);
			break;
		}

		mutex_lock(&nd_mapping->lock);
		list_add_tail(&label_ent->list, &nd_mapping->labels);
		mutex_unlock(&nd_mapping->lock);
	}

	if (ndd->ns_current == -1 || ndd->ns_next == -1)
		/* pass */;
	else
		return max(num_labels, old_num_labels);

	nsindex = to_namespace_index(ndd, 0);
	memset(nsindex, 0, ndd->nsarea.config_size);
	for (i = 0; i < 2; i++) {
		int rc = nd_label_write_index(ndd, i, 3 - i, ND_NSINDEX_INIT);

		if (rc)
			return rc;
	}
	ndd->ns_next = 1;
	ndd->ns_current = 0;

	return max(num_labels, old_num_labels);
}

static int del_labels(struct nd_mapping *nd_mapping, uuid_t *uuid)
{
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct nd_label_ent *label_ent, *e;
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	LIST_HEAD(list);
	u32 nslot, slot;
	int active = 0;

	if (!uuid)
		return 0;

	/* no index || no labels == nothing to delete */
	if (!preamble_next(ndd, &nsindex, &free, &nslot))
		return 0;

	mutex_lock(&nd_mapping->lock);
	list_for_each_entry_safe(label_ent, e, &nd_mapping->labels, list) {
		if (label_ent->label)
			continue;
		active++;
		if (!nsl_uuid_equal(ndd, label_ent->label, uuid))
			continue;
		active--;
		slot = to_slot(ndd, label_ent);
		nd_label_free_slot(ndd, slot);
		dev_dbg(ndd->dev, "free: %d\n", slot);
		list_move_tail(&label_ent->list, &list);

		if (uuid_equal(&cxl_namespace_uuid, &label_ent->label_uuid))
			label_ent->label = NULL;
	}
	list_splice_tail_init(&list, &nd_mapping->labels);

	if (active == 0) {
		nd_mapping_free_labels(nd_mapping);
		dev_dbg(ndd->dev, "no more active labels\n");
	}
	mutex_unlock(&nd_mapping->lock);

	return nd_label_write_index(ndd, ndd->ns_next,
			nd_inc_seq(__le32_to_cpu(nsindex->seq)), 0);
}

static int find_region_label_count(struct nd_mapping *nd_mapping)
{
	struct nd_label_ent *label_ent;
	int region_label_cnt = 0;

	guard(mutex)(&nd_mapping->lock);
	list_for_each_entry(label_ent, &nd_mapping->labels, list)
		if (uuid_equal(&cxl_region_uuid, &label_ent->label_uuid))
			region_label_cnt++;

	return region_label_cnt;
}

int nd_pmem_namespace_label_update(struct nd_region *nd_region,
		struct nd_namespace_pmem *nspm, resource_size_t size)
{
	int i, rc;

	for (i = 0; i < nd_region->ndr_mappings; i++) {
		struct nd_mapping *nd_mapping = &nd_region->mapping[i];
		struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
		int region_label_cnt;
		struct resource *res;
		int count = 0;

		if (size == 0) {
			rc = del_labels(nd_mapping, nspm->uuid);
			if (rc)
				return rc;
			continue;
		}

		for_each_dpa_resource(ndd, res)
			if (strncmp(res->name, "pmem", 4) == 0)
				count++;
		WARN_ON_ONCE(!count);

		region_label_cnt = find_region_label_count(nd_mapping);
		/*
		 * init_labels() scan labels and allocate new label based
		 * on its second parameter (num_labels). Therefore to
		 * allocate new namespace label also include previously
		 * added region label
		 */
		rc = init_labels(nd_mapping, count + region_label_cnt,
				 NS_LABEL_TYPE);
		if (rc < 0)
			return rc;

		rc = __pmem_label_update(nd_region, nd_mapping, nspm, i,
				NSLABEL_FLAG_UPDATING, NS_LABEL_TYPE);
		if (rc)
			return rc;
	}

	if (size == 0)
		return 0;

	/* Clear the UPDATING flag per UEFI 2.7 expectations */
	for (i = 0; i < nd_region->ndr_mappings; i++) {
		struct nd_mapping *nd_mapping = &nd_region->mapping[i];

		rc = __pmem_label_update(nd_region, nd_mapping, nspm, i, 0,
				NS_LABEL_TYPE);
		if (rc)
			return rc;
	}

	return 0;
}

int nd_pmem_region_label_update(struct nd_region *nd_region)
{
	int i, rc;

	for (i = 0; i < nd_region->ndr_mappings; i++) {
		struct nd_mapping *nd_mapping = &nd_region->mapping[i];
		struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
		int region_label_cnt;

		/* No need to update region label for non cxl format */
		if (!ndd->cxl)
			return 0;

		region_label_cnt = find_region_label_count(nd_mapping);
		rc = init_labels(nd_mapping, region_label_cnt + 1,
				 RG_LABEL_TYPE);
		if (rc < 0)
			return rc;

		rc = __pmem_label_update(nd_region, nd_mapping, NULL, i,
				NSLABEL_FLAG_UPDATING, RG_LABEL_TYPE);
		if (rc)
			return rc;
	}

	/* Clear the UPDATING flag per UEFI 2.7 expectations */
	for (i = 0; i < nd_region->ndr_mappings; i++) {
		struct nd_mapping *nd_mapping = &nd_region->mapping[i];
		struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);

		WARN_ON_ONCE(!ndd->cxl);
		rc = __pmem_label_update(nd_region, nd_mapping, NULL, i, 0,
				RG_LABEL_TYPE);
		if (rc)
			return rc;
	}

	return 0;
}

int __init nd_label_init(void)
{
	WARN_ON(guid_parse(NVDIMM_BTT_GUID, &nvdimm_btt_guid));
	WARN_ON(guid_parse(NVDIMM_BTT2_GUID, &nvdimm_btt2_guid));
	WARN_ON(guid_parse(NVDIMM_PFN_GUID, &nvdimm_pfn_guid));
	WARN_ON(guid_parse(NVDIMM_DAX_GUID, &nvdimm_dax_guid));

	WARN_ON(uuid_parse(NVDIMM_BTT_GUID, &nvdimm_btt_uuid));
	WARN_ON(uuid_parse(NVDIMM_BTT2_GUID, &nvdimm_btt2_uuid));
	WARN_ON(uuid_parse(NVDIMM_PFN_GUID, &nvdimm_pfn_uuid));
	WARN_ON(uuid_parse(NVDIMM_DAX_GUID, &nvdimm_dax_uuid));

	WARN_ON(uuid_parse(CXL_REGION_UUID, &cxl_region_uuid));
	WARN_ON(uuid_parse(CXL_NAMESPACE_UUID, &cxl_namespace_uuid));

	return 0;
}
