/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 ARM Ltd.
 */

#ifndef _FFA_COMMON_H
#define _FFA_COMMON_H

#include <linux/arm_ffa.h>
#include <linux/arm-smccc.h>
#include <linux/err.h>

#define FFA_ROOT_DEV_ID	(0)

typedef struct arm_smccc_1_2_regs ffa_value_t;

typedef void (ffa_fn)(ffa_value_t, ffa_value_t *);

bool ffa_device_is_valid(struct ffa_device *ffa_dev);
void ffa_device_match_uuid(struct ffa_device *ffa_dev, const uuid_t *uuid);
int ffa_root_device_driver_register(void);
int ffa_core_init(void);

static __always_inline bool is_ffa_root_device(const struct ffa_device *ffa_dev)
{
	return ffa_dev->id == FFA_ROOT_DEV_ID;
}

#ifdef CONFIG_ARM_FFA_SMCCC
int ffa_transport_init(ffa_fn **invoke_ffa_fn);
#else
static inline int ffa_transport_init(ffa_fn **invoke_ffa_fn)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _FFA_COMMON_H */
