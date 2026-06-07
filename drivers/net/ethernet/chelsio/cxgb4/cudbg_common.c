// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2017 Chelsio Communications.  All rights reserved.
 */

#include "cxgb4.h"
#include "cudbg_if.h"
#include "cudbg_lib_common.h"

void release_scratch_buff(struct cudbg_buffer *pscratch_buff,
			  struct cudbg_buffer *pdbg_buff)
{
	pdbg_buff->size += pscratch_buff->size;
	memset(pscratch_buff->data, 0, pscratch_buff->size);
	pscratch_buff->data = NULL;
	pscratch_buff->offset = 0;
	pscratch_buff->size = 0;
}

int get_scratch_buff(struct cudbg_buffer *pdbg_buff, u32 size,
		     struct cudbg_buffer *pscratch_buff)
{
	u32 scratch_offset;
	int rc = 0;

	scratch_offset = pdbg_buff->size - size;
	if (pdbg_buff->offset > (int)scratch_offset || pdbg_buff->size < size) {
		rc = CUDBG_STATUS_NO_MEM;
		goto err;
	} else {
		pscratch_buff->data = (char *)pdbg_buff->data + scratch_offset;
		pscratch_buff->offset = 0;
		pscratch_buff->size = size;
		pdbg_buff->size -= size;
	}
err:
	return rc;
}

int cudbg_get_buff(struct cudbg_init *pdbg_init,
		   struct cudbg_buffer *pdbg_buff, u32 size,
		   struct cudbg_buffer *pin_buff)
{
	u32 offset;

	offset = pdbg_buff->offset;
	if (offset + size > pdbg_buff->size)
		return CUDBG_STATUS_NO_MEM;

	if (pdbg_init->compress_type != CUDBG_COMPRESSION_NONE) {
		if (size > pdbg_init->compress_buff_size)
			return CUDBG_STATUS_NO_MEM;

		pin_buff->data = (char *)pdbg_init->compress_buff;
		pin_buff->offset = 0;
		pin_buff->size = size;
		return 0;
	}

	pin_buff->data = (char *)pdbg_buff->data + offset;
	pin_buff->offset = offset;
	pin_buff->size = size;
	return 0;
}

void cudbg_put_buff(struct cudbg_init *pdbg_init,
		    struct cudbg_buffer *pin_buff)
{
	/* Clear compression buffer for re-use */
	if (pdbg_init->compress_type != CUDBG_COMPRESSION_NONE)
		memset(pdbg_init->compress_buff, 0,
		       pdbg_init->compress_buff_size);

	pin_buff->data = NULL;
	pin_buff->offset = 0;
	pin_buff->size = 0;
}

void cudbg_update_buff(struct cudbg_buffer *pin_buff,
		       struct cudbg_buffer *pout_buff)
{
	/* We already write to buffer provided by ethool, so just
	 * increment offset to next free space.
	 */
	pout_buff->offset += pin_buff->size;
}
