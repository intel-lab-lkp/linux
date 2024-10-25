/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/binder_genl.yaml */
/* YNL-GEN kernel header */

#ifndef _LINUX_BINDER_GENL_GEN_H
#define _LINUX_BINDER_GENL_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/linux/binder_genl.h>

int binder_genl_nl_set_doit(struct sk_buff *skb, struct genl_info *info);

extern struct genl_family binder_genl_nl_family;

#endif /* _LINUX_BINDER_GENL_GEN_H */
