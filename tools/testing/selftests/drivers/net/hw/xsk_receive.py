#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# This a test for virtio-net rx when there is a XDP socket bound to it. The test
# is expected to be run in the host side.
#
# The run example:
#
# export NETIF=tap0
# export LOCAL_V4=192.168.31.1
# export REMOTE_V4=192.168.31.3
# export REMOTE_TYPE=ssh
# export REMOTE_ARGS='root@192.168.31.3'
# ./ksft-net-drv/run_kselftest.sh -t drivers/net/hw:xsk_receive.py
#
# where:
# - 192.168.31.1 is the IP of tap device in the host
# - 192.168.31.3 is the IP of virtio-net device in the guest
#
# The Qemu command to setup virtio-net
# -netdev tap,id=hostnet1,vhost=on,script=no,downscript=no
# -device virtio-net-pci,netdev=hostnet1,iommu_platform=on,disable-legacy=on
#
# The MTU of tap device can be adjusted to test more cases:
# - 1500: single buffer XDP
# - 9000: multi-buffer XDP

from lib.py import ksft_exit, ksft_run
from lib.py import KsftSkipEx, KsftFailEx
from lib.py import NetDrvEpEnv
from lib.py import bkg, cmd, wait_port_listen
from os import path

SERVER_PORT = 8888
CLIENT_PORT = 9999

def test_xdp_pass(cfg, server_cmd, client_cmd):
    with bkg(server_cmd, host=cfg.remote, exit_wait=True):
        wait_port_listen(SERVER_PORT, proto="udp", host=cfg.remote)
        cmd(client_cmd)

def test_xdp_pass_zc(cfg, server_cmd, client_cmd):
    server_cmd += " -z"
    with bkg(server_cmd, host=cfg.remote, exit_wait=True):
        wait_port_listen(SERVER_PORT, proto="udp", host=cfg.remote)
        cmd(client_cmd)

def test_xdp_redirect(cfg, server_cmd, client_cmd):
    server_cmd += " -d"
    with bkg(server_cmd, host=cfg.remote, exit_wait=True):
        wait_port_listen(SERVER_PORT, proto="udp", host=cfg.remote)
        cmd(client_cmd)

def test_xdp_redirect_zc(cfg, server_cmd, client_cmd):
    server_cmd += " -d -z"
    with bkg(server_cmd, host=cfg.remote, exit_wait=True):
        wait_port_listen(SERVER_PORT, proto="udp", host=cfg.remote)
        cmd(client_cmd)

def main():
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.bin_local = path.abspath(path.dirname(__file__)
                            + "/../../../drivers/net/hw/xsk_receive")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

        server_cmd = f"{cfg.bin_remote} -s -i {cfg.remote_ifname} "
        server_cmd += f"-r {cfg.remote_addr_v["4"]} -l {cfg.addr_v["4"]}"
        client_cmd = f"{cfg.bin_local} -c -r {cfg.remote_addr_v["4"]} "
        client_cmd += f"-l {cfg.addr_v["4"]}"

        ksft_run(globs=globals(), case_pfx={"test_"}, args=(cfg, server_cmd, client_cmd))
    ksft_exit()

if __name__ == "__main__":
    main()
