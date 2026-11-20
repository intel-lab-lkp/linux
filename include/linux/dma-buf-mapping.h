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
	 * Called during attach from dma_buf_match_mapping(). @exp and @imp are
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
};

/**
 * struct dma_buf_mapping - header of a mapping made through an attachment
 * @match: The match set entry the mapping was made with
 *
 * A mapping is an object allocated by whoever produces it, the exporter for
 * the SGT type, embedding this header. The core fills the header in once the
 * producer's map callback has returned and reads it to tear the mapping down.
 */
struct dma_buf_mapping {
	const struct dma_buf_mapping_match *match;
};

/**
 * dma_buf_mapping_type_of - The type a mapping was made with
 * @mapping: the mapping
 *
 * Returns: the type of the match set entry recorded in @mapping.
 */
static inline const struct dma_buf_mapping_type *
dma_buf_mapping_type_of(const struct dma_buf_mapping *mapping)
{
	return mapping->match->type;
}

struct dma_buf_sgt_mapping *
dma_buf_phys_vec_to_sgt(struct dma_buf_attachment *attach,
			struct p2pdma_provider *provider,
			struct phys_vec *phys_vec, size_t nr_ranges,
			size_t size, enum dma_data_direction dir);
void dma_buf_free_sgt(struct dma_buf_attachment *attach,
		      struct dma_buf_sgt_mapping *mapping,
		      enum dma_data_direction dir);

int dma_buf_match_mapping(struct dma_buf_match_args *args,
			  const struct dma_buf_mapping_match *exp_mappings,
			  size_t exp_len);

/**
 * dma_buf_attach_map_type - The match this attachment is mapped with
 * @attach: the attachment
 *
 * Returns: the entry of @attach->matches in force for the attachment: the
 * exporter's default, which is the first entry.
 */
static inline const struct dma_buf_mapping_match *
dma_buf_attach_map_type(const struct dma_buf_attachment *attach)
{
	return &attach->matches[0];
}

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
extern const struct dma_buf_mapping_type dma_buf_mapping_sgt_type;

/**
 * struct dma_buf_sgt_mapping - A mapping of the SGT type
 * @base: Common header, filled in by the core
 * @sgt: The DMA mapped scatterlist, filled in by the exporter
 *
 * Allocated by the exporter's &dma_buf_mapping_sgt_exp_ops.map_dma_buf, which
 * may embed it in a larger structure of its own, and released by its
 * unmap_dma_buf. The core only fills in @base, with the same values for every
 * map of an attachment, so an exporter that keeps one table per attachment
 * may keep one of these per attachment and return it on every map. A mapping
 * must not be shared between attachments. Importers are handed @sgt by
 * dma_buf_map_sgt().
 */
struct dma_buf_sgt_mapping {
	struct dma_buf_mapping base;
	struct sg_table sgt;
};

/**
 * struct dma_buf_mapping_sgt_exp_ops - Exporter ops of the SGT type
 * @ops: Type tag
 * @map_dma_buf: Produce a &struct dma_buf_sgt_mapping for @attach, with
 *               &dma_buf_sgt_mapping.sgt mapped for the device returned by
 *               dma_buf_sgt_dma_device(). Called with the dma_resv lock held.
 *               Returns the mapping, or an ERR_PTR; NULL is taken as -ENOMEM.
 * @unmap_dma_buf: Unmap and release a mapping returned by @map_dma_buf.
 */
struct dma_buf_mapping_sgt_exp_ops {
	struct dma_buf_mapping_exp_ops ops;
	struct dma_buf_sgt_mapping *(*map_dma_buf)(struct dma_buf_attachment *attach,
						   enum dma_data_direction dir);
	void (*unmap_dma_buf)(struct dma_buf_attachment *attach,
			      struct dma_buf_sgt_mapping *mapping,
			      enum dma_data_direction dir);
};

/**
 * dma_buf_sgt_dma_device - Return the device to use for DMA mapping
 * @attach: sgt mapping type attachment
 *
 * Called by the exporter to get the struct device to pass to the DMA API
 * during the map callback. The unmap callback takes it from the mapping with
 * dma_buf_sgt_mapping_dma_device().
 *
 * Returns: the importing device, or NULL if the attachment is not mapped with
 * the SGT type.
 */
static inline struct device *
dma_buf_sgt_dma_device(struct dma_buf_attachment *attach)
{
	if (dma_buf_attach_map_type(attach)->type != &dma_buf_mapping_sgt_type)
		return NULL;
	return dma_buf_attach_map_type(attach)->sgt_data.importing_dma_device;
}

static inline const struct dma_buf_mapping_sgt_exp_ops *
dma_buf_get_sgt_ops(struct dma_buf_attachment *attach)
{
	if (dma_buf_attach_map_type(attach)->type != &dma_buf_mapping_sgt_type)
		return NULL;
	return container_of(dma_buf_attach_map_type(attach)->exp_ops,
			    struct dma_buf_mapping_sgt_exp_ops, ops);
}

/**
 * dma_buf_sgt_mapping_dma_device - Return the device a mapping was made for
 * @mapping: the mapping passed to the exporter's unmap callback
 *
 * Called by the exporter to get the struct device to pass to the DMA API
 * during the unmap callback. It comes from the match set entry recorded in
 * the mapping, so it is the device the mapping was DMA mapped for whatever
 * the attachment is mapped with by the time of the unmap.
 *
 * Returns: the device the mapping was DMA mapped for.
 */
static inline struct device *
dma_buf_sgt_mapping_dma_device(const struct dma_buf_sgt_mapping *mapping)
{
	return mapping->base.match->sgt_data.importing_dma_device;
}

/*
 * Importer SGT entry. Usable in a static table: the importing device is
 * supplied by dma_buf_dynamic_attach() and recorded in the matched entry.
 */
#define DMA_BUF_IMAPPING_SGT()                                              \
	((struct dma_buf_mapping_match){ .type = &dma_buf_mapping_sgt_type })
#define DMA_BUF_EMAPPING_SGT(_exp_ops)                                      \
	((struct dma_buf_mapping_match){ .type = &dma_buf_mapping_sgt_type, \
					 .exp_ops = &((_exp_ops)->ops) })

/* Match on behalf of an exporter that has no match_mapping() */
int dma_buf_sgt_compat_match_mapping(struct dma_buf_match_args *args);

/*
 * dma_buf_ops initializer helper for simple drivers that use a single
 * SGT map/unmap operation.
 */
#define DMA_BUF_SIMPLE_SGT_EXP_MATCH(_map, _unmap)                       \
	.single_exporter_match = &((const struct dma_buf_mapping_match){ \
		.type = &dma_buf_mapping_sgt_type,                       \
		.exp_ops = &((const struct dma_buf_mapping_sgt_exp_ops){ \
			.map_dma_buf = _map,                             \
			.unmap_dma_buf = _unmap,                         \
		}).ops,                                                  \
	})

#endif
