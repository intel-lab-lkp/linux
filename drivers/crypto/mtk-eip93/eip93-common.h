/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2019 - 2021
 *
 * Richard van Schagen <vschagen@icloud.com>
 * Christian Marangi <ansuelsmth@gmail.com
 */

#ifndef _EIP93_COMMON_H_
#define _EIP93_COMMON_H_

#include "eip93-main.h"

void *mtk_get_descriptor(struct mtk_device *mtk);
int mtk_put_descriptor(struct mtk_device *mtk, struct eip93_descriptor *desc);

void mtk_set_sa_record(struct sa_record *sa_record, const unsigned int keylen,
		       const u32 flags);

int mtk_parse_ctrl_stat_err(struct mtk_device *mtk, int err);

int mtk_authenc_setkey(struct crypto_aead *aead, struct sa_record *sa,
		       const u8 *authkey, unsigned int authkeylen);

#endif /* _EIP93_COMMON_H_ */
