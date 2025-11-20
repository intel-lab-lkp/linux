#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from os import path
from lib.py import ksft_run, ksft_exit
from lib.py import MemPrvEnv
from lib.py import bkg, cmd, wait_port_listen


def test_zcrx(cfg) -> None:
    cfg.require_ipver('6')

    rx_cmd = f"{cfg.bin_local} -s -p {cfg.port} -i {cfg.ifname} -q {cfg.target_queue}"
    tx_cmd = f"{cfg.bin_remote} -c -h {cfg.addr_v['6']} -p {cfg.port} -l 12840"
    with bkg(rx_cmd, exit_wait=True):
        wait_port_listen(cfg.port, proto="tcp")
        cmd(tx_cmd, host=cfg.remote)


def test_zcrx_oneshot(cfg) -> None:
    cfg.require_ipver('6')

    rx_cmd = f"{cfg.bin_local} -s -p {cfg.port} -i {cfg.ifname} -q {cfg.target_queue} -o 4"
    tx_cmd = f"{cfg.bin_remote} -c -h {cfg.addr_v['6']} -p {cfg.port} -l 4096 -z 16384"
    with bkg(rx_cmd, exit_wait=True):
        wait_port_listen(cfg.port, proto="tcp")
        cmd(tx_cmd, host=cfg.remote)


def main() -> None:
    with MemPrvEnv(__file__) as cfg:
        cfg.bin_local = path.abspath(path.dirname(__file__) + "/../../../drivers/net/hw/iou-zcrx")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

        ksft_run(globs=globals(), case_pfx={"test_"}, args=(cfg,))

    with MemPrvEnv(__file__, rss=True) as cfg:
        cfg.bin_local = path.abspath(path.dirname(__file__) + "/../../../drivers/net/hw/iou-zcrx")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

        ksft_run(globs=globals(), case_pfx={"test_"}, args=(cfg,), case_sfx="_rss")
    ksft_exit()


if __name__ == "__main__":
    main()
