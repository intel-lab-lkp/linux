/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMA BUF Mapping Helpers
 *
 */
#ifndef __DMA_BUF_MAPPING_H__
#define __DMA_BUF_MAPPING_H__
#include <linux/dma-buf.h>

struct device;
struct dma_buf;
struct dma_buf_attachment;
struct dma_buf_mapping_exp_ops;

/* Type tag for all mapping operations */
struct dma_buf_mapping_exp_ops {};

/*
 * Internal struct to pass arguments from the attach function to the matching
 * function
 */
struct dma_buf_match_args {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	const struct dma_buf_mapping_match *imp_matches;
	size_t imp_len;
};

/**
 * struct dma_buf_mapping_type - Operations for a DMA-buf mapping type
 *
 * Each mapping type provides a singleton instance of this struct to describe
 * the mapping type and its operations.
 */
struct dma_buf_mapping_type {
	/**
	 * @name: Human-readable name for this mapping type, used in debugfs
	 *        output
	 */
	const char *name;

	/**
	 * @match:
	 *
	 * Called during attach from dma_buf_match_mapping(). &exp and &imp are
	 * single items from the importer and exporter mapping match lists.
	 * Both will have the same instance of this struct as their type member.
	 *
	 * It determines if the exporter/importer are compatible.
	 *
	 * Returns: 0 on success
	 *   -EOPNOTSUPP means ignore the failure and continue
	 *   Everything else aborts the search and returns the -errno
	 */
	int (*match)(struct dma_buf *dmabuf,
		     const struct dma_buf_mapping_match *exp,
		     const struct dma_buf_mapping_match *imp);

	/**
	 * @finish_match:
	 *
	 * Called by dma_buf_match_mapping() after a successful match to store
	 * the negotiated result in @args->attach. The matched @exp and @imp
	 * entries are provided so the callback can copy type-specific data into
	 * the attachment.
	 */
	void (*finish_match)(struct dma_buf_match_args *args,
			     const struct dma_buf_mapping_match *exp,
			     const struct dma_buf_mapping_match *imp);

	/**
	 * @debugfs_dump:
	 *
	 * Optional callback to write mapping-type-specific diagnostic
	 * information about @attach to the debugfs seq_file @s.
	 */
	void (*debugfs_dump)(struct seq_file *s,
			     struct dma_buf_attachment *attach);
};

struct sg_table *dma_buf_phys_vec_to_sgt(struct dma_buf_attachment *attach,
					 struct p2pdma_provider *provider,
					 struct dma_buf_phys_vec *phys_vec,
					 size_t nr_ranges, size_t size,
					 enum dma_data_direction dir);
void dma_buf_free_sgt(struct dma_buf_attachment *attach, struct sg_table *sgt,
		      enum dma_data_direction dir);

int dma_buf_match_mapping(struct dma_buf_match_args *args,
			  const struct dma_buf_mapping_match *exp_mappings,
			  size_t exp_len);

#endif
