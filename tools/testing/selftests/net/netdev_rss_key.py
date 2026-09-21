#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Check the quality of the host RSS key, /proc/sys/net/core/netdev_rss_key.

This is the key netdev_rss_key_fill() hands to the drivers. It has to be non
singular for every field of the hash input and every queue count up to
2 ** RSS_KEY_QMAX, see lib/py/rsskey.py for what that means and why it
matters. Unlike the KUnit tests of the generator, this checks the key the
running kernel has actually handed out.

The key is generated lazily, the first time a driver asks for it, so this
test skips until a driver has done so. Any NIC calling netdev_rss_key_fill()
is enough; in a virtual machine, virtio_net does it from
virtnet_init_default_rss() once RSS has been negotiated.

Checking what a given NIC really uses is a different question, since a
driver may bring its own key. See drivers/net/hw/rss_key.py for that.
"""

from lib.py import ksft_run, ksft_exit, ksft_pr
from lib.py import ksft_eq, ksft_ge
from lib.py import KsftSkipEx
from lib.py import RSS_KEY_QMAX, rss_key_full_rank, rss_key_layout

KEY_PATH = "/proc/sys/net/core/netdev_rss_key"

# Shortest key able to hash an IPv6 4-tuple.
MIN_KEY_LEN = 40


def _read_key():
    """Return the host RSS key, skipping if it has not been generated."""
    try:
        with open(KEY_PATH, "r", encoding="ascii") as fp:
            text = fp.read().strip()
    except FileNotFoundError as exc:
        raise KsftSkipEx(f"{KEY_PATH} is not available") from exc

    key = bytes(int(byte, 16) for byte in text.split(":"))

    if not any(key):
        raise KsftSkipEx("the host RSS key has not been generated yet, "
                         "no driver has called netdev_rss_key_fill()")

    return key


def check_length() -> None:
    key = _read_key()

    ksft_pr(f"host RSS key is {len(key)} bytes")
    ksft_ge(len(key), MIN_KEY_LEN, "key too short to hash an IPv6 4-tuple")


def check_spread() -> None:
    key = _read_key()
    degenerate = []

    for ipv6 in (False, True):
        layout, _ = rss_key_layout("sdfn", ipv6)
        family = "IPv6" if ipv6 else "IPv4"

        for name, lsb in layout:
            if lsb + 32 > len(key) * 8:
                continue

            for q in range(1, RSS_KEY_QMAX + 1):
                if not rss_key_full_rank(key, lsb, q):
                    degenerate.append(f"{family} {name} over {1 << q} queues")

    for bad in degenerate:
        ksft_pr(f"degenerate: {bad}")

    ksft_eq(len(degenerate), 0,
            "the host RSS key does not spread flows over all the queues")


def check_grid() -> None:
    """Sweep the whole key, not only the fields of the usual layouts.

    netdev_rss_key_fill() does not get to know what the hardware hashes, so
    it gives the property at every 16-bit aligned position of the key. A NIC
    hashing an encapsulated header reads the key well past the first 40
    bytes, and has to find the same guarantee there.
    """
    key = _read_key()
    bits = len(key) * 8
    positions = 0
    degenerate = []

    for lsb in range(15, bits - 31, 16):
        positions += 1

        for q in range(1, RSS_KEY_QMAX + 1):
            if not rss_key_full_rank(key, lsb, q):
                degenerate.append(f"field ending at bit {lsb} "
                                  f"over {1 << q} queues")

    ksft_pr(f"checked {positions} positions of the {len(key)} byte key")

    for bad in degenerate[:8]:
        ksft_pr(f"degenerate: {bad}")

    ksft_eq(len(degenerate), 0,
            "the host RSS key does not spread flows over all the queues "
            "at every 16-bit aligned position")


def main() -> None:
    ksft_run(globs=globals(), case_pfx={"check_"})
    ksft_exit()


if __name__ == "__main__":
    main()
