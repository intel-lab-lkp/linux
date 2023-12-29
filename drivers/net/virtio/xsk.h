/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __XSK_H__
#define __XSK_H__

int virtnet_xsk_pool_setup(struct net_device *dev, struct netdev_bpf *xdp);
#endif
