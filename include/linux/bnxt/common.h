/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2025, Broadcom Corporation
 *
 */

#ifndef BNXT_COMN_H
#define BNXT_COMN_H

#include <linux/bnxt/hsi.h>
#include <linux/bnxt/ulp.h>
#include <linux/auxiliary_bus.h>

struct bnxt_aux_priv {
	struct auxiliary_device aux_dev;
	struct bnxt_en_dev *edev;
	int id;
};

#endif /* BNXT_COMN_H */
