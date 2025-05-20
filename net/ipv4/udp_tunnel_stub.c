// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2020 Facebook Inc.

#include <net/udp_tunnel.h>

DEFINE_MUTEX(udp_tunnel_nic_lock);
EXPORT_SYMBOL_GPL(udp_tunnel_nic_lock);
const struct udp_tunnel_nic_ops *udp_tunnel_nic_ops;
EXPORT_SYMBOL_GPL(udp_tunnel_nic_ops);
