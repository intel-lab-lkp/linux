/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Huawei Technologies Co., Ltd.
 *
 * Authors:
 * GONG Ruiqi <gongruiqi1@huawei.com>
 *
 * File: ima_tpm.h
 *	Hooks of TPM for IMA RoT
 */

#ifndef __IMA_IMA_TPM_H
#define __IMA_IMA_TPM_H

int ima_tpm_init(struct ima_rot *rot);
int ima_tpm_extend(struct tpm_digest *digests_arg, const void *args);
int ima_tpm_calc_boot_aggregate(struct ima_digest_data *hash);

int ima_pcr_extend(struct tpm_digest *digests_arg, int pcr);

#endif /* __IMA_IMA_TPM_H */
