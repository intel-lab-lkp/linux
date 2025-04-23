// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2025, Advanced Micro Devices, Inc. */

#include <linux/bitmap.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "ionic_res.h"

int ionic_resid_init(struct ionic_resid_bits *resid, int size)
{
	int size_bytes = sizeof(long) * BITS_TO_LONGS(size);

	resid->next_id = 0;
	resid->inuse_size = size;

	resid->inuse = kzalloc(size_bytes, GFP_KERNEL);
	if (!resid->inuse)
		return -ENOMEM;

	return 0;
}

int ionic_resid_get_shared(struct ionic_resid_bits *resid, int wrap_id,
			   int next_id, int size)
{
	int id;

	id = find_next_zero_bit(resid->inuse, size, next_id);
	if (id != size) {
		set_bit(id, resid->inuse);
		return id;
	}

	id = find_next_zero_bit(resid->inuse, next_id, wrap_id);
	if (id != next_id) {
		set_bit(id, resid->inuse);
		return id;
	}

	return -ENOMEM;
}
