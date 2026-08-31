/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Bus width capabilities of DMA engine devices and channels.
 */
#ifndef LINUX_DMA_ENGINE_WIDTHMASK_H
#define LINUX_DMA_ENGINE_WIDTHMASK_H

#include <linux/bitmap.h>
#include <linux/dma/engine/types.h>
#include <linux/types.h>

/**
 * dma_bus_width_valid - test if a bus width is a valid one
 * @width: bus width to validate
 *
 * Return: true if @width is a valid &enum dma_slave_buswidth, false otherwise.
 */
static inline bool dma_bus_width_valid(enum dma_slave_buswidth width)
{
	switch (width) {
	case DMA_SLAVE_BUSWIDTH_UNDEFINED:
	case DMA_SLAVE_BUSWIDTH_1_BYTE:
	case DMA_SLAVE_BUSWIDTH_2_BYTES:
	case DMA_SLAVE_BUSWIDTH_3_BYTES:
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
	case DMA_SLAVE_BUSWIDTH_8_BYTES:
	case DMA_SLAVE_BUSWIDTH_16_BYTES:
	case DMA_SLAVE_BUSWIDTH_32_BYTES:
	case DMA_SLAVE_BUSWIDTH_64_BYTES:
	case DMA_SLAVE_BUSWIDTH_128_BYTES:
		return true;
	default:
		return false;
	}
}

static inline int __dma_bus_width_set_many(dma_buswidth_mask_t *mask,
					   const enum dma_slave_buswidth *widths,
					   unsigned int n_widths)
{
	for (unsigned int i = 0; i < n_widths; i++) {
		if (!dma_bus_width_valid(widths[i]))
			return -EINVAL;

		__set_bit(widths[i], mask->bits);
	}

	return 0;
}

/**
 * dma_bus_width_set_many - set the supported bus widths
 * @mask: bus width mask
 * @widths: array of supported bus widths
 * @n_widths: number of entries in @widths
 *
 * Return: 0 on success, -EINVAL if @widths contains an invalid bus width. Note
 * that the bus widths validated before the failing one are still set.
 */
#define dma_bus_width_set_many(mask, widths, n_widths) \
	__dma_bus_width_set_many(&(mask), (widths), (n_widths))

/**
 * dma_bus_width_set - set a single supported bus width
 * @mask: bus width mask
 * @width: supported bus width
 *
 * Return: 0 on success, -EINVAL if @width is invalid.
 */
#define dma_bus_width_set(mask, width) \
	__dma_bus_width_set_many(&(mask), (const enum dma_slave_buswidth[]){ (width) }, 1)

static inline int __dma_bus_width_clear(dma_buswidth_mask_t *mask,
					enum dma_slave_buswidth width)
{
	if (!dma_bus_width_valid(width))
		return -EINVAL;

	__clear_bit(width, mask->bits);

	return 0;
}

/**
 * dma_bus_width_clear - remove a bus width from a bus width mask
 * @mask: bus width mask
 * @width: bus width to clear
 *
 * Return: 0 on success, -EINVAL if @width is invalid.
 */
#define dma_bus_width_clear(mask, width) __dma_bus_width_clear(&(mask), (width))

static inline bool __dma_bus_width_test(const dma_buswidth_mask_t *mask,
					enum dma_slave_buswidth width)
{
	if (!dma_bus_width_valid(width))
		return false;

	return test_bit(width, mask->bits);
}

/**
 * dma_bus_width_test - test if a bus width is part of a bus width mask
 * @mask: bus width mask
 * @width: bus width to test
 *
 * Return: true if @width is set in @mask, false otherwise.
 */
#define dma_bus_width_test(mask, width) __dma_bus_width_test(&(mask), (width))

static inline enum dma_slave_buswidth
__dma_bus_width_min(const dma_buswidth_mask_t *mask)
{
	enum dma_slave_buswidth width = find_first_bit(mask->bits,
						       DMA_SLAVE_BUSWIDTH_MAX);

	if (width == DMA_SLAVE_BUSWIDTH_MAX)
		return DMA_SLAVE_BUSWIDTH_UNDEFINED;

	return width;
}

/**
 * dma_bus_width_min - get the smallest bus width of a bus width mask
 * @mask: bus width mask
 *
 * Return: the smallest bus width set in @mask, or
 * %DMA_SLAVE_BUSWIDTH_UNDEFINED if @mask is empty.
 */
#define dma_bus_width_min(mask) __dma_bus_width_min(&(mask))

static inline void __dma_bus_width_copy(dma_buswidth_mask_t *dst,
					const dma_buswidth_mask_t *src)
{
	bitmap_copy(dst->bits, src->bits, DMA_SLAVE_BUSWIDTH_MAX);
}

/**
 * dma_bus_width_copy - copy a bus width mask
 * @dst: bus width mask to copy to
 * @src: bus width mask to copy from
 */
#define dma_bus_width_copy(dst, src) __dma_bus_width_copy(&(dst), &(src))

static inline bool __dma_bus_width_and(dma_buswidth_mask_t *dst,
				       const dma_buswidth_mask_t *src1,
				       const dma_buswidth_mask_t *src2)
{
	return bitmap_and(dst->bits, src1->bits, src2->bits,
			  DMA_SLAVE_BUSWIDTH_MAX);
}

/**
 * dma_bus_width_and - intersect two bus width masks
 * @dst: bus width mask to store the result in
 * @src1: first bus width mask
 * @src2: second bus width mask
 *
 * Return: true if @dst has at least one bus width set, false otherwise.
 */
#define dma_bus_width_and(dst, src1, src2) \
	__dma_bus_width_and(&(dst), &(src1), &(src2))

#endif /* LINUX_DMA_ENGINE_WIDTHMASK_H */
