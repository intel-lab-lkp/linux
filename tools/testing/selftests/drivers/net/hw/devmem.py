#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from os import path
from lib.py import ksft_run, ksft_exit
from lib.py import ksft_eq, KsftSkipEx
from lib.py import NetDrvEpEnv
from lib.py import bkg, cmd, rand_port, wait_port_listen


def require_devmem(cfg):
    if not hasattr(cfg, "_devmem_probed"):
        probe_command = f"{cfg.bin_local} -f {cfg.ifname}"
        cfg._devmem_supported = cmd(probe_command, fail=False, shell=True).ret == 0
        cfg._devmem_probed = True

    if not cfg._devmem_supported:
        raise KsftSkipEx("Test requires devmem support")


def check_rx(cfg, ipver) -> None:
    require_devmem(cfg)

    fs_5_tuple = False
    if "FLOW_STEERING_5_TUPLE" in cfg.env:
        fs_5_tuple = cfg.env["FLOW_STEERING_5_TUPLE"]

    addr = cfg.addr_v[ipver]
    remote_addr = cfg.remote_addr_v[ipver]
    port = rand_port()

    if ipver == "6":
        addr = "[" + addr + "]"
        remote_addr = "[" + remote_addr + "]"

    socat = f"socat -u - TCP{ipver}:{addr}:{port}"

    if fs_5_tuple:
        socat += f",bind={remote_addr}:{port}"

    listen_cmd = f"{cfg.bin_local} -l -f {cfg.ifname} -s {addr} -p {port} -v 7"

    if fs_5_tuple:
        listen_cmd += f" -c {remote_addr}"

    with bkg(listen_cmd, exit_wait=True) as ncdevmem:
        wait_port_listen(port)
        cmd(f"yes $(echo -e \x01\x02\x03\x04\x05\x06) | \
            head -c 1K | {socat}", host=cfg.remote, shell=True)

    ksft_eq(ncdevmem.ret, 0)


def check_tx(cfg, ipver) -> None:
    require_devmem(cfg)

    port = rand_port()
    listen_cmd = f"socat -U - TCP{ipver}-LISTEN:{port}"

    addr = cfg.addr_v[ipver]

    with bkg(listen_cmd) as socat:
        wait_port_listen(port)
        cmd(f"echo -e \"hello\\nworld\"| {cfg.bin_remote} -f {cfg.ifname} -s {addr} -p {port}", host=cfg.remote, shell=True)

    ksft_eq(socat.stdout.strip(), "hello\nworld")


def main() -> None:
    with NetDrvEpEnv(__file__) as cfg:
        cfg.bin_local = path.abspath(path.dirname(__file__) + "/ncdevmem")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

        if "4" in cfg.addr_v:
            ipver = "4"
        else:
            ipver = "6"

        ksft_run([check_rx, check_tx],
                 args=(cfg, ipver))
    ksft_exit()


if __name__ == "__main__":
    main()
