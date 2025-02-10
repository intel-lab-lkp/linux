/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Arm Ltd.
 *
 * Authors:
 * Stuart Yoder <stuart.yoder@arm.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * This device driver implements the TPM CRB start method
 * as defined in the TPM Service Command Response Buffer
 * Interface Over FF-A (DEN0138).
 */
#ifndef _FFA_CRB_H
#define _FFA_CRB_H

#if IS_ENABLED(CONFIG_TCG_ARM_FFA_CRB)
int ffa_crb_init(void);
int ffa_crb_get_interface_version(uint16_t *major, uint16_t *minor);
int ffa_crb_start(int request_type, int locality);
#else
static inline int ffa_crb_init(void) { return 0; }
static inline int ffa_crb_get_interface_version(uint16_t *major, uint16_t *minor) { return 0; }
static inline int ffa_crb_start(int request_type, int locality) { return 0; }
#endif

#define FFA_CRB_START_TYPE_COMMAND 0
#define FFA_CRB_START_TYPE_LOCALITY_REQUEST 1

#endif
