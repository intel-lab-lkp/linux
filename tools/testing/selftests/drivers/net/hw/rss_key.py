#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Check that the RSS key a device actually uses spreads flows over all of the
entries of its indirection table.

Most drivers take their key from netdev_rss_key_fill(), which generates keys
having that property, but some bring their own and firmware sometimes
installs one of its own. This reads back the key the device reports, so it
covers wherever the key came from. See net/lib/py/rsskey.py for what the
property is and why a random key is not good enough.
"""

import random

from lib.py import ksft_run, ksft_exit, ksft_pr
from lib.py import ksft_eq
from lib.py import KsftSkipEx
from lib.py import NetDrvEnv, EthtoolFamily, cmd
from lib.py import RSS_KEY_QMAX, rss_key_assign_bit, rss_key_toeplitz, \
    rss_key_full_rank, rss_key_layout

# "define" for the ID of the Toeplitz hash function
ETH_RSS_HASH_TOP = 1

FLOW_TYPES = ("tcp4", "udp4", "tcp6", "udp6")

# How ethtool -n spells the fields of the hash input.
FIELD_NAMES = {
    "IP SA": "s",
    "IP DA": "d",
    "L3 proto": "t",
    "L4 bytes 0 & 1 [TCP/UDP src port]": "f",
    "L4 bytes 2 & 3 [TCP/UDP dst port]": "n",
    "IPv6 Flow Label": "l",
}


def _get_rss(cfg):
    """The key and indirection table of @cfg's device, or a skip."""
    rss = cfg.ethnl.rss_get({"header": {"dev-index": cfg.ifindex}})

    hkey = rss.get("hkey")
    if not hkey or not any(hkey):
        raise KsftSkipEx(f"{cfg.ifname} does not report an RSS key")

    if rss.get("hfunc") != ETH_RSS_HASH_TOP:
        raise KsftSkipEx(f"{cfg.ifname} does not use the Toeplitz hash")

    if rss.get("input-xfrm"):
        raise KsftSkipEx(f"{cfg.ifname} transforms the hash input")

    indir = rss.get("indir")
    if not indir:
        raise KsftSkipEx(f"{cfg.ifname} does not report an indirection table")

    if len(indir) & (len(indir) - 1):
        raise KsftSkipEx(f"{cfg.ifname} has {len(indir)} indirection table "
                         "entries, which is not a power of two")

    return bytes(hkey), indir


def _get_layouts(cfg):
    """The hash input layouts in use, mapped to the flow types sharing them."""
    layouts = {}

    for fl_type in FLOW_TYPES:
        proc = cmd(f"ethtool -n {cfg.ifname} rx-flow-hash {fl_type}",
                   fail=False)
        if proc.ret:
            continue

        fields = ""
        for line in proc.stdout.split("\n")[1:-2]:
            # if this raises we probably need to add more keys to FIELD_NAMES
            fields += FIELD_NAMES[line]

        layout, nbits = rss_key_layout(fields, fl_type.endswith("6"))
        if layout is None:
            ksft_pr(f"{fl_type}: not checked, hashes fields we can not place "
                    f"({fields})")
            continue

        layouts.setdefault((tuple(layout), nbits), []).append(fl_type)

    if not layouts:
        raise KsftSkipEx("no flow type with a hash input we can describe")

    return layouts


def test_rss_key_rank(cfg) -> None:
    """The key has to be non singular for the size of the table."""
    hkey, indir = _get_rss(cfg)
    q = min((len(indir) - 1).bit_length(), RSS_KEY_QMAX)
    degenerate = []

    if not q:
        raise KsftSkipEx("the indirection table has a single entry")

    for (layout, _), fl_types in _get_layouts(cfg).items():
        for name, lsb in layout:
            if lsb + 32 > len(hkey) * 8:
                ksft_pr(f"{name}: not checked, the key is {len(hkey)} bytes")
                continue

            if not rss_key_full_rank(hkey, lsb, q):
                degenerate.append(f"{'/'.join(fl_types)} {name}")

    for bad in degenerate:
        ksft_pr(f"degenerate: {bad}")

    ksft_eq(len(degenerate), 0,
            f"the key of {cfg.ifname} does not spread flows over the "
            f"{1 << q} entries of its indirection table")


def test_rss_key_spread(cfg) -> None:
    """Hash bursts differing in one field only, and place them in the table."""
    hkey, indir = _get_rss(cfg)
    q = (len(indir) - 1).bit_length()
    collisions = []

    if q > RSS_KEY_QMAX:
        raise KsftSkipEx(f"{len(indir)} indirection table entries is more "
                         "than the kernel guarantees")
    if not q:
        raise KsftSkipEx("the indirection table has a single entry")

    for (layout, nbits), fl_types in _get_layouts(cfg).items():
        # Unlike the rank check, this hashes the whole input, so it reads
        # the key up to 31 bits past its last bit rather than past the last
        # bit of one field.
        if nbits + 31 > len(hkey) * 8:
            ksft_pr(f"{'/'.join(fl_types)}: not checked, the key is "
                    f"{len(hkey)} bytes")
            continue

        for name, lsb in layout:
            inp = bytearray(random.randbytes(nbits // 8))
            entries = set()
            for value in range(1 << q):
                for bit in range(q):
                    rss_key_assign_bit(inp, lsb - bit, value & (1 << bit))
                hash_ = rss_key_toeplitz(hkey, inp, nbits)
                entries.add(hash_ & (len(indir) - 1))

            if len(entries) != 1 << q:
                collisions.append(f"{'/'.join(fl_types)} {name} reaches "
                                  f"{len(entries)} of the {1 << q} entries")

    for bad in collisions:
        ksft_pr(bad)

    ksft_eq(len(collisions), 0,
            f"flows differing in one field only do not fill the "
            f"indirection table of {cfg.ifname}")


def main() -> None:
    """ Ksft boiler plate main """

    with NetDrvEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        ksft_run(globs=globals(), case_pfx={"test_"}, args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
