/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMA-buf mapping type internals shared by dma-buf.c and dma-buf-mapping.c.
 * Nothing in here is for exporters or importers.
 */
#ifndef __DMA_BUF_MAPPING_INTERNAL_H__
#define __DMA_BUF_MAPPING_INTERNAL_H__

#include <linux/dma-buf.h>
#include <linux/dma-buf-mapping.h>

/*
 * Internal: Selection is made by exporters through &dma_buf_ops.select_mapping
 * in dma_buf_pin() or map paths. Importers never call this.
 */
int dma_buf_mapping_select(struct dma_buf_attachment *attach);
void dma_buf_mapping_unselect(struct dma_buf_attachment *attach,
			      bool was_selected);

/*
 * Internal: Records the selection was successful after the pin or map that
 * followed dma_buf_mapping_select() succeeds.
 */
static inline void dma_buf_mapping_commit(struct dma_buf_attachment *attach)
{
	attach->selected = true;
}

/*
 * Internal: .map / .unmap of the SGT type. Implemented in dma-buf.c using
 * the exporter's &dma_buf_mapping_sgt_exp_ops.
 */
struct dma_buf_mapping *dma_buf_sgt_type_map(struct dma_buf_attachment *attach,
					     enum dma_data_direction dir);
void dma_buf_sgt_type_unmap(struct dma_buf_attachment *attach,
			    struct dma_buf_mapping *mapping,
			    enum dma_data_direction dir);

#endif /* __DMA_BUF_MAPPING_INTERNAL_H__ */
