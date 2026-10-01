#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""AF_XDP zero-copy hardware tests with an SKB-mode XSK peer."""

import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

from lib.py import (EthtoolFamily, KsftFailEx, KsftNamedVariant, KsftSkipEx,
                    NetdevFamily, NetDrvEpEnv, bkg, cmd, defer, ethtool,
                    fd_read_timeout, ip, ksft_exit, ksft_pr, ksft_run,
                    ksft_variants, rand_port)

CASE_TIMEOUT = 120
LISTEN_TIMEOUT = 30
NR_HUGEPAGES = 32
SKIP_CODE = 4
XSK_BIN = (Path(__file__).parent / "../../../net/lib/xskxceiver").resolve()
XSK_CASES = XSK_BIN.parent / "xsk/test_xsk_case_defs.h"


def _hardware_cases():
    defs = XSK_CASES.read_text()
    entries = re.findall(r"(?m)^XSK_TEST_CASE\((\w+),\s*\w+,\s*([^)]+)\)", defs)
    if not entries or len(entries) != defs.count("XSK_TEST_CASE("):
        raise KsftFailEx(f"cannot parse {XSK_CASES}")
    variants = []
    for test_id, (name, hw) in enumerate(entries):
        flags = {flag.strip() for flag in hw.split("|")}
        if not flags <= {"0", "XSK_HW_RX", "XSK_HW_TX"}:
            raise KsftFailEx(f"invalid hardware directions for {name}: {hw}")
        variants += [KsftNamedVariant(f"{d}_{name.lower()}", name, d, test_id)
                     for d in ("rx", "tx") if f"XSK_HW_{d.upper()}" in flags]
    return variants


def _management_addr(cfg):
    if cfg.env.get("REMOTE_TYPE") == "ssh":
        return cfg.remote.name.rpartition("@")[2]
    raise KsftSkipEx("AF_XDP hardware tests require an SSH remote")


def _root_cmd(command, host=None, sudo=False, **kwargs):
    if sudo:
        command = "sudo -n " + command
    return cmd(command, host=host, **kwargs)


def _remote_binary(cfg, local_binary):
    binary = cfg.env.get("XSK_REMOTE_BIN")
    if not binary:
        return cfg.remote.deploy(local_binary)
    if cfg.env.get("REMOTE_TYPE") != "ssh":
        raise KsftSkipEx("XSK_REMOTE_BIN requires the SSH remote backend")
    if cfg.env.get("XSK_REMOTE_DEPLOY", "1") == "0":
        result = cmd(f"test -x {shlex.quote(binary)}", host=cfg.remote,
                     fail=False)
        if result.ret:
            raise KsftSkipEx(f"remote xskxceiver is not executable: {binary}")
        return binary
    upload = binary + ".upload"
    cmd(["scp", "-q", str(local_binary), f"{cfg.remote.name}:{upload}"],
        shell=False)
    cmd(f"mv -f {shlex.quote(upload)} {shlex.quote(binary)}",
        host=cfg.remote)
    return binary


def _remote_sudo(cfg):
    return cfg.env.get("XSK_REMOTE_SUDO", "").lower() not in ("", "0", "false")


def _restore_mtu(ifname, mtu, host=None, sudo=False):
    actual = ip(f"link show dev {shlex.quote(ifname)}", json=True,
                host=host)[0]["mtu"]
    if actual == mtu:
        return
    _root_cmd(f"ip link set dev {shlex.quote(ifname)} mtu {mtu}",
              host=host, sudo=sudo)


def _reserve_hugepages(host=None, sudo=False):
    have = int(cmd("sysctl -n vm.nr_hugepages", host=host).stdout)
    if have >= NR_HUGEPAGES:
        return
    _root_cmd(f"sysctl -qw vm.nr_hugepages={NR_HUGEPAGES}", host=host,
              sudo=sudo, fail=False)
    got = int(cmd("sysctl -n vm.nr_hugepages", host=host).stdout)
    if got != have:
        defer(_root_cmd, f"sysctl -qw vm.nr_hugepages={have}", host=host,
              sudo=sudo)
    if got < NR_HUGEPAGES:
        raise KsftSkipEx(f"only {got}/{NR_HUGEPAGES} hugepages available")


def _prefer_busy_poll(ifname):
    """SO_PREFER_BUSY_POLL keeps the device IRQs masked between busy polls
    only while napi_defer_hard_irqs and gro_flush_timeout are set. Use the
    values that test_xsk.sh sets on veth."""
    for knob, value in (("napi_defer_hard_irqs", "2"),
                        ("gro_flush_timeout", "200000")):
        path = Path("/sys/class/net", ifname, knob)
        orig = path.read_text(encoding="utf-8").strip()
        if orig != value:
            path.write_text(value, encoding="utf-8")
            defer(path.write_text, orig, encoding="utf-8")


def _feature_state(ifname, feature, host=None):
    """Read an ethtool -k feature. Parse the text output, as older ethtool
    versions can't print -k as JSON."""
    output = ethtool(f"-k {shlex.quote(ifname)}", host=host).stdout
    match = re.search(rf"^{re.escape(feature)}:\s+(on|off)(.*)$", output,
                      re.MULTILINE)
    if not match:
        raise KsftSkipEx(f"ethtool does not report {feature} on {ifname}")
    return {"active": match[1] == "on", "fixed": "[fixed]" in match[2]}


def _steer_udp(ifname, port, queue, host=None, sudo=False):
    dev = shlex.quote(ifname)
    rule = f"ethtool -N {dev} flow-type udp4 dst-port {port} action {queue}"
    # Look at ntuple-filters only once the rule is refused, as every command
    # on the remote costs a round trip.
    added = _root_cmd(rule, host=host, sudo=sudo, fail=False)
    if added.ret:
        ntuple = _feature_state(ifname, "ntuple-filters", host)
        if not ntuple["active"]:
            if ntuple["fixed"]:
                raise KsftSkipEx("NIC does not support ntuple filters")
            _root_cmd(f"ethtool -K {dev} ntuple-filters on", host=host,
                      sudo=sudo)
            defer(_root_cmd, f"ethtool -K {dev} ntuple-filters off",
                  host=host, sudo=sudo)
            added = _root_cmd(rule, host=host, sudo=sudo, fail=False)
    if added.ret:
        ksft_pr(repr(added))
        raise KsftSkipEx(f"cannot steer UDP flow to queue {queue} on {ifname}")
    match = re.search(r"ID (\d+)", added.stdout)
    if not match:
        raise KsftFailEx(f"cannot identify new ntuple rule: {added.stdout}")
    defer(_root_cmd, f"ethtool -N {dev} delete {match[1]}", host=host,
          sudo=sudo)


def _reserve_rx_queue(cfg, port):
    """Move one RX queue out of default RSS and steer the test UDP flow."""
    header = {"dev-index": cfg.ifindex}
    channels = cfg.ethnl.channels_get({"header": header})
    count = (channels.get("combined-count") or 0) + (channels.get("rx-count") or 0)
    if count < 2:
        raise KsftSkipEx("RX queue isolation needs at least two channels")
    queue = count - 1

    try:
        rss = cfg.ethnl.rss_get({"header": header})
        saved = list(rss["indir"])
    except Exception as exc:
        raise KsftSkipEx(f"cannot read RSS indirection table: {exc}") from exc
    if queue in saved:
        updated = [0 if entry == queue else entry for entry in saved]
        try:
            cfg.ethnl.rss_set({"header": header, "indir": updated})
        except Exception as exc:
            raise KsftSkipEx(f"cannot reserve RX queue in RSS: {exc}") from exc
        defer(cfg.ethnl.rss_set, {"header": header, "indir": saved})
        actual = cfg.ethnl.rss_get({"header": header}).get("indir")
        if actual != updated:
            raise KsftSkipEx("NIC did not remove the reserved queue from RSS")

    _steer_udp(cfg.ifname, port, queue)
    return queue


def _case_args(cfg, name):
    if name != "TOO_MANY_FRAGS":
        return []
    try:
        info = cfg.netnl.dev_get({"ifindex": cfg.ifindex})
    except Exception as exc:
        raise KsftSkipEx(f"cannot query DUT fragment limit: {exc}") from exc
    max_frags = info.get("xdp-zc-max-segs", 0)
    if max_frags <= 1:
        raise KsftSkipEx("DUT does not advertise multi-buffer zero-copy")
    return ["--max-frags", str(max_frags)]


def _idle_mtu(ifname, host=None):
    """Return the MTU of a link that has no XDP program attached."""
    info = ip(f"link show dev {shlex.quote(ifname)}", json=True, host=host)[0]
    if "xdp" in info:
        raise KsftSkipEx(f"{ifname} already has an XDP program")
    return info["mtu"]


def _remote_idle_mtu(cfg):
    """Look at the remote link once. Every case leaves its MTU as it found
    it and an endpoint replaces a program left behind by an earlier case,
    so looking again would only add a round trip to each case."""
    if not hasattr(cfg, "xsk_remote_mtu"):
        cfg.xsk_remote_mtu = _idle_mtu(cfg.remote_ifname, cfg.remote)
    return cfg.xsk_remote_mtu


def _detach_xdp(ifname, mode, host=None, sudo=False):
    """A killed endpoint leaves its XDP program behind; name the mode, as a
    bare "xdp off" only detaches driver mode on a native-XDP NIC."""
    info = ip(f"link show dev {shlex.quote(ifname)}", json=True, host=host)[0]
    if "xdp" not in info:
        return
    _root_cmd(f"ip link set dev {shlex.quote(ifname)} {mode} off", host=host,
              sudo=sudo)


def _wait_remote_listen(remote, port):
    """Read the remote endpoint's stderr up to the line it prints once it
    listens, as polling its sockets would cost a round trip per try. Return
    what was read and whether that line came."""
    line = f"Listening for XSK peer on port {port}\n".encode()
    fd = remote.proc.stderr.fileno()
    head = b""
    deadline = time.monotonic() + LISTEN_TIMEOUT
    while line not in head:
        try:
            data = fd_read_timeout(fd, max(deadline - time.monotonic(), 0))
        except TimeoutError:
            break
        if not data:
            break
        head += data
    return head.decode(), line in head


def _require_zc(cfg):
    if not hasattr(cfg, "xsk_zc"):
        xdp_features = cfg.netnl.dev_get({"ifindex": cfg.ifindex})["xdp-features"]
        cfg.xsk_zc = "xsk-zerocopy" in xdp_features
    if not cfg.xsk_zc:
        raise KsftSkipEx("NIC does not advertise AF_XDP zero-copy")


def _remote_test_binary(cfg):
    if not hasattr(cfg, "xsk_remote_bin"):
        cfg.xsk_remote_bin = _remote_binary(cfg, XSK_BIN)
    return cfg.xsk_remote_bin


def _udp_port(cfg):
    port = int(cfg.env.get("XSK_UDP_PORT", "42567"))
    if not 1 <= port <= 65535:
        raise KsftFailEx("XSK_UDP_PORT must be from 1 to 65535")
    return port


def _setup_case(cfg, name, direction, port, busy_poll):
    """Set up what this case alone needs; defer() undoes it when it ends.
    Return the DUT queue and the fallbacks for what the endpoints undo."""
    sudo = _remote_sudo(cfg)
    local_mtu = _idle_mtu(cfg.ifname)
    remote_mtu = _remote_idle_mtu(cfg)
    if "UNALIGNED" in name:
        _reserve_hugepages()
        _reserve_hugepages(cfg.remote, sudo)
    if busy_poll:
        _prefer_busy_poll(cfg.ifname)

    if direction == "rx":
        queue = _reserve_rx_queue(cfg, port)
    else:
        queue = 0
        _steer_udp(cfg.remote_ifname, port, 0, cfg.remote, sudo)

    # Undo what a failed or killed endpoint may leave behind. Defers run in
    # reverse, so the XDP programs, which can cap the MTU, are detached first.
    fallbacks = [
        defer(_restore_mtu, cfg.ifname, local_mtu),
        defer(_restore_mtu, cfg.remote_ifname, remote_mtu, cfg.remote, sudo),
        defer(_detach_xdp, cfg.ifname, "xdpdrv"),
        defer(_detach_xdp, cfg.remote_ifname, "xdpgeneric", cfg.remote, sudo),
    ]
    return queue, fallbacks


def _udp_args(cfg, direction, port):
    if direction == "rx":
        src, dst = cfg.remote_addr_v["4"], cfg.addr_v["4"]
    else:
        src, dst = cfg.addr_v["4"], cfg.remote_addr_v["4"]
    return ["--udp-src", src, "--udp-dst", dst, "--udp-port", str(port)]


def _case_result(local, remote):
    if local and local.stdout:
        ksft_pr(local.stdout, line_pfx="dut|")
    if local and local.stderr:
        ksft_pr(local.stderr, line_pfx="dut|")
    if remote.stdout:
        ksft_pr(remote.stdout, line_pfx="remote|")
    if remote.stderr:
        ksft_pr(remote.stderr, line_pfx="remote|")
    if local is None:
        raise KsftFailEx("remote XSK endpoint did not listen on the control "
                         f"port, exit {remote.ret}")
    if local.ret == SKIP_CODE or remote.ret == SKIP_CODE:
        raise KsftSkipEx("AF_XDP case unsupported by one endpoint")
    if local.ret or remote.ret:
        raise KsftFailEx(f"DUT exit {local.ret}, remote exit {remote.ret}")


def _run_local(args):
    local = cmd(args, background=True, shell=False, fail=False)
    try:
        local.process(terminate=False, fail=False, timeout=CASE_TIMEOUT)
    except subprocess.TimeoutExpired as exc:
        local.proc.kill()
        local.process(terminate=False, fail=False)
        raise KsftFailEx(f"DUT case timed out after {CASE_TIMEOUT}s") from exc
    return local


def _run_endpoints(cfg, local_args, remote_args, control_port, fallbacks):
    local = None
    remote = bkg(shlex.join(remote_args), host=cfg.remote, exit_wait=True,
                 fail=False)
    try:
        head, listening = _wait_remote_listen(remote, control_port)
        if listening:
            local = _run_local(local_args)
            remote.process(terminate=False, fail=False, timeout=10)
    finally:
        if remote.ret is None:
            remote.process(terminate=True, fail=False)
    # _wait_remote_listen() took the start of stderr off the pipe.
    remote.stderr = head + remote.stderr
    if local and {local.ret, remote.ret} <= {0, SKIP_CODE}:
        # Both endpoints detached XDP and restored the MTU when they exited.
        for fallback in fallbacks:
            fallback.cancel()
    _case_result(local, remote)


def test_xsk(cfg, name, direction, test_id, busy_poll=False):
    cfg.require_ipver("4")
    sync_addr = _management_addr(cfg)
    udp_port = _udp_port(cfg)
    _require_zc(cfg)
    remote_bin = _remote_test_binary(cfg)
    case_args = _case_args(cfg, name)
    queue, fallbacks = _setup_case(cfg, name, direction, udp_port, busy_poll)

    udp_args = _udp_args(cfg, direction, udp_port)
    control_port = rand_port()
    remote_args = [str(remote_bin), "-i", cfg.remote_ifname,
                   "--hw", "--listen", "-m", "skb",
                   "-t", str(test_id),
                   "-e", "tx" if direction == "rx" else "rx",
                   "-p", "0.0.0.0", "-P", str(control_port), *udp_args,
                   "--peer-mac", cfg.dev["address"], "--queue", "0", *case_args]
    if _remote_sudo(cfg):
        remote_args = ["sudo", "-n"] + remote_args
    local_args = [str(XSK_BIN), "-i", cfg.ifname, "--hw", "-m", "zc",
                  "-t", str(test_id), "-e", direction,
                  "-p", sync_addr, "-P", str(control_port), *udp_args,
                  "--peer-mac", cfg.remote_dev["address"],
                  "--queue", str(queue), *case_args]
    if busy_poll:
        local_args.append("-b")
    _run_endpoints(cfg, local_args, remote_args, control_port, fallbacks)


def test_xsk_busy_poll(cfg, name, direction, test_id):
    """Run a case with the DUT endpoint busy polling."""
    test_xsk(cfg, name, direction, test_id, busy_poll=True)


def main():
    # ksft_run() rejects options it does not know, so take -b out first.
    busy_poll = "-b" in sys.argv[1:]
    if busy_poll:
        sys.argv.remove("-b")
    test = test_xsk_busy_poll if busy_poll else test_xsk
    variants = ksft_variants(_hardware_cases())(test)

    if any(arg in ("-h", "-l") for arg in sys.argv[1:]):
        ksft_run(cases=[variants])
        return
    if not XSK_BIN.exists():
        print(f"1..0 # SKIP {XSK_BIN} was not built")
        sys.exit(SKIP_CODE)
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        cfg.netnl = NetdevFamily()
        ksft_run(cases=[variants], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
