/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_RPMSG_H__
#define __QDA_RPMSG_H__

#include "qda_drv.h"

/*
 * Transport layer registration
 */
int qda_rpmsg_register(void);
void qda_rpmsg_unregister(void);

#endif /* __QDA_RPMSG_H__ */
