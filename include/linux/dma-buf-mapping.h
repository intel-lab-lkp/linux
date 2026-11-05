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
	/*
	 * Exporter: MMIO, importer must accept P2P and pci_p2pdma_distance()
	 * from exporting_p2p_device must succeed at match time. The match runs
	 * without the dma_resv lock whenever an importer attaches, so only use
	 * this if exporting_p2p_device stays bound to its driver for as long as
	 * the dma_buf exists. Importers that are not PCI devices never match.
	 */
	DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE,
	/*
	 * Exporter: MMIO, importer must accept P2P. No distance check at match;
	 * the exporter's map does its own, e.g. via dma_buf_phys_vec_to_sgt().
	 */
	DMA_SGT_EXPORTER_REQUIRES_P2P,
	DMA_SGT_IMPORTER_ACCEPTS_P2P,
};

/* Type tag for all mapping operations */
struct dma_buf_mapping_exp_ops {};

/**
 * struct dma_buf_mapping - header of a mapping made with dma_buf_mapping_map()
 * @type: The mapping type the exporter selected. Importers that listed more
 *        than one type switch on this and use the type's accessor to get at
 *        the type-specific result embedding this header.
 */
struct dma_buf_mapping {
	const struct dma_buf_mapping_type *type;
};

/*
 * Internal struct to pass arguments from the attach function to the matching
 * function
 */
struct dma_buf_match_args {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	/* The importing device passed to dma_buf_dynamic_attach() */
	struct device *dev;
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
	 * The importing device is @args->dev; importer entries do not carry
	 * it, so they can be static tables shared by every attachment.
	 *
	 * It determines if the exporter/importer are compatible.
	 *
	 * Returns: 0 on success
	 *   -EOPNOTSUPP means ignore the failure and continue
	 *   Everything else aborts the search and returns the -errno
	 */
	int (*match)(struct dma_buf_match_args *args,
		     const struct dma_buf_mapping_match *exp,
		     const struct dma_buf_mapping_match *imp);

	/**
	 * @finish_match:
	 *
	 * Called by dma_buf_match_mapping() after a successful match to fill
	 * in @match, the entry being added to @args->attach->matches. The
	 * matched @exp and @imp entries are provided so the callback can copy
	 * type-specific data into it, along with @args->dev if the type needs
	 * the importing device later.
	 */
	void (*finish_match)(struct dma_buf_match_args *args,
			     const struct dma_buf_mapping_match *exp,
			     const struct dma_buf_mapping_match *imp,
			     struct dma_buf_mapping_match *match);

	/**
	 * @debugfs_dump:
	 *
	 * Optional callback to write mapping-type-specific diagnostic
	 * information about @attach to the debugfs seq_file @s.
	 */
	void (*debugfs_dump)(struct seq_file *s,
			     struct dma_buf_attachment *attach);

	/**
	 * @map:
	 *
	 * Produce a mapping of this type for @attach, for which this type has
	 * already been selected. Called by dma_buf_mapping_map()
	 * with the dma_resv lock held. Responsible for pin-on-map of static
	 * importers if the type supports them. Optional; a type without it
	 * can only be used through its own map functions.
	 */
	struct dma_buf_mapping *(*map)(struct dma_buf_attachment *attach,
				       enum dma_data_direction dir);

	/**
	 * @unmap:
	 *
	 * Release a mapping returned by @map.
	 */
	void (*unmap)(struct dma_buf_attachment *attach,
		      struct dma_buf_mapping *mapping,
		      enum dma_data_direction dir);
};

struct dma_buf_mapping *dma_buf_mapping_map(struct dma_buf_attachment *attach,
					    enum dma_data_direction dir);
void dma_buf_mapping_unmap(struct dma_buf_attachment *attach,
			   struct dma_buf_mapping *mapping,
			   enum dma_data_direction dir);

struct sg_table *dma_buf_phys_vec_to_sgt(struct dma_buf_attachment *attach,
					 struct p2pdma_provider *provider,
					 struct phys_vec *phys_vec,
					 size_t nr_ranges, size_t size,
					 enum dma_data_direction dir);
void dma_buf_free_sgt(struct dma_buf_attachment *attach, struct sg_table *sgt,
		      enum dma_data_direction dir);

int dma_buf_match_mapping(struct dma_buf_match_args *args,
			  const struct dma_buf_mapping_match *exp_mappings,
			  size_t exp_len);

/**
 * dma_buf_attach_map_type - The match this attachment is mapped with
 * @attach: the attachment
 *
 * Returns the entry of @attach->matches the attachment is mapped with: the
 * one the exporter selected at the last pin or map, or the exporter's
 * default, the first entry, before anything was selected. After
 * dma_buf_invalidate_mappings() it still names the type a live mapping was
 * made with, so a pending unmap can use it, until the next selection.
 */
static inline const struct dma_buf_mapping_match *
dma_buf_attach_map_type(const struct dma_buf_attachment *attach)
{
	return &attach->matches[attach->selected_idx];
}

/*
 * Internal: Selection is made by exporters through &dma_buf_ops.select_mapping
 * in dma_buf_pin() or map paths. Importers never call this.
 */
int dma_buf_mapping_select(struct dma_buf_attachment *attach);
void dma_buf_mapping_unselect(struct dma_buf_attachment *attach,
			      bool was_selected);

/**
 * dma_buf_attach_accepts - Is @type in the attachment's match set
 * @attach: the attachment
 * @type: a mapping type
 *
 * Called by exporters deciding placement in pin or select_mapping to determine
 * whether the importer could be served with @type.
 */
static inline bool
dma_buf_attach_accepts(const struct dma_buf_attachment *attach,
		       const struct dma_buf_mapping_type *type)
{
	unsigned int i;

	for (i = 0; i < attach->nr_matches; i++)
		if (attach->matches[i].type == type)
			return true;
	return false;
}

/* Internal: &dma_buf_mapping_sgt_type.map / .unmap, implemented in dma-buf.c */
struct dma_buf_mapping *dma_buf_sgt_type_map(struct dma_buf_attachment *attach,
					     enum dma_data_direction dir);
void dma_buf_sgt_type_unmap(struct dma_buf_attachment *attach,
			    struct dma_buf_mapping *mapping,
			    enum dma_data_direction dir);

/*
 * DMA Mapped Scatterlist Type
 *
 * When this type is matched the map/unmap functions are:
 *
 *  dma_buf_map_sgt()
 *  dma_buf_unmap_sgt()
 *
 * The struct sg_table returned by those functions has only the DMA portions
 * available. The caller must not try to use the struct page * information.
 *
 * importing_dma_device is the device passed to dma_buf_dynamic_attach(),
 * recorded at match time, and is passed to the DMA API to provide the
 * dma_addr_t's.
 */
extern struct dma_buf_mapping_type dma_buf_mapping_sgt_type;

/**
 * struct dma_buf_sgt_mapping - dma_buf_mapping_map() result for the SGT type
 * @base: common header, @base.type == &dma_buf_mapping_sgt_type
 * @sgt: the DMA mapped scatterlist, as the SGT type's own map function
 *       returns it
 */
struct dma_buf_sgt_mapping {
	struct dma_buf_mapping base;
	struct sg_table *sgt;
};

static inline struct sg_table *
dma_buf_sgt_mapping_table(struct dma_buf_mapping *mapping)
{
	if (WARN_ON(mapping->type != &dma_buf_mapping_sgt_type))
		return NULL;
	return container_of(mapping, struct dma_buf_sgt_mapping, base)->sgt;
}

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
	if (dma_buf_attach_map_type(attach)->type != &dma_buf_mapping_sgt_type)
		return NULL;
	return dma_buf_attach_map_type(attach)->sgt_data.importing_dma_device;
}

/**
 * dma_buf_sgt_p2p_allowed - True if MMIO memory can be used peer to peer
 * @attach: sgt mapping type attachment
 *
 * Should be called by exporters, returns true if the exporter's
 * DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE was matched.
 *
 * Reflects the current selection, so is only meaningful inside the
 * exporter's pin and map callbacks. For attachments which may be unmapped use
 * dma_buf_attach_accepts_sgt_p2p().
 */
static inline bool dma_buf_sgt_p2p_allowed(struct dma_buf_attachment *attach)
{
	if (dma_buf_attach_map_type(attach)->type != &dma_buf_mapping_sgt_type)
		return false;
	return dma_buf_attach_map_type(attach)->sgt_data.exporter_requires_p2p ==
	       DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE;
}

/**
 * dma_buf_attach_accepts_sgt_p2p - Could this attachment take a P2P SGT?
 * @attach: any attachment
 *
 * Like dma_buf_sgt_p2p_allowed() but valid for attachments that are not mapped.
 * Instead determines if p2p is part of the current possible match set rather
 * than the current selection. Called by exporters walking all attachments in
 * their pin path.
 */
static inline bool
dma_buf_attach_accepts_sgt_p2p(const struct dma_buf_attachment *attach)
{
	unsigned int i;

	for (i = 0; i < attach->nr_matches; i++)
		if (attach->matches[i].type == &dma_buf_mapping_sgt_type &&
		    attach->matches[i].sgt_data.exporter_requires_p2p ==
			    DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE)
			return true;
	return false;
}

static inline const struct dma_buf_mapping_sgt_exp_ops *
dma_buf_get_sgt_ops(struct dma_buf_attachment *attach)
{
	if (dma_buf_attach_map_type(attach)->type != &dma_buf_mapping_sgt_type)
		return NULL;
	return container_of(dma_buf_attach_map_type(attach)->exp_ops,
			    struct dma_buf_mapping_sgt_exp_ops, ops);
}

/*
 * Importer SGT entry. Usable in a static table: the importing device is
 * supplied by dma_buf_dynamic_attach() and recorded in the matched entry.
 */
#define DMA_BUF_IMAPPING_SGT(_importer_accepts_p2p)                         \
	((struct dma_buf_mapping_match){                                    \
		.type = &dma_buf_mapping_sgt_type,                          \
		.sgt_data = { .importer_accepts_p2p = (_importer_accepts_p2p) } })
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

/*
 * Only matches if the importer accepts P2P. Whether the importing device can
 * actually reach the exporter's MMIO is left to the exporter's map, for
 * exporters whose importers need not be PCI devices.
 */
static inline struct dma_buf_mapping_match
DMA_BUF_EMAPPING_SGT_REQUIRES_P2P(const struct dma_buf_mapping_sgt_exp_ops *exp_ops)
{
	struct dma_buf_mapping_match match = DMA_BUF_EMAPPING_SGT(exp_ops);

	match.sgt_data.exporter_requires_p2p = DMA_SGT_EXPORTER_REQUIRES_P2P;
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
