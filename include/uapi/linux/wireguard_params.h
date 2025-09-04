/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UAPI_LINUX_WIREGUARD_PARAMS_H
#define _UAPI_LINUX_WIREGUARD_PARAMS_H

#include <linux/time_types.h>
#include <linux/if.h>
#include <linux/in.h>

/* These definitions are currently needed for definitions which can't
 * be expressed directly in Documentation/netlink/specs/wireguard.yaml
 */
#define __WG_INADDR_SZ (sizeof(struct in_addr))
#define __WG_SOCKADDR_SZ (sizeof(struct sockaddr))
#define __WG_TIMESPEC_SZ (sizeof(struct __kernel_timespec))
#define __WG_IFNAMLEN (IFNAMSIZ - 1)

#endif /* _UAPI_LINUX_WIREGUARD_PARAMS_H */
