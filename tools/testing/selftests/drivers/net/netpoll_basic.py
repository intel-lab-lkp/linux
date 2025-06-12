#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

# This test aims to evaluate the netpoll polling mechanism (as in netpoll_poll_dev()).
# It presents a complex scenario where the network attempts to send a packet but fails,
# prompting it to poll the NIC from within the netpoll TX side.
#
# This has been a crucial path in netpoll that was previously untested. Jakub
# suggested using a single RX/TX queue, pushing traffic to the NIC, and then sending
# netpoll messages (via netconsole) to trigger the poll. `perf` probing of netpoll_poll_dev()
# showed that this test indeed triggers netpoll_poll_dev() once or twice in 10 iterations.

# Author: Breno Leitao <leitao@debian.org>

import errno
import os
import random
import string
import time

from lib.py import (
    ethtool,
    GenerateTraffic,
    ksft_exit,
    ksft_pr,
    ksft_run,
    KsftFailEx,
    KsftSkipEx,
    NetdevFamily,
    NetDrvEpEnv,
)

NETCONSOLE_CONFIGFS_PATH = "/sys/kernel/config/netconsole"
REMOTE_PORT = 6666
LOCAL_PORT = 1514
# Number of netcons messages to send. I usually see netpoll_poll_dev()
# being called at least once in 10 iterations.
ITERATIONS = 10
DEBUG = False


def generate_random_netcons_name() -> str:
    """Generate a random name starting with 'netcons'"""
    random_suffix = "".join(random.choices(string.ascii_lowercase + string.digits, k=8))
    return f"netcons_{random_suffix}"


def get_stats(cfg: NetDrvEpEnv, netdevnl: NetdevFamily) -> dict[str, int]:
    """Get the statistics for the interface"""
    return netdevnl.qstats_get({"ifindex": cfg.ifindex}, dump=True)[0]


def set_single_rx_tx_queue(interface_name: str) -> None:
    """Set the number of RX and TX queues to 1 using ethtool"""
    try:
        # This don't need to be reverted, since interfaces will be deleted after test
        ethtool(f"-G {interface_name} rx 1 tx 1")
    except Exception as e:
        raise KsftSkipEx(
            f"Failed to configure RX/TX queues: {e}. Ethtool not available?"
        )


def create_netconsole_target(
    config_data: dict[str, str],
    target_name: str,
) -> None:
    """Create a netconsole dynamic target against the interfaces"""
    ksft_pr(f"Using netconsole name: {target_name}")
    try:
        ksft_pr(f"Created target directory: {NETCONSOLE_CONFIGFS_PATH}/{target_name}")
        os.makedirs(f"{NETCONSOLE_CONFIGFS_PATH}/{target_name}", exist_ok=True)
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise KsftFailEx(f"Failed to create netconsole target directory: {e}")

    try:
        for key, value in config_data.items():
            if DEBUG:
                ksft_pr(f"Setting {key} to {value}")
            with open(
                f"{NETCONSOLE_CONFIGFS_PATH}/{target_name}/{key}",
                "w",
                encoding="utf-8",
            ) as f:
                # Always convert to string to write to file
                f.write(str(value))
                f.close()

        if DEBUG:
            # Read all configuration values for debugging
            for debug_key in config_data.keys():
                with open(
                    f"{NETCONSOLE_CONFIGFS_PATH}/{target_name}/{debug_key}",
                    "r",
                    encoding="utf-8",
                ) as f:
                    content = f.read()
                    ksft_pr(
                        f"{NETCONSOLE_CONFIGFS_PATH}/{target_name}/{debug_key} {content}"
                    )

    except Exception as e:
        raise KsftFailEx(f"Failed to configure netconsole target: {e}")


def set_netconsole(cfg: NetDrvEpEnv, interface_name: str, target_name: str) -> None:
    """Configure netconsole on the interface with the given target name"""
    config_data = {
        "extended": "1",
        "dev_name": interface_name,
        "local_port": LOCAL_PORT,
        "remote_port": REMOTE_PORT,
        "local_ip": cfg.addr_v["4"] if cfg.addr_ipver == "4" else cfg.addr_v["6"],
        "remote_ip": (
            cfg.remote_addr_v["4"] if cfg.addr_ipver == "4" else cfg.remote_addr_v["6"]
        ),
        "remote_mac": "00:00:00:00:00:00",  # Not important for this test
        "enabled": "1",
    }

    create_netconsole_target(config_data, target_name)
    ksft_pr(f"Created netconsole target: {target_name} on interface {interface_name}")


def delete_netconsole_target(name: str) -> None:
    """Delete a netconsole dynamic target"""
    target_path = f"{NETCONSOLE_CONFIGFS_PATH}/{name}"
    try:
        if os.path.exists(target_path):
            os.rmdir(target_path)
    except OSError as e:
        raise KsftFailEx(f"Failed to delete netconsole target: {e}")


def check_traffic_flowing(cfg: NetDrvEpEnv, netdevnl: NetdevFamily) -> int:
    """Check if traffic is flowing on the interface"""
    stat1 = get_stats(cfg, netdevnl)
    time.sleep(1)
    stat2 = get_stats(cfg, netdevnl)
    pkts_per_sec = stat2["rx-packets"] - stat1["rx-packets"]
    # Just make sure this will not fail even in slow/debug kernels
    if pkts_per_sec < 10:
        raise KsftFailEx(f"Traffic seems low: {pkts_per_sec}")
    if DEBUG:
        ksft_pr(f"Traffic per second {pkts_per_sec} ", pkts_per_sec)

    return pkts_per_sec


def do_netpoll_flush(cfg: NetDrvEpEnv, netdevnl: NetdevFamily) -> None:
    """Print messages to the console, trying to trigger a netpoll poll"""
    for i in range(int(ITERATIONS)):
        pkts_per_s = check_traffic_flowing(cfg, netdevnl)
        with open("/dev/kmsg", "w", encoding="utf-8") as kmsg:
            kmsg.write(f"netcons test #{i}: ({pkts_per_s} packets/s)\n")


def test_netpoll(cfg: NetDrvEpEnv, netdevnl: NetdevFamily) -> None:
    """Test netpoll by sending traffic to the interface and then sending netconsole messages to trigger a poll"""
    target_name = generate_random_netcons_name()
    ifname = cfg.dev["ifname"]
    traffic = None

    try:
        set_single_rx_tx_queue(ifname)
        traffic = GenerateTraffic(cfg)
        check_traffic_flowing(cfg, netdevnl)
        set_netconsole(cfg, ifname, target_name)
        do_netpoll_flush(cfg, netdevnl)
    finally:
        if traffic:
            traffic.stop()
        delete_netconsole_target(target_name)


def check_dependencies() -> None:
    """Check if the dependencies are met"""
    if not os.path.exists(NETCONSOLE_CONFIGFS_PATH):
        raise KsftSkipEx(
            f"Directory {NETCONSOLE_CONFIGFS_PATH} does not exist. CONFIG_NETCONSOLE_DYNAMIC might not be set."
        )


def main() -> None:
    """Main function to run the test"""
    check_dependencies()
    netdevnl = NetdevFamily()
    with NetDrvEpEnv(__file__, nsim_test=True) as cfg:
        ksft_run(
            [test_netpoll],
            args=(
                cfg,
                netdevnl,
            ),
        )
    ksft_exit()


if __name__ == "__main__":
    main()
