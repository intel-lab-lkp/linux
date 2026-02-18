// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA BUF Mapping Helpers
 *
 */
#include <linux/dma-buf-mapping.h>
#include <linux/dma-resv.h>
#include <linux/dma-buf.h>
#include <linux/seq_file.h>

static struct scatterlist *fill_sg_entry(struct scatterlist *sgl, size_t length,
					 dma_addr_t addr)
{
	unsigned int len, nents;
	int i;

	nents = DIV_ROUND_UP(length, UINT_MAX);
	for (i = 0; i < nents; i++) {
		len = min_t(size_t, length, UINT_MAX);
		length -= len;
		/*
		 * DMABUF abuses scatterlist to create a scatterlist
		 * that does not have any CPU list, only the DMA list.
		 * Always set the page related values to NULL to ensure
		 * importers can't use it. The phys_addr based DMA API
		 * does not require the CPU list for mapping or unmapping.
		 */
		sg_set_page(sgl, NULL, 0, 0);
		sg_dma_address(sgl) = addr + (dma_addr_t)i * UINT_MAX;
		sg_dma_len(sgl) = len;
		sgl = sg_next(sgl);
	}

	return sgl;
}

static unsigned int calc_sg_nents(struct dma_iova_state *state,
				  struct dma_buf_phys_vec *phys_vec,
				  size_t nr_ranges, size_t size)
{
	unsigned int nents = 0;
	size_t i;

	if (!state || !dma_use_iova(state)) {
		for (i = 0; i < nr_ranges; i++)
			nents += DIV_ROUND_UP(phys_vec[i].len, UINT_MAX);
	} else {
		/*
		 * In IOVA case, there is only one SG entry which spans
		 * for whole IOVA address space, but we need to make sure
		 * that it fits sg->length, maybe we need more.
		 */
		nents = DIV_ROUND_UP(size, UINT_MAX);
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
					 struct dma_buf_phys_vec *phys_vec,
					 size_t nr_ranges, size_t size,
					 enum dma_data_direction dir)
{
	struct device *dma_dev = dma_buf_sgt_dma_device(attach);
	unsigned int nents, mapped_len = 0;
	struct dma_buf_dma *dma;
	struct scatterlist *sgl;
	dma_addr_t addr;
	size_t i;
	int ret;

	dma_resv_assert_held(attach->dmabuf->resv);

	if (WARN_ON(!attach || !attach->dmabuf || !provider))
		/* This function is supposed to work on MMIO memory only */
		return ERR_PTR(-EINVAL);

	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return ERR_PTR(-ENOMEM);

	switch (pci_p2pdma_map_type(provider, dma_dev)) {
	case PCI_P2PDMA_MAP_BUS_ADDR:
		/*
		 * There is no need in IOVA at all for this flow.
		 */
		break;
	case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
		dma->state = kzalloc(sizeof(*dma->state), GFP_KERNEL);
		if (!dma->state) {
			ret = -ENOMEM;
			goto err_free_dma;
		}

		dma_iova_try_alloc(dma_dev, dma->state, 0, size);
		break;
	default:
		ret = -EINVAL;
		goto err_free_dma;
	}

	nents = calc_sg_nents(dma->state, phys_vec, nr_ranges, size);
	ret = sg_alloc_table(&dma->sgt, nents, GFP_KERNEL | __GFP_ZERO);
	if (ret)
		goto err_free_state;

	sgl = dma->sgt.sgl;

	for (i = 0; i < nr_ranges; i++) {
		if (!dma->state) {
			addr = pci_p2pdma_bus_addr_map(provider,
						       phys_vec[i].paddr);
		} else if (dma_use_iova(dma->state)) {
			ret = dma_iova_link(dma_dev, dma->state,
					    phys_vec[i].paddr, 0,
					    phys_vec[i].len, dir,
					    DMA_ATTR_MMIO);
			if (ret)
				goto err_unmap_dma;

			mapped_len += phys_vec[i].len;
		} else {
			addr = dma_map_phys(dma_dev, phys_vec[i].paddr,
					    phys_vec[i].len, dir,
					    DMA_ATTR_MMIO);
			ret = dma_mapping_error(dma_dev, addr);
			if (ret)
				goto err_unmap_dma;
		}

		if (!dma->state || !dma_use_iova(dma->state))
			sgl = fill_sg_entry(sgl, phys_vec[i].len, addr);
	}

	if (dma->state && dma_use_iova(dma->state)) {
		WARN_ON_ONCE(mapped_len != size);
		ret = dma_iova_sync(dma_dev, dma->state, 0, mapped_len);
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
		dma_iova_destroy(dma_dev, dma->state, mapped_len, dir,
				 DMA_ATTR_MMIO);
	} else {
		for_each_sgtable_dma_sg(&dma->sgt, sgl, i)
			dma_unmap_phys(dma_dev, sg_dma_address(sgl),
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
	struct device *dma_dev = dma_buf_sgt_dma_device(attach);
	int i;

	dma_resv_assert_held(attach->dmabuf->resv);

	if (!dma->state) {
		; /* Do nothing */
	} else if (dma_use_iova(dma->state)) {
		dma_iova_destroy(dma_dev, dma->state, dma->size, dir,
				 DMA_ATTR_MMIO);
	} else {
		struct scatterlist *sgl;

		for_each_sgtable_dma_sg(sgt, sgl, i)
			dma_unmap_phys(dma_dev, sg_dma_address(sgl),
				       sg_dma_len(sgl), dir, DMA_ATTR_MMIO);
	}

	sg_free_table(sgt);
	kfree(dma->state);
	kfree(dma);

}
EXPORT_SYMBOL_NS_GPL(dma_buf_free_sgt, "DMA_BUF");

/**
 * dma_buf_match_mapping - Select a mapping type agreed upon by exporter and
 *                         importer
 * @args: Match arguments from attach. On success this is updated with the
 *        matched exporter and importer entries.
 * @exp: Array of mapping types supported by the exporter, in priority order
 * @exp_len: Number of entries in @exp
 *
 * Iterate over the exporter's supported mapping types and for each one search
 * the importer's list for a compatible matching type. args and args->attach are
 * populated with the resulting match.
 *
 * Because the exporter list is walked in order, the exporter controls the
 * priority of mapping types.
 */
int dma_buf_match_mapping(struct dma_buf_match_args *args,
			  const struct dma_buf_mapping_match *exp,
			  size_t exp_len)
{
	const struct dma_buf_mapping_match *exp_end = exp + exp_len;
	const struct dma_buf_mapping_match *imp_end =
		args->imp_matches + args->imp_len;
	int ret;

	for (; exp != exp_end; exp++) {
		const struct dma_buf_mapping_match *imp = args->imp_matches;

		for (; imp != imp_end; imp++) {
			if (exp->type != imp->type)
				continue;
			if (exp->type->match) {
				ret = exp->type->match(args->dmabuf, exp, imp);
				if (ret == -EOPNOTSUPP)
					continue;
				if (ret != 0)
					return ret;
			}
			exp->type->finish_match(args, exp, imp);
			return 0;
		}
	}
	return -EINVAL;
}
EXPORT_SYMBOL_NS_GPL(dma_buf_match_mapping, "DMA_BUF");

static int dma_buf_sgt_match(struct dma_buf *dmabuf,
			     const struct dma_buf_mapping_match *exp,
			     const struct dma_buf_mapping_match *imp)
{
	switch (exp->sgt_data.exporter_requires_p2p) {
	case DMA_SGT_NO_P2P:
		return 0;
	case DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE:
		if (WARN_ON(!exp->sgt_data.exporting_p2p_device) ||
		    imp->sgt_data.importer_accepts_p2p !=
			    DMA_SGT_IMPORTER_ACCEPTS_P2P)
			return -EOPNOTSUPP;
		if (pci_p2pdma_distance(exp->sgt_data.exporting_p2p_device,
					imp->sgt_data.importing_dma_device,
					true) < 0)
			return -EOPNOTSUPP;
		return 0;
	}
	return 0;
}

static inline void
dma_buf_sgt_finish_match(struct dma_buf_match_args *args,
			 const struct dma_buf_mapping_match *exp,
			 const struct dma_buf_mapping_match *imp)
{
	struct dma_buf_attachment *attach = args->attach;

	attach->map_type = (struct dma_buf_mapping_match) {
		.type = &dma_buf_mapping_sgt_type,
		.exp_ops = exp->exp_ops,
		.sgt_data = {
			.importing_dma_device = imp->sgt_data.importing_dma_device,
			/* exporting_p2p_device is left opaque */
			.importer_accepts_p2p = imp->sgt_data.importer_accepts_p2p,
			.exporter_requires_p2p = exp->sgt_data.exporter_requires_p2p,
		},
	};
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
};
EXPORT_SYMBOL_NS_GPL(dma_buf_mapping_sgt_type, "DMA_BUF");

static const struct dma_buf_mapping_pal_exp_ops *
to_pal_exp_ops(struct dma_buf_attachment *attach)
{
	return container_of(attach->map_type.exp_ops,
			    struct dma_buf_mapping_pal_exp_ops, ops);
}

/**
 * dma_buf_pal_map_phys - Obtain the physical address list for a PAL attachment
 * @attach: The DMA-buf attachment
 *
 * Calls the exporter's map_phys() callback to retrieve the physical address
 * list for the buffer. The caller must hold the dma-buf's reservation lock.
 *
 * This symbol is restricted to iommufd to prevent misuse.
 *
 * Returns the physical address list on success, or an ERR_PTR on failure.
 * The returned list must be freed with dma_buf_pal_unmap_phys().
 */
struct dma_buf_phys_list *
dma_buf_pal_map_phys(struct dma_buf_attachment *attach)
{
	dma_resv_assert_held(attach->dmabuf->resv);
	return to_pal_exp_ops(attach)->map_phys(attach);
}
/*
 * Restricted, iommufd is the only importer allowed to prevent misuse of this
 * API.
 */
EXPORT_SYMBOL_FOR_MODULES(dma_buf_pal_map_phys, "iommufd");

/**
 * dma_buf_pal_unmap_phys - Unmap a physical address list
 * @attach: The DMA-buf attachment
 * @phys: The physical address list returned by dma_buf_pal_map_phys()
 *
 * Returns the mapping back to the exporter. After this point the importer may
 * not touch any of the addresses in any way.
 */
void dma_buf_pal_unmap_phys(struct dma_buf_attachment *attach,
			    struct dma_buf_phys_list *phys)
{
	to_pal_exp_ops(attach)->unmap_phys(attach, phys);
}
EXPORT_SYMBOL_NS_GPL(dma_buf_pal_unmap_phys, "DMA_BUF");

static inline void
dma_buf_pal_finish_match(struct dma_buf_match_args *args,
			 const struct dma_buf_mapping_match *exp,
			 const struct dma_buf_mapping_match *imp)
{
	args->attach->map_type = (struct dma_buf_mapping_match){
		.type = &dma_buf_mapping_pal_type,
		.exp_ops = exp->exp_ops,
	};
}

struct dma_buf_mapping_type dma_buf_mapping_pal_type = {
	.name = "Physical Address List",
	.finish_match = dma_buf_pal_finish_match,
};
EXPORT_SYMBOL_NS_GPL(dma_buf_mapping_pal_type, "DMA_BUF");
