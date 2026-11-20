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

	/**
	 * @map:
	 *
	 * Produce a mapping of this type for @attach, for which this type has
	 * already been selected. Called by dma_buf_mapping_map() with the
	 * dma_resv lock held. The result embeds &struct dma_buf_mapping, which
	 * the core fills in once this returns. A map that may unmap its result
	 * on its own unwind must record the entry in it first, since every
	 * unmap reads it. Responsible for pin-on-map of static importers if
	 * the type supports them.
	 */
	struct dma_buf_mapping *(*map)(struct dma_buf_attachment *attach,
				       enum dma_data_direction dir);

	/**
	 * @unmap:
	 *
	 * Release a mapping returned by @map. Called by dma_buf_mapping_unmap()
	 * with the dma_resv lock held and with dma_buf_attach_map_type()
	 * naming the entry the mapping was made with, which need not be the
	 * attachment's current selection.
	 */
	void (*unmap)(struct dma_buf_attachment *attach,
		      struct dma_buf_mapping *mapping,
		      enum dma_data_direction dir);
};

/**
 * struct dma_buf_mapping - header of a mapping made through an attachment
 * @match: The entry of &dma_buf_attachment.matches the mapping was made with.
 *         The mapping is torn down through it, whatever the attachment has
 *         selected by then.
 *
 * A mapping is an object allocated by whoever produces it, the exporter for
 * the SGT type, embedding this header. The core fills the header in, at the
 * latest once the producer's map callback has returned, and reads it to tear
 * the mapping down.
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

struct dma_buf_mapping *dma_buf_mapping_map(struct dma_buf_attachment *attach,
					    enum dma_data_direction dir);
void dma_buf_mapping_unmap(struct dma_buf_attachment *attach,
			   struct dma_buf_mapping *mapping,
			   enum dma_data_direction dir);

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
 * Returns: the entry of @attach->matches the attachment is mapped with.
 */
static inline const struct dma_buf_mapping_match *
dma_buf_attach_map_type(const struct dma_buf_attachment *attach)
{
	return &attach->matches[attach->selected_idx];
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
 */
struct dma_buf_mapping_sgt_exp_ops {
	struct dma_buf_mapping_exp_ops ops;

	/**
	 * @map_dma_buf:
	 *
	 * This is called by dma_buf_map_sgt() and is used to map a
	 * shared &dma_buf into device address space, and it is mandatory. It
	 * can only be called if &dma_buf_ops.attach has been called successfully.
	 *
	 * This call may sleep, e.g. when the backing storage first needs to be
	 * allocated, or moved to a location suitable for all currently attached
	 * devices.
	 *
	 * Note that any specific buffer attributes required for this function
	 * should get added to device_dma_parameters accessible via
	 * &device.dma_params from the &dma_buf_attachment. The
	 * &dma_buf_ops.attach callback should also check these constraints.
	 *
	 * If this is being called for the first time, the exporter can now
	 * choose to scan through the list of attachments for this buffer,
	 * collate the requirements of the attached devices, and choose an
	 * appropriate backing storage for the buffer.
	 *
	 * Based on enum dma_data_direction, it might be possible to have
	 * multiple users accessing at the same time (for reading, maybe), or
	 * any other kind of sharing that the exporter might wish to make
	 * available to buffer-users.
	 *
	 * This is always called with the dmabuf->resv object locked.
	 *
	 * Note that for non-dynamic exporters the driver must guarantee
	 * that the memory is available for use and cleared of any old data by
	 * the time this function returns.  Drivers which pipeline their buffer
	 * moves internally must wait for all moves and clears to complete.
	 * Dynamic exporters do not need to follow this rule: For non-dynamic
	 * importers the buffer is already pinned through &dma_buf_ops.pin,
	 * which has the same requirements. Dynamic importers otoh are required
	 * to obey the
	 * dma_resv fences.
	 *
	 * Returns:
	 *
	 * A &struct dma_buf_sgt_mapping allocated by the exporter, whose
	 * &dma_buf_sgt_mapping.sgt is the scatter list of the backing storage
	 * of the DMA buffer, already mapped into the device address space of
	 * the &device attached with the provided &dma_buf_attachment. The
	 * addresses and lengths in the scatter list are PAGE_SIZE aligned.
	 *
	 * On failure, returns a negative error value wrapped into a pointer.
	 * May also return -EINTR when a signal was received while being
	 * blocked.
	 *
	 * The mapping is handed to the DMA-BUF code and comes back through
	 * @unmap_dma_buf. An exporter that maps an attachment once may keep
	 * one mapping per attachment and return it on every call, see
	 * &struct dma_buf_sgt_mapping; the core does any other caching.
	 */
	struct dma_buf_sgt_mapping *(*map_dma_buf)(struct dma_buf_attachment *attach,
						   enum dma_data_direction dir);

	/**
	 * @unmap_dma_buf:
	 *
	 * This is called by dma_buf_unmap_sgt() and should unmap and
	 * release the &struct dma_buf_sgt_mapping allocated in @map_dma_buf,
	 * and it is mandatory. For static dma_buf handling this might also
	 * unpin the backing storage if this is the last mapping of the DMA
	 * buffer.
	 */
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

/**
 * dma_buf_sgt_mapping_table - The sg_table for a mapping of the SGT type
 * @mapping: what dma_buf_mapping_map() returned
 *
 * For importers that listed more than one type and got the SGT type.
 *
 * Returns: the embedded &struct sg_table, or NULL if @mapping is not of the
 * SGT type.
 */
static inline struct sg_table *
dma_buf_sgt_mapping_table(struct dma_buf_mapping *mapping)
{
	if (WARN_ON(dma_buf_mapping_type_of(mapping) != &dma_buf_mapping_sgt_type))
		return NULL;
	return &container_of(mapping, struct dma_buf_sgt_mapping, base)->sgt;
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
