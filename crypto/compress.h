/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cryptographic API.
 *
 * Copyright 2015 LG Electronics Inc.
 * Copyright (c) 2016, Intel Corporation
 * Copyright (c) 2023 Herbert Xu <herbert@gondor.apana.org.au>
 */
#ifndef _LOCAL_CRYPTO_COMPRESS_H
#define _LOCAL_CRYPTO_COMPRESS_H

#include <crypto/internal/scompress.h>
#include "internal.h"

struct acomp_req;

int crypto_init_scomp_ops_async(struct crypto_tfm *tfm);
int scomp_no_setparam(struct crypto_scomp *tfm, const u8 *param,
		      unsigned int len);

void comp_prepare_alg(struct comp_alg_common *alg);

static inline bool crypto_scomp_alg_has_setparam(struct scomp_alg *alg)
{
	return alg->setparam != scomp_no_setparam;
}

#endif	/* _LOCAL_CRYPTO_COMPRESS_H */
