/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ECDH helper functions - KPP wrappings
 *
 * Copyright (C) 2017 Intel Corporation
 */
#include <crypto/kpp.h>
#include <linux/types.h>

int compute_ecdh_secret(struct crypto_kpp *tfm, const u8 pair_public_key[64],
			u8 secret[32]);
int set_ecdh_privkey(struct crypto_kpp *tfm, const u8 private_key[32]);
int generate_ecdh_public_key(struct crypto_kpp *tfm, u8 public_key[64]);
int generate_ecdh_keys(struct crypto_kpp *tfm, u8 public_key[64]);
