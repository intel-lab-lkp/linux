/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _TOOLS_LINUX_PIDFD_H
#define _TOOLS_LINUX_PIDFD_H

/*
 * Some systems have issues with the linux/fcntl.h import in linux/pidfd.h, so
 * work around this by setting the header guard.
 */
#define _LINUX_FCNTL_H
#include "../../../include/uapi/linux/pidfd.h"
#undef _LINUX_FCNTL_H

#endif /* _TOOLS_LINUX_PIDFD_H */
