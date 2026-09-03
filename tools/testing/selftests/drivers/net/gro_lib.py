# SPDX-License-Identifier: GPL-2.0

"""Shared helpers for the GRO selftests."""

import os

from lib.py import ksft_pr
from lib.py import KsftFailEx, KsftXfailEx
from lib.py import bkg, cmd, defer, ethtool, ip


# gro.c exits with this code when it detects over-coalescing
EXIT_OVER_COALESCE = 42


def resolve_dmac(cfg, ipver):
    """
    Finds the destination MAC address remote host should use to send packets
    towards the local host. It may be a router / gateway address.
    """

    attr = "dmac" + ipver
    # Cache the response across test cases
    if hasattr(cfg, attr):
        return getattr(cfg, attr)

    route = ip(f"-{ipver} route get {cfg.addr_v[ipver]}",
               json=True, host=cfg.remote)[0]
    gw = route.get("gateway")
    # Local L2 segment, address directly
    if not gw:
        setattr(cfg, attr, cfg.dev['address'])
        return getattr(cfg, attr)

    # ping to make sure neighbor is resolved,
    # bind to an interface, for v6 the GW is likely link local
    cmd(f"ping -c1 -W0 -I{cfg.remote_ifname} {gw}", host=cfg.remote)

    neigh = ip(f"neigh get {gw} dev {cfg.remote_ifname}",
               json=True, host=cfg.remote)[0]
    setattr(cfg, attr, neigh['lladdr'])
    return getattr(cfg, attr)


def write_defer_restore(cfg, path, val, defer_undo=False):
    """Writes val to a sysfs file, optionally restoring it on test exit."""
    with open(path, "r", encoding="utf-8") as fp:
        orig_val = fp.read().strip()
        if str(val) == orig_val:
            return
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(val)
    if defer_undo:
        defer(write_defer_restore, cfg, path, orig_val)


def set_mtu_restore(dev, mtu, host):
    """Raises a device's MTU to at least mtu, restoring it on test exit."""
    if dev['mtu'] < mtu:
        ip(f"link set dev {dev['ifname']} mtu {mtu}", host=host)
        defer(ip, f"link set dev {dev['ifname']} mtu {dev['mtu']}", host=host)


def set_ethtool_feat(dev, current, feats, host=None):
    """Sets ethtool features, restoring them on test exit.

    current is the feature state as reported by "ethtool -k", xfail if a
    feature which needs changing is fixed.
    """
    s2n = {True: "on", False: "off"}

    new = ["-K", dev]
    old = ["-K", dev]
    no_change = True
    for name, state in feats.items():
        new += [name, s2n[state]]
        old += [name, s2n[current[name]["active"]]]

        if current[name]["active"] != state:
            no_change = False
            if current[name]["fixed"]:
                raise KsftXfailEx(f"Device does not support {name}")
    if no_change:
        return

    eth_cmd = ethtool(" ".join(new), host=host)
    defer(ethtool, " ".join(old), host=host)

    # If ethtool printed something kernel must have modified some features
    if eth_cmd.stdout:
        ksft_pr(eth_cmd)


def setup_hw_gro(cfg):
    """Turns on HW GRO and make sure SW GRO stays out of the way.

    Expects cfg.feat to hold the local device's "ethtool -k" state.
    """
    set_ethtool_feat(cfg.ifname, cfg.feat,
                     {"generic-receive-offload": False,
                      "rx-gro-hw": True,
                      "large-receive-offload": False})

    # Some NICs treat HW GRO as a GRO sub-feature so disabling GRO
    # will also clear HW GRO. Use a hack of installing XDP generic
    # to skip SW GRO, even when enabled.
    feat = ethtool(f"-k {cfg.ifname}", json=True)[0]
    if not feat["rx-gro-hw"]["active"]:
        ksft_pr("Driver clears HW GRO and SW GRO is cleared, using generic XDP workaround")
        prog = cfg.net_lib_dir / "xdp_dummy.bpf.o"
        ip(f"link set dev {cfg.ifname} xdpgeneric obj {prog} sec xdp")
        defer(ip, f"link set dev {cfg.ifname} xdpgeneric off")

        # Attaching XDP may change features, fetch the latest state
        feat = ethtool(f"-k {cfg.ifname}", json=True)[0]

        set_ethtool_feat(cfg.ifname, feat,
                         {"generic-receive-offload": True,
                          "rx-gro-hw": True,
                          "large-receive-offload": False})


# pylint: disable=too-many-arguments,too-many-positional-arguments
# pylint: disable=too-many-locals
def run_gro_bin(cfg, test_name, protocol=None, num_flows=None,
                order_check=False, verbose=False, fail=False,
                common_args=None):
    """Runs gro binary with given test and return the process result.

    common_args is a list of extra arguments passed to both ends.
    """
    if not hasattr(cfg, "bin_remote"):
        cfg.bin_local = cfg.net_lib_dir / "gro"
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

    if protocol is None:
        ipver = cfg.addr_ipver
        protocol = f"ipv{ipver}"
    else:
        ipver = "6" if protocol[-1] == "6" else "4"

    dmac = resolve_dmac(cfg, ipver)

    base_args = [
        f"--{protocol}",
        f"--dmac {dmac}",
        f"--smac {cfg.remote_dev['address']}",
        f"--daddr {cfg.addr_v[ipver]}",
        f"--saddr {cfg.remote_addr_v[ipver]}",
        f"--test {test_name}",
    ]
    if num_flows:
        base_args.append(f"--num-flows {num_flows}")
    if order_check:
        base_args.append("--order-check")
    if verbose:
        base_args.append("--verbose")
    if common_args:
        base_args += common_args

    args = " ".join(base_args)

    rx_cmd = f"{cfg.bin_local} {args} --rx --iface {cfg.ifname}"
    tx_cmd = f"{cfg.bin_remote} {args} --iface {cfg.remote_ifname}"

    with bkg(rx_cmd, ksft_ready=True, exit_wait=True, fail=fail) as rx_proc:
        cmd(tx_cmd, host=cfg.remote)

    return rx_proc


def run_with_retries(cfg, test_name, protocol=None, **kwargs):
    """Runs a single gro test case, retrying to deflake it.

    Each test is run 6 times, because given the receive timing, not all
    packets that should coalesce will be considered in the same flow on
    every try.  Over-coalescing is a hard failure, retries can only
    cause false negatives there.

    Extra keyword arguments (num_flows, order_check, verbose,
    common_args) are forwarded to run_gro_bin().
    """
    max_retries = 6
    for attempt in range(max_retries):
        fail_now = attempt >= max_retries - 1
        rx_proc = run_gro_bin(cfg, test_name, protocol=protocol,
                              fail=fail_now, **kwargs)

        if rx_proc.ret == 0:
            return

        ksft_pr(rx_proc)

        # ret==42 means the receiver detected over-coalescing.
        # This is unambiguous proof of a bug, retries can only cause
        # false negatives.
        if rx_proc.ret == EXIT_OVER_COALESCE:
            raise KsftFailEx(f"GRO over-coalesced in {protocol}/{test_name}")

        if test_name.startswith("large_") and os.environ.get("KSFT_MACHINE_SLOW"):
            ksft_pr(f"Ignoring {protocol}/{test_name} failure due to slow environment")
            return

        ksft_pr(f"Attempt {attempt + 1}/{max_retries} failed, retrying...")
