#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
PSP conformance tests built on the GRO test binary.

This reuses the gro binary in --psp mode:
The sender crafts encapsulated & sw-encrypted PSP packets with the receiver PSP
rx-assoc, and the receiver device decrypts and decapsulates the packets before
an AF_PACKET tap gets to analyze them.

All tests from gro.py which could run with PSP are included.
"""

import socket

from lib.py import ksft_run, ksft_exit
from lib.py import ksft_variants, KsftNamedVariant
from lib.py import KsftSkipEx
from lib.py import NetDrvEpEnv, PSPFamily
from lib.py import defer, ethtool

# The helpers live next to their other users, gro.py and psp.py. Importing
# lib.py above puts the selftests root on sys.path, which is what makes these
# two resolve, so they have to stay after it - pylint wants the opposite.
# pylint: disable=wrong-import-order,import-error
from drivers.net.gro_lib import run_with_retries, set_mtu_restore, setup_hw_gro
from drivers.net.psp_lib import init_psp_dev, require_version
# pylint: enable=wrong-import-order,import-error


# Tests for both IP versions, a subset of gro.py. Missing:
# - tcp_csum: PSP packets are validated by HW (with the ICV) and marked with
# CHECKSUM_UNNECESSARY.
_COMMON = ["data_same", "data_lrg_sml", "data_sml_lrg", "data_lrg_1byte",
           "data_burst",
           "ack",
           "flags_psh", "flags_syn", "flags_rst", "flags_urg", "flags_cwr",
           "tcp_seq", "tcp_ts", "tcp_opt",
           "ip_ecn", "ip_tos",
           "large_max", "large_rem",
]

# Tests specific to IPv4, a subset of gro.py. Missing:
# - ip_csum: psp_dev_rcv() recomputes the IP checksum.
# - ip_frag4: PSP is incompatible with IP fragmentation.
_V4 = ["ip_ttl", "ip_opt",
       "ip_id_df1_inc", "ip_id_df1_fixed",
       "ip_id_df0_inc", "ip_id_df0_fixed",
       "ip_id_df1_inc_fixed", "ip_id_df1_fixed_inc",
]

# No IPv6-only tests:
# - ip_frag6: PSP doesn't support fragmentation
# - ip_v6ext_same, ip_v6ext_diff: PSP doesn't support IPv6 ext headers.


def _psp_variants():
    for ver in range(4):
        for proto in ["ipv4", "ipv6"]:
            for test_name in _COMMON + (_V4 if proto == "ipv4" else []):
                yield KsftNamedVariant(f"v{ver}_{proto}_{test_name}",
                                       ver, proto, test_name)


def _ip_variants():
    """IPv4/IPv6."""
    for proto in ("ipv4", "ipv6"):
        yield KsftNamedVariant(proto, proto)


def _psp_assoc(cfg, version=0):
    # This socket receives no traffic, exists solely to own the rx assoc.
    s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    defer(s.close)
    return cfg.pspnl.rx_assoc({"version": version, "dev-id": cfg.psp_dev_id,
                               "sock-fd": s.fileno()})['rx-key']


def _setup(cfg, version, test_name):
    """Enables PSP and HW GRO on the device under test."""
    init_psp_dev(cfg)
    require_version(cfg, version)

    if not hasattr(cfg, "feat"):
        cfg.feat = ethtool(f"-k {cfg.ifname}", json=True)[0]

    setup_hw_gro(cfg)

    # "large_*" tests need at least 4k MTU
    if test_name.startswith("large_"):
        set_mtu_restore(cfg.dev, 4096, None)
        set_mtu_restore(cfg.remote_dev, 4096, cfg.remote)


def _psp_args(cfg, versions):
    """Produces PSP associations as gro binary --psp-assoc arguments."""
    keys = [_psp_assoc(cfg, ver) for ver in versions]
    return [f"--psp-assoc {ver},{key['spi']:x},{key['key'].hex()}"
            for ver, key in zip(versions, keys)]


def _run(cfg, test_name, protocol, versions):
    """Sets up, associates and runs one gro test case under PSP."""
    ipver = protocol[-1]
    cfg.require_ipver(ipver)

    _setup(cfg, max(versions), test_name)

    run_with_retries(cfg, test_name, protocol=protocol, verbose=True,
                     common_args=_psp_args(cfg, versions))


@ksft_variants(_psp_variants())
def test_psp_gro(cfg, version, protocol, test_name):
    """Runs one gro conformance case with PSP encapsulation."""
    _run(cfg, test_name, protocol, [version])


# PSP-specific GRO tests

@ksft_variants(_ip_variants())
def test_psp_spi_diff(cfg, protocol):
    """Frames from two different SPIs must not coalesce."""
    _run(cfg, "psp_spi_diff", protocol, [0, 0])


@ksft_variants(_ip_variants())
def test_psp_ver_diff(cfg, protocol):
    """Frames from two different PSP versions must not coalesce."""
    init_psp_dev(cfg)
    if len(cfg.psp_info['psp-versions-cap']) < 2:
        raise KsftSkipEx("Device supports a single PSP version")

    _run(cfg, "psp_ver_diff", protocol, [0, 1])


@ksft_variants(_ip_variants())
def test_psp_mixed(cfg, protocol):
    """A PSP frame must not coalesce with a clear text one."""
    _run(cfg, "psp_mixed", protocol, [0])


@ksft_variants(_ip_variants())
def test_psp_after_reconfig(cfg, protocol):
    """Verifies that decap still works after PSP off + on."""
    cfg.require_ipver(protocol[-1])

    _setup(cfg, 0, "data_same")

    cap = cfg.psp_info['psp-versions-cap']
    cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'psp-versions-ena': []})
    cfg.pspnl.dev_set({'id': cfg.psp_dev_id, 'psp-versions-ena': cap})

    run_with_retries(cfg, "data_same", protocol=protocol, verbose=True,
                     common_args=_psp_args(cfg, [0]))


def main() -> None:
    """ Ksft boiler plate main """

    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.pspnl = PSPFamily()

        ksft_run(cases=[test_psp_gro, test_psp_spi_diff, test_psp_ver_diff,
                        test_psp_mixed, test_psp_after_reconfig],
                 args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
