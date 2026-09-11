// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Intel Corporation */

#include <linux/net/intel/libie/irq.h>
#include <linux/net/intel/virtchnl2.h>

/**
 * libie_irq_init - init irq for whole device
 * @irq: pointer to libie_irq structure
 * @pdev: pdev which interrupts is used
 * @min: number of static interrupts
 * @max: max - min is the number of dynamic interrupts
 *
 * Function will call pci_alloc_irq_vectors(). Minimum vectors value is a number
 * of LIBIE_IRQ_STATIC. It means that if there is no enough interrupts for
 * all static interrupts this function will return -ENOSPC.
 *
 * If there is no support for dynamic interrupts allocation
 * pci_alloc_irq_vectors() is called with max value, otherwise max is equal to
 * min. There is no need to change the limits values for LIBIE_IRQ_DYNAMIC.
 * The libie_irq_alloc() function is changing the LIBIE_IRQ_DYNAMIC to
 * LIBIE_IRQ_STATIC when there is no such support. User of this helpers doesn't
 * have to think if there is dynamic support or there isn't.
 *
 * Limits' max is the last index that can be used. Assuming 0 index is valid
 * (which is true here) there is need to subtract one from the number of
 * interrupts.
 * Ex. min = 5 -> limits.min = 0, limits.max = 4; indexes 0, 1, 2, 3, 4
 *
 * Return: 0 in case of success otherwise -ENOSPC or -EINVAL when called
 *         with min == 0
 */
int libie_irq_init(struct libie_irq *irq, struct pci_dev *pdev,
		   int min, int max)
{
	int vectors;

	/* At least one static vector needs to be allocated */
	if (!min || min > max)
		return -EINVAL;

	irq->limits[LIBIE_IRQ_STATIC].min = 0;
	irq->limits[LIBIE_IRQ_STATIC].max = min - 1;
	irq->limits[LIBIE_IRQ_DYNAMIC].min = min;
	irq->limits[LIBIE_IRQ_DYNAMIC].max = max - 1;

	if (!pci_msix_can_alloc_dyn(pdev))
		irq->limits[LIBIE_IRQ_STATIC].max = max - 1;
	else
		/* max can be lowered, as rest can be allocated dynamically */
		max = min;

	vectors = pci_alloc_irq_vectors(pdev, min, max, PCI_IRQ_MSIX);
	if (vectors < 0)
		return vectors;

	/* static vectors needs to be lowered if there is not enough irqs */
	if (irq->limits[LIBIE_IRQ_STATIC].max + 1 > vectors)
		irq->limits[LIBIE_IRQ_STATIC].max = vectors - 1;

	irq->pdev = pdev;
	xa_init_flags(&irq->entries, XA_FLAGS_ALLOC);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(libie_irq_init, "LIBIE_IRQ");

/**
 * libie_irq_deinit - deinit irq initialized by libie_irq_init()
 * @irq: libie_irq stored in driver data
 *
 * Should be called after all irqs are cleaned by libie_put_irq()
 */
void libie_irq_deinit(struct libie_irq *irq)
{
	struct libie_irq_entry *entry;
	unsigned long i;

	if (!irq->pdev)
		return;

	xa_for_each(&irq->entries, i, entry)
		kfree(entry);
	xa_destroy(&irq->entries);
	pci_free_irq_vectors(irq->pdev);
	irq->pdev = NULL;
}
EXPORT_SYMBOL_NS_GPL(libie_irq_deinit, "LIBIE_IRQ");

/**
 * libie_get_irq - get new allocated entry for specific irq type
 * @irq: libie_irq structure used to get limits and entries xarray
 * @type: one of the enum libie_irq_type
 *
 * Return: struct libie_irq_entry * in case of success or NULL otherwise
 */
static struct libie_irq_entry *libie_get_irq(struct libie_irq *irq,
					     enum libie_irq_type type)
{
	struct libie_irq_entry *entry;
	unsigned int index;

	if (!irq->pdev)
		return NULL;

	/* Change entry type if dynamic isn't supported. Reflect correct type
	 * to not call pci_msix_free_irq() during freeing this irq.
	 */
	if (!pci_msix_can_alloc_dyn(irq->pdev))
		type = LIBIE_IRQ_STATIC;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return NULL;

	if (xa_alloc(&irq->entries, &index, entry, irq->limits[type],
		     GFP_KERNEL))
		goto free_entry;

	entry->index = index;
	entry->type = type;

	return entry;

free_entry:
	kfree(entry);
	return NULL;
}

/**
 * libie_put_irq - inform that the irq isn't used anymore
 * @irq: libie_irq structure used to get entries xarray
 * @index: software 0-based index of irq to be marked as unused
 */
void libie_put_irq(struct libie_irq *irq, unsigned int index)
{
	struct libie_irq_entry *entry;

	entry = xa_erase(&irq->entries, index);
	kfree(entry);
}
EXPORT_SYMBOL_NS_GPL(libie_put_irq, "LIBIE_IRQ");

/**
 * libie_irq_alloc - alloc new irq, or get existing one in case of static
 * @irq: libie_irq structure
 * @type: one of enum libie_irq_type
 *
 *
 * For LIBIE_IRQ_DYNAMIC function allocs new interrupt and return it.
 * For LIBIE_IRQ_STATIC function returns already allocated one.
 *
 * The function should be called for getting irq information (index and virq)
 * for specific irq type. Returned information should be stored to use index for
 * gathering HW specific information and virq to request/free irq line.
 *
 * Calling this function with LIBIE_IRQ_DYNAMIC type when dynamic irq isn't
 * support is fine and will use limits from static field set in
 * libie_irq_init().
 *
 * Return: map.index = -ENOENT if there is no free interrupts of chosen type
 *	   map.index = -EINVAL if pci_irq_vector() fails
 *	   correct map.index and map.virq if everything is fine
 */
struct msi_map libie_irq_alloc(struct libie_irq *irq, enum libie_irq_type type)
{
	struct msi_map map = { .index = -ENOENT,
			       .virq = 0 };
	struct libie_irq_entry *entry;

	entry = libie_get_irq(irq, type);
	if (!entry)
		return map;

	if (entry->type == LIBIE_IRQ_DYNAMIC) {
		map = pci_msix_alloc_irq_at(irq->pdev, entry->index, NULL);
		if (map.index < 0)
			goto put_irq;
	} else {
		map.index = entry->index;
		map.virq = pci_irq_vector(irq->pdev, map.index);
		if (map.virq < 0) {
			/* In dynamic case error is in .index, put it there
			 * also for static case to allow the caller always look
			 * for an error in the same place.
			 */
			map.index = map.virq;
			goto put_irq;
		}
	}

	return map;

put_irq:
	libie_put_irq(irq, entry->index);
	return map;
}
EXPORT_SYMBOL_NS_GPL(libie_irq_alloc, "LIBIE_IRQ");

/**
 * libie_irq_free - free irq, allocated using libie_alloc_irq()
 * @irq: libie_irq structure
 * @map: msi_map structure returned from libie_alloc_irq()
 *
 * In case of dynamic allocation and LIBIE_IRQ_DYNAMIC type pci_msix_free_irq()
 * is called. Otherwise only free driver irq entry related resources.
 *
 * It is safe to call this function with map that doesn't exist in xarray
 * as long as the map.virq is 0 or negative. It is true when libie_irq_alloc()
 * has failed.
 */
void libie_irq_free(struct libie_irq *irq, struct msi_map map)
{
	struct libie_irq_entry *entry;

	if (map.virq <= 0 || map.index < 0)
		return;

	entry = xa_load(&irq->entries, map.index);
	if (!entry)
		return;

	if (entry->type == LIBIE_IRQ_DYNAMIC)
		pci_msix_free_irq(irq->pdev, map);

	libie_put_irq(irq, map.index);
}
EXPORT_SYMBOL_NS_GPL(libie_irq_free, "LIBIE_IRQ");

/**
 * libie_irq_reserve - reserve a interrupt index without allocating MSI-X
 * @irq: libie_irq structure containing interrupt management data
 *
 * This function reserves an interrupt index from the dynamic range without
 * actually allocating the corresponding MSI-X vector. The reserved index can
 * be used for hardware queue configuration before the actual interrupt
 * allocation. The caller should use libie_put_irq() to release the reserved
 * index when no longer needed.
 *
 * Return: Reserved interrupt index on success, or -ENOENT if no dynamic
 *         interrupt indices are available.
 */
int libie_irq_reserve(struct libie_irq *irq)
{
	struct libie_irq_entry *ent = libie_get_irq(irq, LIBIE_IRQ_DYNAMIC);

	if (!ent)
		return -ENOENT;

	return ent->index;
}
EXPORT_SYMBOL_NS_GPL(libie_irq_reserve, "LIBIE_IRQ");

/**
 * libie_irq_create_info - Save vectors information from firmware
 * @info: parsed information is stored here
 * @caps: virtchannel capabilities
 * @vectors: vector information from firmware to be parsed
 * @num_vectors: number of vectors
 *
 * Return: 0 on success, negative on failure.
 */
int libie_irq_create_info(struct libie_irq_info *info,
			  const struct virtchnl2_get_capabilities *caps,
			  const struct virtchnl2_alloc_vectors *vectors,
			  const u16 num_vectors)
{
	const struct virtchnl2_vector_chunks *chunks = &vectors->vchunks;
	struct libie_hw_vector *vector;
	const int mb_vectors = 1;
	int reg_cnt, all_vectors;

	if (le16_to_cpu(vectors->num_vectors) < num_vectors)
		return -EINVAL;

	all_vectors = num_vectors + mb_vectors;
	info->vectors = kzalloc_objs(*info->vectors, all_vectors);
	if (!info->vectors)
		return -ENOMEM;
	/* Mailbox irq information are stored in different places. Fill index 0
	 * of our vectors info with capabilities and rest with information
	 * from vector chunks.
	 */
	vector = &info->vectors[0];
	vector->idx = le16_to_cpu(caps->mailbox_vector_id);
	vector->regs.dyn_ctl = le32_to_cpu(caps->mailbox_dyn_ctl);
	reg_cnt = mb_vectors;

	for (int i = 0; i < le16_to_cpu(chunks->num_vchunks); i++) {
		const struct virtchnl2_vector_chunk *chunk = &chunks->vchunks[i];
		u32 dyn_spacing, itrn_spacing;
		struct libie_vec_regs reg_val;
		u16 vec_id;

		reg_val.dyn_ctl = le32_to_cpu(chunk->dynctl_reg_start);
		reg_val.itrn = le32_to_cpu(chunk->itrn_reg_start);
		reg_val.itrn_index_spacing =
			le32_to_cpu(chunk->itrn_index_spacing);

		dyn_spacing = le32_to_cpu(chunk->dynctl_reg_spacing);
		itrn_spacing = le32_to_cpu(chunk->itrn_reg_spacing);
		vec_id = le16_to_cpu(chunk->start_vector_id);

		for (int j = 0; j < le16_to_cpu(chunk->num_vectors); j++) {
			if (reg_cnt >= all_vectors)
				break;

			vector = &info->vectors[reg_cnt];

			vector->regs = reg_val;
			vector->idx = vec_id;

			reg_val.dyn_ctl += dyn_spacing;
			reg_val.itrn += itrn_spacing;

			vec_id += 1;
			reg_cnt += 1;
		}
	}

	if (reg_cnt != all_vectors) {
		kfree(info->vectors);
		info->vectors = NULL;
		return -EINVAL;
	}

	info->num = all_vectors;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(libie_irq_create_info, "LIBIE_IRQ");

/**
 * libie_irq_destroy_info - free memory allocated during building irq_info
 * @info: libie_irq_info struct to be freed
 */
void libie_irq_destroy_info(struct libie_irq_info *info)
{
	kfree(info->vectors);
	info->vectors = NULL;
	info->num = 0;
}
EXPORT_SYMBOL_NS_GPL(libie_irq_destroy_info, "LIBIE_IRQ");

/* Module */

MODULE_DESCRIPTION("Helper functions for managing MSI-X in driver");
MODULE_LICENSE("GPL");
