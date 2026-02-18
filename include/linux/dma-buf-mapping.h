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

enum dma_sgt_requires_p2p {
	DMA_SGT_NO_P2P = 0,
	DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE,
	DMA_SGT_IMPORTER_ACCEPTS_P2P,
};

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

/*
 * DMA Mapped Scatterlist Type
 *
 * When this type is matched the map/unmap functions are:
 *
 *  dma_buf_map_attachment()
 *  dma_buf_unmap_attachment()
 *
 * The struct sg_table returned by those functions has only the DMA portions
 * available. The caller must not try to use the struct page * information.
 *
 * importing_dma_device is passed to the DMA API to provide the dma_addr_t's.
 */
extern struct dma_buf_mapping_type dma_buf_mapping_sgt_type;

struct dma_buf_mapping_sgt_exp_ops {
	struct dma_buf_mapping_exp_ops ops;
	struct sg_table *(*map_dma_buf)(struct dma_buf_attachment *attach,
					enum dma_data_direction dir);
	void (*unmap_dma_buf)(struct dma_buf_attachment *attach,
			      struct sg_table *sgt,
			      enum dma_data_direction dir);
};

/**
 * dma_buf_sgt_dma_device - Return the device to use for DMA mapping
 * @attach: sgt mapping type attachment
 *
 * Called by the exporter to get the struct device to pass to the DMA API
 * during map and unmap callbacks.
 */
static inline struct device *
dma_buf_sgt_dma_device(struct dma_buf_attachment *attach)
{
	if (attach->map_type.type != &dma_buf_mapping_sgt_type)
		return NULL;
	return attach->map_type.sgt_data.importing_dma_device;
}

/**
 * dma_buf_sgt_p2p_allowed - True if MMIO memory can be used peer to peer
 * @attach: sgt mapping type attachment
 *
 * Should be called by exporters, returns true if the exporter's
 * DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE was matched.
 */
static inline bool dma_buf_sgt_p2p_allowed(struct dma_buf_attachment *attach)
{
	if (attach->map_type.type != &dma_buf_mapping_sgt_type)
		return false;
	return attach->map_type.sgt_data.exporter_requires_p2p ==
	       DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE;
}

static inline const struct dma_buf_mapping_sgt_exp_ops *
dma_buf_get_sgt_ops(struct dma_buf_attachment *attach)
{
	if (attach->map_type.type != &dma_buf_mapping_sgt_type)
		return NULL;
	return container_of(attach->map_type.exp_ops,
			    struct dma_buf_mapping_sgt_exp_ops, ops);
}

static inline struct dma_buf_mapping_match
DMA_BUF_IMAPPING_SGT(struct device *importing_dma_device,
		     enum dma_sgt_requires_p2p importer_accepts_p2p)
{
	return (struct dma_buf_mapping_match){
		.type = &dma_buf_mapping_sgt_type,
		.sgt_data = { .importing_dma_device = importing_dma_device,
			      .importer_accepts_p2p = importer_accepts_p2p },
	};
}
#define DMA_BUF_EMAPPING_SGT(_exp_ops)                                      \
	((struct dma_buf_mapping_match){ .type = &dma_buf_mapping_sgt_type, \
					 .exp_ops = &((_exp_ops)->ops) })

/*
 * Only matches if the importing device is P2P capable and the P2P subsystem
 * says P2P is possible from p2p_device.
 */
static inline struct dma_buf_mapping_match
DMA_BUF_EMAPPING_SGT_P2P(const struct dma_buf_mapping_sgt_exp_ops *exp_ops,
			 struct pci_dev *p2p_device)
{
	struct dma_buf_mapping_match match = DMA_BUF_EMAPPING_SGT(exp_ops);

	match.sgt_data.exporter_requires_p2p =
		DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE;
	match.sgt_data.exporting_p2p_device = p2p_device;
	return match;
}

extern const struct dma_buf_mapping_match dma_buf_sgt_exp_compat_match;

/*
 * dma_buf_ops initializer helper for simple drivers that use a single
 * SGT map/unmap operation without P2P.
 */
#define DMA_BUF_SIMPLE_SGT_EXP_MATCH(_map, _unmap)                       \
	.single_exporter_match = &((const struct dma_buf_mapping_match){ \
		.type = &dma_buf_mapping_sgt_type,                       \
		.exp_ops = &((const struct dma_buf_mapping_sgt_exp_ops){ \
			.map_dma_buf = _map,                             \
			.unmap_dma_buf = _unmap,                         \
		}.ops),                                                  \
		.sgt_data = {                                            \
			.exporter_requires_p2p = DMA_SGT_NO_P2P,         \
		} })

#endif
