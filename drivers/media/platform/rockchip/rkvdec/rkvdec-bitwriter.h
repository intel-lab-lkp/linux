/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip Video Decoder bit writer
 *
 * Copyright (C) 2026 Collabora, Ltd.
 *      Detlev Casanova <detlev.casanova@collabora.com>
 * Copyright (C) 2019 Collabora, Ltd.
 *	Boris Brezillon <boris.brezillon@collabora.com>
 */

#ifndef RKVDEC_BIT_WRITER_H_
#define RKVDEC_BIT_WRITER_H_

#include <linux/types.h>

struct rkvdec_bw_field {
	u16 offset;
	u8 len;
};

#define BW_FIELD(_offset, _len) ((struct rkvdec_bw_field){ _offset, _len })

void rkvdec_set_bw_field(u32 *buf, struct rkvdec_bw_field field, u32 value);

#endif /* RKVDEC_BIT_WRITER_H_ */
