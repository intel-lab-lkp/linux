#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import socket
import errno

from os import path
from lib.py import ksft_run, ksft_exit
from lib.py import ksft_eq, KsftSkipEx
from lib.py import NetDrvEpEnv
from lib.py import bkg, cmd, rand_port, wait_port_listen
from lib.py import ksft_disruptive


def require_devmem(cfg):
    if not hasattr(cfg, "_devmem_probed"):
        probe_command = f"{cfg.bin_local} -f {cfg.ifname}"
        cfg._devmem_supported = cmd(probe_command, fail=False, shell=True).ret == 0
        cfg._devmem_probed = True

    if not cfg._devmem_supported:
        raise KsftSkipEx("Test requires devmem support")


@ksft_disruptive
def check_rx(cfg) -> None:
    require_devmem(cfg)

    port = rand_port()
    socat = f"socat -u - TCP{cfg.addr_ipver}:{cfg.baddr}:{port},bind={cfg.remote_baddr}:{port}"
    listen_cmd = f"{cfg.bin_local} -l -f {cfg.ifname} -s {cfg.addr} -p {port} -c {cfg.remote_addr} -v 7"

    with bkg(listen_cmd, exit_wait=True) as ncdevmem:
        wait_port_listen(port)
        cmd(f"yes $(echo -e \x01\x02\x03\x04\x05\x06) | \
            head -c 1K | {socat}", host=cfg.remote, shell=True)

    ksft_eq(ncdevmem.ret, 0)


@ksft_disruptive
def check_tx(cfg) -> None:
    require_devmem(cfg)

    port = rand_port()
    listen_cmd = f"socat -U - TCP{cfg.addr_ipver}-LISTEN:{port}"

    with bkg(listen_cmd, host=cfg.remote, exit_wait=True) as socat:
        wait_port_listen(port, host=cfg.remote)
        cmd(f"echo -e \"hello\\nworld\"| {cfg.bin_local} -f {cfg.ifname} -s {cfg.remote_addr} -p {port}", shell=True)

    ksft_eq(socat.stdout.strip(), "hello\nworld")


@ksft_disruptive
def check_tx_chunks(cfg) -> None:
    require_devmem(cfg)

    port = rand_port()
    listen_cmd = f"socat -U - TCP{cfg.addr_ipver}-LISTEN:{port}"

    with bkg(listen_cmd, host=cfg.remote, exit_wait=True) as socat:
        wait_port_listen(port, host=cfg.remote)
        cmd(f"echo -e \"hello\\nworld\"| {cfg.bin_local} -f {cfg.ifname} -s {cfg.remote_addr} -p {port} -z 3", shell=True)

    ksft_eq(socat.stdout.strip(), "hello\nworld")


@ksft_disruptive
def check_autorelease_disabled(cfg) -> None:
    """Test RX with autorelease disabled (requires manual token release in ncdevmem)"""
    require_devmem(cfg)

    port = rand_port()
    socat = f"socat -u - TCP{cfg.addr_ipver}:{cfg.baddr}:{port},bind={cfg.remote_baddr}:{port}"
    listen_cmd = f"{cfg.bin_local} -l -f {cfg.ifname} -s {cfg.addr} -p {port} -c {cfg.remote_addr} -v 7 -A 0"

    with bkg(listen_cmd, exit_wait=True) as ncdevmem:
        wait_port_listen(port)
        cmd(f"yes $(echo -e \x01\x02\x03\x04\x05\x06) | \
            head -c 1K | {socat}", host=cfg.remote, shell=True)

    ksft_eq(ncdevmem.ret, 0)


@ksft_disruptive
def check_autorelease_enabled(cfg) -> None:
    """Test RX with autorelease enabled (requires token autorelease in ncdevmem)"""
    require_devmem(cfg)

    port = rand_port()
    socat = f"socat -u - TCP{cfg.addr_ipver}:{cfg.baddr}:{port},bind={cfg.remote_baddr}:{port}"
    listen_cmd = f"{cfg.bin_local} -l -f {cfg.ifname} -s {cfg.addr} -p {port} -c {cfg.remote_addr} -v 7 -A 1"

    with bkg(listen_cmd, exit_wait=True) as ncdevmem:
        wait_port_listen(port)
        cmd(f"yes $(echo -e \x01\x02\x03\x04\x05\x06) | \
            head -c 1K | {socat}", host=cfg.remote, shell=True)

    ksft_eq(ncdevmem.ret, 0)


def check_sockopt_autorelease_default(cfg) -> None:
    """Test that SO_DEVMEM_AUTORELEASE default is 0"""
    SO_DEVMEM_AUTORELEASE = 85

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        val = sock.getsockopt(socket.SOL_SOCKET, SO_DEVMEM_AUTORELEASE)
        ksft_eq(val, 0, "Default autorelease should be 0")
    except OSError as e:
        if e.errno == errno.ENOPROTOOPT:
            raise KsftSkipEx("SO_DEVMEM_AUTORELEASE not supported")
        raise
    finally:
        sock.close()


def check_sockopt_autorelease_set_0(cfg) -> None:
    """Test setting SO_DEVMEM_AUTORELEASE to 0"""
    SO_DEVMEM_AUTORELEASE = 85

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, SO_DEVMEM_AUTORELEASE, 0)
        val = sock.getsockopt(socket.SOL_SOCKET, SO_DEVMEM_AUTORELEASE)
        ksft_eq(val, 0, "Autorelease should be 0 after setting")
    except OSError as e:
        if e.errno == errno.ENOPROTOOPT:
            raise KsftSkipEx("SO_DEVMEM_AUTORELEASE not supported")
        raise
    finally:
        sock.close()


def check_sockopt_autorelease_set_1(cfg) -> None:
    """Test setting SO_DEVMEM_AUTORELEASE to 1"""
    SO_DEVMEM_AUTORELEASE = 85

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        # First set to 0
        sock.setsockopt(socket.SOL_SOCKET, SO_DEVMEM_AUTORELEASE, 0)
        # Then set back to 1
        sock.setsockopt(socket.SOL_SOCKET, SO_DEVMEM_AUTORELEASE, 1)
        val = sock.getsockopt(socket.SOL_SOCKET, SO_DEVMEM_AUTORELEASE)
        ksft_eq(val, 1, "Autorelease should be 1 after setting")
    except OSError as e:
        if e.errno == errno.ENOPROTOOPT:
            raise KsftSkipEx("SO_DEVMEM_AUTORELEASE not supported")
        raise
    finally:
        sock.close()


def check_sockopt_autorelease_invalid(cfg) -> None:
    """Test that SO_DEVMEM_AUTORELEASE rejects invalid values"""
    SO_DEVMEM_AUTORELEASE = 85

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        try:
            sock.setsockopt(socket.SOL_SOCKET, SO_DEVMEM_AUTORELEASE, 2)
            raise Exception("setsockopt should have failed with EINVAL")
        except OSError as e:
            if e.errno == errno.ENOPROTOOPT:
                raise KsftSkipEx("SO_DEVMEM_AUTORELEASE not supported")
            ksft_eq(e.errno, errno.EINVAL, "Should fail with EINVAL for invalid value")
    finally:
        sock.close()


def main() -> None:
    with NetDrvEpEnv(__file__) as cfg:
        cfg.bin_local = path.abspath(path.dirname(__file__) + "/ncdevmem")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

        ksft_run([check_rx, check_tx, check_tx_chunks,
                  check_autorelease_enabled,
                  check_autorelease_disabled,
                  check_sockopt_autorelease_default,
                  check_sockopt_autorelease_set_0,
                  check_sockopt_autorelease_set_1,
                  check_sockopt_autorelease_invalid],
                 args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
