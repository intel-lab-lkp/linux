/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_H
#define _LINUX_UNWIND_USER_H

#include <linux/unwind_user_types.h>

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries);

#endif /* _LINUX_UNWIND_USER_H */
