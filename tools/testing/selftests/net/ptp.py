#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# By Maciek Machnikowski <maciek@machnikowski.net> (c) 2026,
#
# Self-tests for HW timestamping and 1588 synchronization
#
# This test runs a ptp4l leader/follower pair and waits for follower sync
# state (s2), using one common execution path.
#
# By default, it:
# - Creates two netdevsim instances in separate namespaces
# - Assigns IPv4 addresses and links them together
# - Uses those interfaces as leader/follower endpoints for ptp4l
#
# Optional: --leader and --follower override endpoints with existing
# interfaces. Each argument can be either "ifname" (initial netns) or
# "netns:ifname". Both options must be provided together.

import os
import shutil
import subprocess
import sys
import tempfile
import time

from lib.py import (
    NetNS,
    NetdevSimDev,
    KsftSkipEx,
    defer,
    ip,
    ksft_exit,
    ksft_pr,
    ksft_run,
    ksft_true,
)

PTP4L_SYNC_TIMEOUT = 40


def _parse_interface_spec(spec):
    """Return (netns_name_or_None, ifname). 'ns:ifname' uses that netns."""
    if ":" in spec:
        ns, ifname = spec.split(":", 1)
        return ns, ifname
    return None, spec


def _strip_ptp_port_args():
    """
    Remove --leader/--follower from sys.argv. Return (leader_spec, follower_spec).
    """
    leader = follower = None
    new_argv = [sys.argv[0]]
    args = iter(sys.argv[1:])
    for a in args:
        if a == "--leader":
            leader = next(args, None)
        elif a == "--follower":
            follower = next(args, None)
        else:
            new_argv.append(a)
    sys.argv[:] = new_argv
    return leader, follower


def _run_ptp4l_wait_sync(leader_ifname, follower_ifname,
                         leader_ns=None, follower_ns=None):
    leader_log_path, follower_log_path = _prepare_ptp4l_logs()
    leader_proc, leader_log = _start_ptp4l(
        leader_ifname, leader_log_path, leader_ns, ptp4l_params=["-4"]
    )
    follower_proc, follower_log = _start_ptp4l(
        follower_ifname, follower_log_path, follower_ns, ptp4l_params=["-s", "-4"]
    )
    defer(lambda: _stop_ptp4l(leader_proc, leader_log))
    defer(lambda: _stop_ptp4l(follower_proc, follower_log))

    deadline = time.monotonic() + PTP4L_SYNC_TIMEOUT
    while time.monotonic() < deadline:
        try:
            with open(follower_log_path) as f:
                if " s2 " in f.read():
                    return
        except FileNotFoundError:
            pass
        time.sleep(1)

    ksft_pr(
        f"ptp4l follower did not reach locked state (s2) within {PTP4L_SYNC_TIMEOUT}s"
    )
    try:
        with open(follower_log_path) as f:
            tail = f.read().strip().split("\n")[-10:]
        ksft_pr("Follower log (last 10 lines): " + " | ".join(tail))
    except Exception:
        pass
    ksft_true(False, "PTP sync timeout")


def _start_ptp4l(ifname, log_path, ns_name, ptp4l_params=None):
    cmd = ["ptp4l", "-i", ifname, "-m", "-P"]
    if ptp4l_params:
        cmd.extend(ptp4l_params)
    if ns_name is not None:
        cmd = ["ip", "netns", "exec", ns_name] + cmd

    log_file = open(log_path, "w")
    proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT)
    return proc, log_file


def _stop_ptp4l(proc, log_file):
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        try:
            proc.kill()
            proc.wait(timeout=2)
        except (OSError, subprocess.TimeoutExpired):
            pass
    finally:
        log_file.close()


def _prepare_ptp4l_logs():
    leader_log = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
    follower_log = tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".log")
    leader_log.close()
    follower_log.close()
    defer(os.unlink, leader_log.name)
    defer(os.unlink, follower_log.name)
    return leader_log.name, follower_log.name


def ptp_sync_test(leader_spec=None, follower_spec=None):
    if not shutil.which("ptp4l"):
        raise KsftSkipEx("ptp4l command not found. Skipping PTP sync test")

    use_custom = leader_spec is not None
    if use_custom ^ (follower_spec is not None):
        ksft_true(False, "PTP sync: specify both --leader and --follower or neither")
        return

    if use_custom:
        leader_ns, if1 = _parse_interface_spec(leader_spec)
        follower_ns, if2 = _parse_interface_spec(follower_spec)

        _run_ptp4l_wait_sync(if1, if2, leader_ns, follower_ns)
        return

    with NetNS("nssv") as nssv, NetNS("nscl") as nscl, \
         NetdevSimDev(port_count=1, queue_count=1, ns=nssv) as nsimdevsv, \
         NetdevSimDev(port_count=1, queue_count=1, ns=nscl) as nsimdevcl:

        nsimsv = nsimdevsv.nsims[0]
        nsimcl = nsimdevcl.nsims[0]

        ip(f"addr add 192.168.1.1/24 dev {nsimsv.ifname}", ns=nssv.name)
        ip(f"link set dev {nsimsv.ifname} up", ns=nssv.name)

        ip(f"addr add 192.168.1.2/24 dev {nsimcl.ifname}", ns=nscl.name)
        ip(f"link set dev {nsimcl.ifname} up", ns=nscl.name)

        nssv_path = f"/var/run/netns/{nssv.name}"
        nscl_path = f"/var/run/netns/{nscl.name}"

        with open(nssv_path) as nssv_file, open(nscl_path) as nscl_file:
            link_val = f"{nssv_file.fileno()}:{nsimsv.ifindex} {nscl_file.fileno()}:{nsimcl.ifindex}"
            NetdevSimDev.ctrl_write("link_device", link_val)
            _run_ptp4l_wait_sync(nsimsv.ifname, nsimcl.ifname, nssv.name, nscl.name)
            NetdevSimDev.ctrl_write("unlink_device", f"{nssv_file.fileno()}:{nsimsv.ifindex}")


def main():
    leader, follower = _strip_ptp_port_args()
    ksft_run([ptp_sync_test], args=(leader, follower))
    ksft_exit()


if __name__ == "__main__":
    main()
