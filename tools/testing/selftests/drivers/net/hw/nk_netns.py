#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import subprocess
import time
from os import path
from lib.py import ksft_run, ksft_exit, KsftSkipEx, KsftFailEx
from lib.py import NetNS, MemPrvEnv
from lib.py import cmd, defer, ip, rand_ifname


def attach(bin, netif_ifindex, nk_host_ifindex, ipv6_prefix):
    cmd = [
        bin,
        "-n", str(nk_host_ifindex),
        "-e", str(netif_ifindex),
        "-i", ipv6_prefix
    ]
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.5)
        if proc.poll() is not None:
            _, stderr = proc.communicate()
            raise KsftFailEx(f"Failed to attach nk_forward BPF program: {stderr}")
        return proc
    except Exception as e:
        raise KsftFailEx(f"Failed to attach nk_forward BPF program: {e}")


def detach(proc):
    if proc and proc.poll() is None:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def test_netkit_ping(cfg) -> None:
    cfg.require_ipver("6")

    local_prefix = cfg.env.get("LOCAL_PREFIX_V6")
    if not local_prefix:
        raise KsftSkipEx("LOCAL_PREFIX_V6 required")

    local_prefix = local_prefix.rstrip("/64").rstrip("::").rstrip(":")
    ipv6_prefix = f"{local_prefix}::"

    nk_host_ifname = rand_ifname()
    nk_guest_ifname = rand_ifname()

    ip(f"link add {nk_host_ifname} type netkit mode l2 forward peer forward {nk_guest_ifname}")
    nk_host_info = ip(f"-d link show dev {nk_host_ifname}", json=True)[0]
    nk_host_ifindex = nk_host_info["ifindex"]
    nk_guest_info = ip(f"-d link show dev {nk_guest_ifname}", json=True)[0]
    nk_guest_ifindex = nk_guest_info["ifindex"]

    bpf_proc = attach(cfg.nk_forward_bin, cfg.ifindex, nk_host_ifindex, ipv6_prefix)
    defer(detach, bpf_proc)

    guest_ipv6 = f"{local_prefix}::2:1"
    ip(f"link set dev {nk_host_ifname} up")
    ip(f"-6 addr add fe80::1/64 dev {nk_host_ifname} nodad")
    ip(f"-6 route add {guest_ipv6}/128 via fe80::2 dev {nk_host_ifname}")

    with NetNS() as netns:
        ip(f"link set dev {nk_guest_ifname} netns {netns.name}")

        ip("link set lo up", ns=netns)
        ip(f"link set dev {nk_guest_ifname} up", ns=netns)
        ip(f"-6 addr add fe80::2 dev {nk_guest_ifname}", ns=netns)
        ip(f"-6 addr add {guest_ipv6} dev {nk_guest_ifname} nodad", ns=netns)

        ip(f"-6 route add default via fe80::1 dev {nk_guest_ifname}", ns=netns)

        cmd(f"ping -c 1 -W5 {guest_ipv6}")
        cmd(f"ping -c 1 -W5 {cfg.remote_addr_v['6']}", ns=netns)


def main() -> None:
    with MemPrvEnv(__file__) as cfg:
        cfg.nk_forward_bin = path.abspath(path.dirname(__file__) + "/nk_forward")
        ksft_run([test_netkit_ping], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
