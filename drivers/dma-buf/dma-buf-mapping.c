// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA BUF Mapping Helpers
 *
 */
#include <linux/dma-buf-mapping.h>
#include <linux/dma-resv.h>
#include <linux/dma-buf.h>
#include <linux/seq_file.h>
#include <linux/overflow.h>
#include <linux/align.h>

#define MAX_SG_ENT_SZ ALIGN_DOWN(UINT_MAX, PAGE_SIZE)

static struct scatterlist *fill_sg_entry(struct scatterlist *sgl, size_t length,
					 dma_addr_t addr)
{
	size_t len;

	while (length) {
		len = min(length, MAX_SG_ENT_SZ);
		length -= len;
		/*
		 * DMABUF abuses scatterlist to create a scatterlist
		 * that does not have any CPU list, only the DMA list.
		 * Always set the page related values to NULL to ensure
		 * importers can't use it. The phys_addr based DMA API
		 * does not require the CPU list for mapping or unmapping.
		 */
		sg_set_page(sgl, NULL, 0, 0);
		sg_dma_address(sgl) = addr;
		sg_dma_len(sgl) = len;
		addr += len;
		/* Unconditionally advance. On last segment, this becomes NULL */
		sgl = sg_next(sgl);
	}

	return sgl;
}

static unsigned int calc_sg_nents(struct dma_iova_state *state,
				  struct phys_vec *phys_vec, size_t nr_ranges,
				  size_t size)
{
	unsigned int nents = 0;
	size_t i;

	if (!state || !dma_use_iova(state)) {
		for (i = 0; i < nr_ranges; i++) {
			unsigned int added = DIV_ROUND_UP(phys_vec[i].len, MAX_SG_ENT_SZ);

			if (check_add_overflow(nents, added, &nents))
				return 0;
		}
	} else {
		/*
		 * In IOVA case, there is only one SG entry which spans
		 * for whole IOVA address space, but we need to make sure
		 * that it fits sg->length, maybe we need more.
		 */
		nents = DIV_ROUND_UP(size, MAX_SG_ENT_SZ);
	}

	return nents;
}

/**
 * struct dma_buf_dma - holds DMA mapping information
 * @sgt:    Scatter-gather table
 * @state:  DMA IOVA state relevant in IOMMU-based DMA
 * @size:   Total size of DMA transfer
 */
struct dma_buf_dma {
	struct sg_table sgt;
	struct dma_iova_state *state;
	size_t size;
};

/**
 * dma_buf_phys_vec_to_sgt - Returns the scatterlist table of the attachment
 * from arrays of physical vectors. This funciton is intended for MMIO memory
 * only.
 * @attach:	[in]	attachment whose scatterlist is to be returned
 * @provider:	[in]	p2pdma provider
 * @phys_vec:	[in]	array of physical vectors
 * @nr_ranges:	[in]	number of entries in phys_vec array
 * @size:	[in]	total size of phys_vec
 * @dir:	[in]	direction of DMA transfer
 *
 * Returns sg_table containing the scatterlist to be returned; returns ERR_PTR
 * on error. May return -EINTR if it is interrupted by a signal.
 *
 * On success, the DMA addresses and lengths in the returned scatterlist are
 * PAGE_SIZE aligned.
 *
 * A mapping must be unmapped by using dma_buf_free_sgt().
 *
 * NOTE: This function is intended for exporters. If direct traffic routing is
 * mandatory exporter should call routing pci_p2pdma_map_type() before calling
 * this function.
 */
struct sg_table *dma_buf_phys_vec_to_sgt(struct dma_buf_attachment *attach,
					 struct p2pdma_provider *provider,
					 struct phys_vec *phys_vec,
					 size_t nr_ranges, size_t size,
					 enum dma_data_direction dir)
{
	struct dma_buf_dma *dma;
	struct scatterlist *sgl;
	size_t mapped_len = 0;
	unsigned int nents;
	dma_addr_t addr;
	size_t i;
	int ret;

	dma_resv_assert_held(attach->dmabuf->resv);

	if (WARN_ON(!attach || !attach->dmabuf || !provider))
		/* This function is supposed to work on MMIO memory only */
		return ERR_PTR(-EINVAL);

	dma = kzalloc_obj(*dma);
	if (!dma)
		return ERR_PTR(-ENOMEM);

	switch (pci_p2pdma_map_type(provider, attach->dev)) {
	case PCI_P2PDMA_MAP_BUS_ADDR:
		/*
		 * There is no need in IOVA at all for this flow.
		 */
		break;
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
		dma->state = kzalloc_obj(*dma->state);
		if (!dma->state) {
			ret = -ENOMEM;
			goto err_free_dma;
		}

		dma_iova_try_alloc(attach->dev, dma->state, 0, size);
		break;
	default:
		ret = -EINVAL;
		goto err_free_dma;
	}

	nents = calc_sg_nents(dma->state, phys_vec, nr_ranges, size);

	/* sg_alloc_table will cleanly fail and return -EINVAL if nents == 0 */
	ret = sg_alloc_table(&dma->sgt, nents, GFP_KERNEL | __GFP_ZERO);
	if (ret)
		goto err_free_state;

	sgl = dma->sgt.sgl;

	for (i = 0; i < nr_ranges; i++) {
		if (!dma->state) {
			addr = pci_p2pdma_bus_addr_map(provider,
						       phys_vec[i].paddr);
		} else if (dma_use_iova(dma->state)) {
			ret = dma_iova_link(attach->dev, dma->state,
					    phys_vec[i].paddr, 0,
					    phys_vec[i].len, dir,
					    DMA_ATTR_MMIO);
			if (ret)
				goto err_unmap_dma;

			mapped_len += phys_vec[i].len;
		} else {
			addr = dma_map_phys(attach->dev, phys_vec[i].paddr,
					    phys_vec[i].len, dir,
					    DMA_ATTR_MMIO);
			ret = dma_mapping_error(attach->dev, addr);
			if (ret)
				goto err_unmap_dma;
		}

		if (!dma->state || !dma_use_iova(dma->state))
			sgl = fill_sg_entry(sgl, phys_vec[i].len, addr);
	}

	if (dma->state && dma_use_iova(dma->state)) {
		WARN_ON_ONCE(mapped_len != size);
		ret = dma_iova_sync(attach->dev, dma->state, 0, mapped_len);
		if (ret)
			goto err_unmap_dma;

		sgl = fill_sg_entry(sgl, mapped_len, dma->state->addr);
	}

	dma->size = size;

	/*
	 * No CPU list included — set orig_nents = 0 so others can detect
	 * this via SG table (use nents only).
	 */
	dma->sgt.orig_nents = 0;


	/*
	 * SGL must be NULL to indicate that SGL is the last one
	 * and we allocated correct number of entries in sg_alloc_table()
	 */
	WARN_ON_ONCE(sgl);
	return &dma->sgt;

err_unmap_dma:
	if (!i || !dma->state) {
		; /* Do nothing */
	} else if (dma_use_iova(dma->state)) {
		dma_iova_destroy(attach->dev, dma->state, mapped_len, dir,
				 DMA_ATTR_MMIO);
	} else {
		for_each_sgtable_dma_sg(&dma->sgt, sgl, i)
			dma_unmap_phys(attach->dev, sg_dma_address(sgl),
				       sg_dma_len(sgl), dir, DMA_ATTR_MMIO);
	}
	sg_free_table(&dma->sgt);
err_free_state:
	kfree(dma->state);
err_free_dma:
	kfree(dma);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_phys_vec_to_sgt, "DMA_BUF");

/**
 * dma_buf_free_sgt- unmaps the buffer
 * @attach:	[in]	attachment to unmap buffer from
 * @sgt:	[in]	scatterlist info of the buffer to unmap
 * @dir:	[in]	direction of DMA transfer
 *
 * This unmaps a DMA mapping for @attached obtained
 * by dma_buf_phys_vec_to_sgt().
 */
void dma_buf_free_sgt(struct dma_buf_attachment *attach, struct sg_table *sgt,
		      enum dma_data_direction dir)
{
	struct dma_buf_dma *dma = container_of(sgt, struct dma_buf_dma, sgt);
	int i;

	dma_resv_assert_held(attach->dmabuf->resv);

	if (!dma->state) {
		; /* Do nothing */
	} else if (dma_use_iova(dma->state)) {
		dma_iova_destroy(attach->dev, dma->state, dma->size, dir,
				 DMA_ATTR_MMIO);
	} else {
		struct scatterlist *sgl;

		for_each_sgtable_dma_sg(sgt, sgl, i)
			dma_unmap_phys(attach->dev, sg_dma_address(sgl),
				       sg_dma_len(sgl), dir, DMA_ATTR_MMIO);
	}

	sg_free_table(sgt);
	kfree(dma->state);
	kfree(dma);

}
EXPORT_SYMBOL_NS_GPL(dma_buf_free_sgt, "DMA_BUF");

/**
 * dma_buf_match_mapping - Record the mapping types agreed upon by exporter
 *                         and importer
 * @args: Match arguments from attach
 * @exp: Array of mapping types supported by the exporter, in priority order
 * @exp_len: Number of entries in @exp
 *
 * Walk the exporter's list in order and, for each entry, find the first
 * importer entry of the same type that the type's match() callback accepts.
 * Every such pair is recorded in @args->attach->matches, so the match set
 * preserves the exporter's priority. The first entry is the default returned
 * by dma_buf_attach_map_type() until &dma_buf_ops.select_mapping chooses
 * otherwise at pin or map time.
 *
 * Because the exporter list is walked in order, the exporter controls the
 * priority of mapping types.
 *
 * Returns: 0 if at least one type matched, -EOPNOTSUPP if the exporter and
 * importer have no type in common, -EINVAL if either list is empty, -E2BIG if
 * more than DMA_BUF_MAX_MATCHES types matched, or the error a match() callback
 * aborted the search with.
 */
int dma_buf_match_mapping(struct dma_buf_match_args *args,
			  const struct dma_buf_mapping_match *exp,
			  size_t exp_len)
{
	const struct dma_buf_mapping_match *exp_end = exp + exp_len;
	const struct dma_buf_mapping_match *imp_end =
		args->imp_matches + args->imp_len;
	struct dma_buf_attachment *attach = args->attach;
	unsigned int nr = 0;
	int ret;

	if (!exp_len || !args->imp_len)
		return -EINVAL;

	for (; exp != exp_end; exp++) {
		const struct dma_buf_mapping_match *imp = args->imp_matches;

		for (; imp != imp_end; imp++) {
			if (exp->type != imp->type)
				continue;
			if (exp->type->match) {
				ret = exp->type->match(args, exp, imp);
				if (ret == -EOPNOTSUPP)
					continue;
				if (ret != 0)
					return ret;
			}
			if (nr == ARRAY_SIZE(attach->matches))
				return -E2BIG;
			exp->type->finish_match(args, exp, imp,
						&attach->matches[nr++]);
			break;
		}
	}

	if (!nr)
		return -EOPNOTSUPP;

	attach->nr_matches = nr;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_match_mapping, "DMA_BUF");

/**
 * dma_buf_mapping_select - Let the exporter choose the type to map with
 * @attach: Attachment about to be pinned or mapped
 *
 * Called with the dma_resv lock held by dma_buf_pin() and the map functions.
 * The first call after attach, and after each dma_buf_invalidate_mappings(),
 * calls &dma_buf_ops.select_mapping to pick an entry of the match set given
 * the buffer's placement and the other live mappings. Exporters without the
 * callback get the first entry. Every other call returns the previous choice
 * unchanged, so the mapping type cannot change between unmap and remap without
 * the exporter having explicitly invalidated.
 *
 * Returns: 0 on success or the exporter's error, typically -EBUSY when the
 * attachment cannot be served without disturbing a mapping that cannot be
 * invalidated.
 */
int dma_buf_mapping_select(struct dma_buf_attachment *attach)
{
	int idx = 0;

	dma_resv_assert_held(attach->dmabuf->resv);

	if (attach->selected)
		return 0;

	if (WARN_ON(!attach->nr_matches))
		return -EINVAL;

	if (attach->dmabuf->ops->select_mapping) {
		idx = attach->dmabuf->ops->select_mapping(attach);
		if (idx < 0)
			return idx;
		if (WARN_ON(idx >= attach->nr_matches))
			return -EINVAL;
	}

	attach->selected_idx = idx;
	attach->selected = true;
	return 0;
}

/**
 * dma_buf_mapping_unselect - Undo a selection that established nothing
 * @attach: Attachment whose pin or map just failed
 * @was_selected: attach->selected before the failed pin or map began
 *
 * A pin or map that fails after dma_buf_mapping_select() may reopen the
 * choice only if it was the one that made it. A selection that predates the
 * call belongs to a live pin or mapping and must survive, otherwise the next
 * pin or map would reselect under it without an invalidate.
 */
void dma_buf_mapping_unselect(struct dma_buf_attachment *attach,
			      bool was_selected)
{
	dma_resv_assert_held(attach->dmabuf->resv);

	if (was_selected)
		return;

	attach->selected = false;
	attach->selected_idx = 0;
}

/**
 * dma_buf_mapping_map - Map an attachment with whichever type the exporter
 *                       selects
 * @attach: the attachment
 * @dir: DMA direction
 *
 * For importers that listed more than one mapping type. The exporter picks the
 * type from the possible matches and the result's &dma_buf_mapping.type records
 * which was used. Must be called with the dma_resv lock held, and the lock must
 * be held until the result has been acted on. Importers that listed a single
 * type keep using that type's map function.
 *
 * Returns: a &struct dma_buf_mapping to pass to dma_buf_mapping_unmap(), or
 * an ERR_PTR.
 */
struct dma_buf_mapping *dma_buf_mapping_map(struct dma_buf_attachment *attach,
					    enum dma_data_direction dir)
{
	const struct dma_buf_mapping_type *type;
	struct dma_buf_mapping *mapping;
	bool selected;
	int ret;

	might_sleep();

	if (WARN_ON(!attach || !attach->dmabuf))
		return ERR_PTR(-EINVAL);

	dma_resv_assert_held(attach->dmabuf->resv);

	selected = attach->selected;
	ret = dma_buf_mapping_select(attach);
	if (ret)
		return ERR_PTR(ret);

	type = dma_buf_attach_map_type(attach)->type;
	if (!type->map) {
		ret = -EOPNOTSUPP;
		goto err_release;
	}

	mapping = type->map(attach, dir);
	if (IS_ERR(mapping)) {
		ret = PTR_ERR(mapping);
		goto err_release;
	}

	return mapping;

err_release:
	dma_buf_mapping_unselect(attach, selected);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_mapping_map, "DMA_BUF");

/**
 * dma_buf_mapping_unmap - Release a mapping from dma_buf_mapping_map()
 * @attach: the attachment
 * @mapping: what dma_buf_mapping_map() returned
 * @dir: DMA direction used for the map
 */
void dma_buf_mapping_unmap(struct dma_buf_attachment *attach,
			   struct dma_buf_mapping *mapping,
			   enum dma_data_direction dir)
{
	might_sleep();

	if (WARN_ON(!attach || !attach->dmabuf || !mapping))
		return;

	dma_resv_assert_held(attach->dmabuf->resv);

	mapping->type->unmap(attach, mapping, dir);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_mapping_unmap, "DMA_BUF");

static int dma_buf_sgt_match(struct dma_buf_match_args *args,
			     const struct dma_buf_mapping_match *exp,
			     const struct dma_buf_mapping_match *imp)
{
	switch (exp->sgt_data.exporter_requires_p2p) {
	case DMA_SGT_NO_P2P:
		return 0;
	case DMA_SGT_EXPORTER_REQUIRES_P2P:
		if (imp->sgt_data.importer_accepts_p2p !=
		    DMA_SGT_IMPORTER_ACCEPTS_P2P)
			return -EOPNOTSUPP;
		return 0;
	case DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE:
		if (WARN_ON(!exp->sgt_data.exporting_p2p_device) ||
		    imp->sgt_data.importer_accepts_p2p !=
			    DMA_SGT_IMPORTER_ACCEPTS_P2P)
			return -EOPNOTSUPP;
		/*
		 * Not verbose: failing to match P2P is normal when the
		 * exporter also offers a system memory type.
		 */
		if (pci_p2pdma_distance(exp->sgt_data.exporting_p2p_device,
					args->dev, false) < 0)
			return -EOPNOTSUPP;
		return 0;
	}
	return 0;
}

static inline void
dma_buf_sgt_finish_match(struct dma_buf_match_args *args,
			 const struct dma_buf_mapping_match *exp,
			 const struct dma_buf_mapping_match *imp,
			 struct dma_buf_mapping_match *match)
{
	struct dma_buf_attachment *attach = args->attach;

	*match = (struct dma_buf_mapping_match) {
		.type = &dma_buf_mapping_sgt_type,
		.exp_ops = exp->exp_ops,
		.sgt_data = {
			.importing_dma_device = args->dev,
			/* exporting_p2p_device is left opaque */
			.importer_accepts_p2p = imp->sgt_data.importer_accepts_p2p,
			.exporter_requires_p2p = exp->sgt_data.exporter_requires_p2p,
		},
	};

	/*
	 * Setup the SGT type variables stored in attach because importers and
	 * exporters that do not natively use mappings expect them to be there.
	 * When converting to use mappings users should use the match versions
	 * of these instead.
	 */
	attach->dev = args->dev;
	attach->peer2peer = match->sgt_data.importer_accepts_p2p ==
			    DMA_SGT_IMPORTER_ACCEPTS_P2P;
}

static void dma_buf_sgt_debugfs_dump(struct seq_file *s,
				     struct dma_buf_attachment *attach)
{
	seq_printf(s, " %s", dev_name(dma_buf_sgt_dma_device(attach)));
}

struct dma_buf_mapping_type dma_buf_mapping_sgt_type = {
	.name = "DMA Mapped Scatter Gather Table",
	.match = dma_buf_sgt_match,
	.finish_match = dma_buf_sgt_finish_match,
	.debugfs_dump = dma_buf_sgt_debugfs_dump,
	.map = dma_buf_sgt_type_map,
	.unmap = dma_buf_sgt_type_unmap,
};
EXPORT_SYMBOL_NS_GPL(dma_buf_mapping_sgt_type, "DMA_BUF");

static struct sg_table *
dma_buf_sgt_compat_map_dma_buf(struct dma_buf_attachment *attach,
			       enum dma_data_direction dir)
{
	return attach->dmabuf->ops->map_dma_buf(attach, dir);
}

static void dma_buf_sgt_compat_unmap_dma_buf(struct dma_buf_attachment *attach,
					     struct sg_table *sgt,
					     enum dma_data_direction dir)
{
	attach->dmabuf->ops->unmap_dma_buf(attach, sgt, dir);
}

/* Route the classic map/unmap ops through the exp ops for old importers */
static const struct dma_buf_mapping_sgt_exp_ops dma_buf_sgt_compat_exp_ops = {
	.map_dma_buf = dma_buf_sgt_compat_map_dma_buf,
	.unmap_dma_buf = dma_buf_sgt_compat_unmap_dma_buf,
};

/*
 * This mapping type is used for unaware exporters that do not support
 * match_mapping(). It wraps the dma_buf ops for SGT mappings into a mapping
 * type so aware importers can transparently work with unaware exporters. This
 * does not require p2p because old exporters will check it through the
 * attach->peer2peer mechanism.
 */
const struct dma_buf_mapping_match dma_buf_sgt_exp_compat_match =
	DMA_BUF_EMAPPING_SGT(&dma_buf_sgt_compat_exp_ops);
