#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from lib.py import bkg, ip, ksft_exit, ksft_run, ksft_ge, ksft_true
from lib.py import NetNS, NetNSEnter, RtnlAddrFamily, RtnlFamily
import socket
import struct
import time
import types

IPV4_ALL_HOSTS_MULTICAST = b'\xe0\x00\x00\x01'

def dump_mcaddr_check() -> None:
    """
    Verify that at least one interface has the IPv4 all-hosts multicast address.
    At least the loopback interface should have this address.
    """

    rtnl = RtnlAddrFamily()
    addresses = rtnl.getmulticast({"ifa-family": socket.AF_INET}, dump=True)

    all_host_multicasts = [
        addr for addr in addresses if addr['multicast'] == IPV4_ALL_HOSTS_MULTICAST
    ]

    ksft_ge(len(all_host_multicasts), 1,
            "No interface found with the IPv4 all-hosts multicast address")

def ipv4_devconf_notify() -> None:
    """
    Configure an interface and set ipv4-devconf values through netlink
    to verify that the appropriate netlink notifications are being sent.
    """

    with NetNS() as ns:
        with NetNSEnter(str(ns)):
            rtnl = RtnlFamily()

            ifname = "dummy1"
            ip(f"link add name {ifname} type dummy", ns=str(ns))

            link_info = ip(f"link show dev {ifname}", ns=str(ns), json=True)
            ksft_true(bool(link_info), f"Failed to retrieve link info for {ifname}")
            ifindex = link_info[0]["ifindex"]
            notification_found = False

            # YNL do not support netconf notifications yet
            with bkg(f"ip monitor", ns=str(ns)) as cmd_obj:
                original_add_attr = rtnl._add_attr
                time.sleep(0.5)

                # Currently YNL has a bug for applying devconf values,
                # this hack fixes it. In essence, YNL is declaring an
                # array of u32 values, while kernel expects a nested attribute
                # on set operation.
                def patched_add_attr(self, space, name, value, search_attrs):
                    if name == 'conf' and value == b"MAGIC_CONF":
                        fwd_attr = struct.pack("=HHI", 8, 1, 1)
                        proxy_arp_attr = struct.pack("=HHI", 8, 3, 1)
                        rp_filter_attr = struct.pack("=HHI", 8, 8, 1)
                        ignore_routes_attr = struct.pack("=HHI", 8, 29, 1)

                        return struct.pack("=HH", 36, 0x8001) + fwd_attr \
                                + proxy_arp_attr \
                                + rp_filter_attr \
                                + ignore_routes_attr

                    return original_add_attr(space, name, value, search_attrs)

                rtnl._add_attr = types.MethodType(patched_add_attr, rtnl)

                req = {
                    "ifi-index": ifindex,
                    "af-spec": {
                        "inet": {
                            "conf": b"MAGIC_CONF"
                        }
                    }
                }
                rtnl.newlink(req)
                time.sleep(0.5)

    ksft_true(f"inet {ifname} ignore_routes_with_linkdown on" in cmd_obj.stdout,
              f"No 'ignore_routes_with_linkdown on' notificiation found for interface {ifname}")
    ksft_true(f"inet {ifname} rp_filter strict" in cmd_obj.stdout,
              f"No 'rp_filter strict' notificiation found for interface {ifname}")
    ksft_true(f"inet {ifname} proxy_neigh on" in cmd_obj.stdout,
              f"No 'proxy_neigh on' notificiation found for interface {ifname}")
    ksft_true(f"inet {ifname} forwarding on" in cmd_obj.stdout,
              f"No 'forwarding on' notificiation found for interface {ifname}")

def main() -> None:
    ksft_run([dump_mcaddr_check, ipv4_devconf_notify])
    ksft_exit()

if __name__ == "__main__":
    main()
