/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2010 - 2015, Intel Corporation.
 */

#ifndef __IA_CSS_QUEUE_COMM_H
#define __IA_CSS_QUEUE_COMM_H

#include <linux/build_bug.h>
#include <linux/types.h>

#include "type_support.h"

/****************************************************************
 *
 * Portable Data structures
 *
 ****************************************************************/
/*
 * @brief Data structure for the circular descriptor.
 */
typedef struct ia_css_circbuf_desc_s ia_css_circbuf_desc_t;
struct ia_css_circbuf_desc_s {
	u8 size;	/* the maximum number of elements*/
	u8 step;   /* number of bytes per element */
	u8 start;	/* index of the oldest element */
	u8 end;	/* index at which to write the new element */
};

#define SIZE_OF_IA_CSS_CIRCBUF_DESC_S_STRUCT				\
	(4 * sizeof(uint8_t))

static_assert(sizeof(struct ia_css_circbuf_desc_s) == SIZE_OF_IA_CSS_CIRCBUF_DESC_S_STRUCT);

/*
 * @brief Data structure for the circular buffer element.
 */
typedef struct ia_css_circbuf_elem_s ia_css_circbuf_elem_t;
struct ia_css_circbuf_elem_s {
	u32 val;	/* the value stored in the element */
};

#define SIZE_OF_IA_CSS_CIRCBUF_ELEM_S_STRUCT				\
	(sizeof(uint32_t))

static_assert(sizeof(struct ia_css_circbuf_elem_s) == SIZE_OF_IA_CSS_CIRCBUF_ELEM_S_STRUCT);

/****************************************************************
 *
 * Inline functions.
 *
 ****************************************************************/
/*
 * @brief Test if the circular buffer is empty.
 *
 * @param cb_desc The pointer to the circular buffer descriptor.
 *
 * @return
 *	- true when it is empty.
 *	- false when it is not empty.
 */
static inline bool ia_css_circbuf_desc_is_empty(
    ia_css_circbuf_desc_t *cb_desc)
{
	return (cb_desc->end == cb_desc->start);
}

/*
 * @brief Test if the circular buffer descriptor is full.
 *
 * @param cb_desc	The pointer to the circular buffer
 *			descriptor.
 *
 * @return
 *	- true when it is full.
 *	- false when it is not full.
 */
static inline bool ia_css_circbuf_desc_is_full(
    ia_css_circbuf_desc_t *cb_desc)
{
	return ((cb_desc->end + 1) % cb_desc->size) == cb_desc->start;
}

/*
 * @brief Get the offset between two positions in the circular buffer
 * descriptor.
 * Get the offset from the source position to the terminal position,
 * along the direction in which the new elements come in.
 *
 * @param cb_desc	The pointer to the circular buffer descriptor.
 * @param src_pos	The source position.
 * @param dest_pos	The terminal position.
 *
 * @return the offset.
 */
static inline int ia_css_circbuf_desc_get_offset(
    ia_css_circbuf_desc_t *cb_desc,
    u32 src_pos,
    uint32_t dest_pos)
{
	int offset;

	offset = (int)(dest_pos - src_pos);
	offset += (offset < 0) ? cb_desc->size : 0;

	return offset;
}

/*
 * @brief Get the number of available elements.
 *
 * @param cb_desc The pointer to the circular buffer.
 *
 * @return The number of available elements.
 */
static inline uint32_t ia_css_circbuf_desc_get_num_elems(
    ia_css_circbuf_desc_t *cb_desc)
{
	int num;

	num = ia_css_circbuf_desc_get_offset(cb_desc,
					     cb_desc->start,
					     cb_desc->end);

	return (uint32_t)num;
}

/*
 * @brief Get the number of free elements.
 *
 * @param cb_desc The pointer to the circular buffer descriptor.
 *
 * @return: The number of free elements.
 */
static inline uint32_t ia_css_circbuf_desc_get_free_elems(
    ia_css_circbuf_desc_t *cb_desc)
{
	u32 num;

	num = ia_css_circbuf_desc_get_offset(cb_desc,
					     cb_desc->start,
					     cb_desc->end);

	return (cb_desc->size - num);
}

/*****************************************************************************
 * Queue Public Data Structures
 *****************************************************************************/

/* Queue location specifier */
/* Avoiding enums to save space */
#define IA_CSS_QUEUE_LOC_HOST 0
#define IA_CSS_QUEUE_LOC_SP   1
#define IA_CSS_QUEUE_LOC_ISP  2

/* Queue type specifier */
/* Avoiding enums to save space */
#define IA_CSS_QUEUE_TYPE_REMOTE 1

/* for DDR Allocated queues,
allocate minimum these many elements.
DDR->SP' DMEM DMA transfer needs 32byte aligned address.
Since each element size is 4 bytes, 8 elements need to be
DMAed to access single element.*/
#define IA_CSS_MIN_ELEM_COUNT    8
#define IA_CSS_DMA_XFER_MASK (IA_CSS_MIN_ELEM_COUNT - 1)

/* Remote Queue object descriptor */
struct ia_css_queue_remote {
	u32 cb_desc_addr; /*Circbuf desc address for remote queues*/
	u32 cb_elems_addr; /*Circbuf elements addr for remote queue*/
	u8 location;    /* Cell location for queue */
	u8 proc_id;     /* Processor id for queue access */
};

typedef struct ia_css_queue_remote ia_css_queue_remote_t;

#endif /* __IA_CSS_QUEUE_COMM_H */
