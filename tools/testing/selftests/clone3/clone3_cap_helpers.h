/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __CLONE3_CAP_HELPERS_H
#define __CLONE3_CAP_HELPERS_H

#include <linux/capability.h>

/*
 * Compatible with older version
 * header file without defined
 * CAP_CHECKPOINT_RESTORE.
 */
#ifndef CAP_CHECKPOINT_RESTORE
#define CAP_CHECKPOINT_RESTORE 40
#endif

/*
 * Removed the libcap library dependency.
 * So declare them here directly.
 */
int capget(cap_user_header_t header, cap_user_data_t data);
int capset(cap_user_header_t header, const cap_user_data_t data);

#endif
