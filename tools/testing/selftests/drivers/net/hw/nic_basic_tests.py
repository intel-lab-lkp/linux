#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

#Introduction:
#This file has basic tests for generic NIC drivers.
#The test comprises of auto-negotiation, speed and duplex checks.
#Also has tests to check the throughput
#
#Setup:
#Connect the DUT PC with NIC card to partner pc back via ethernet medium of your choice(RJ45, T1)
#
#        DUT PC                                              Partner PC
#┌───────────────────────┐                         ┌──────────────────────────┐
#│                       │                         │                          │
#│                       │                         │                          │
#│           ┌───────────┐                         │                          │
#│           │DUT NIC    │         Eth             │                          │
#│           │Interface ─┼─────────────────────────┼─    any eth Interface    │
#│           └───────────┘                         │                          │
#│                       │                         │                          │
#│                       │                         │                          │
#└───────────────────────┘                         └──────────────────────────┘
#
#Configurations:
# Change the below configuration based on your hw needs.
# """Default values"""
sleep_time = 5 #time taken to wait for transitions to happen, in seconds.
test_duration = 5  #performance test duration for the throughput check, in seconds.
throughput_threshold = 0.8 #percentage of throughput required to pass the throughput

import time
import os
import re
import configparser
import json
from lib.py import ksft_run, ksft_exit, ksft_pr, ksft_eq
from lib.py import KsftFailEx, KsftSkipEx
from lib.py import NetDrvEpEnv
from lib.py import cmd
from lib.py import ethtool

"""Global variables"""
common_link_modes = []

def get_ethtool_content(ifname: str, field: str):
    capture = False
    content = []

    """Get the ethtool content for the interface"""
    process = ethtool(f"{ifname}")
    if process.ret != 0:
        raise KsftSkipEx(f"Error while getting the ethtool content for interface {ifname}")
    lines = process.stdout.splitlines()

    """Retrieve the content of the field"""
    for line in lines:
        if field in line:
            capture = True
            data = line.split(":")[1].strip()
            content.extend(data.split())
            continue

        if capture:
            if ":" in line:
                break;
            if line.strip():
                content.extend(line.strip().split())
    if capture == False:
        raise KsftSkipEx(f"Field \"{field}\" not found in ethtool output")
    return content

def get_speed_duplex(content):
    speed = []
    duplex = []
    """Check the link modes"""
    for data in content:
        parts = data.split('/')
        speed_value = re.match(r'\d+', parts[0])
        if speed_value:
            speed.append(speed_value.group())
        else:
            raise KsftSkipEx(f"No speed value found for interface {ifname}")
        duplex.append(parts[1].lower())
    return speed, duplex

def verify_link_up(ifname: str) -> None:
    """Verify whether the link is up"""
    with open(f"/sys/class/net/{ifname}/operstate", "r") as fp:
        link_state = fp.read().strip()

    if link_state == "down":
        raise KsftSkipEx(f"Link state of interface {ifname} is DOWN")

def set_autonegotiation_state(ifname: str, state: str) -> None:
    content = get_ethtool_content(ifname, "Supported link modes:")
    speeds, duplex_modes = get_speed_duplex(content)
    speed = speeds[0]
    duplex = duplex_modes[0]
    if not speed or not duplex:
        KsftSkipEx("No speed or duplex modes found")
    """Set the autonegotiation state for the interface"""
    process = ethtool(f"-s {ifname} speed {speed} duplex {duplex} autoneg {state}")
    if process.ret != 0:
        raise KsftFailEx(f"Not able to set autoneg parameter for {ifname}")
    ksft_pr(f"Autoneg set as {state} for {ifname}")

def verify_autonegotiation(ifname: str, expected_state: str) -> None:
    verify_link_up(ifname)
    """Verifying the autonegotiation state"""
    output = get_ethtool_content(ifname, "Auto-negotiation:")
    actual_state = output[0]

    ksft_eq(actual_state, expected_state)

def test_link_modes(cfg) -> None:
    global common_link_modes
    link_modes = get_ethtool_content(cfg.ifname, "Supported link modes:")
    partner_link_modes = get_ethtool_content(cfg.ifname, "Link partner advertised link modes:")

    if link_modes and partner_link_modes:
        for idx1 in range(len(link_modes)):
            for idx2 in range(len(partner_link_modes)):
                if link_modes[idx1] == partner_link_modes[idx2]:
                    common_link_modes.append(link_modes[idx1])
                    break
    else:
        raise KsftFailEx("No link modes available")

def test_autonegotiation(cfg) -> None:
    autoneg = get_ethtool_content(cfg.ifname, "Supports auto-negotiation:")
    if autoneg[0] == "Yes":
        for state in ["off", "on"]:
            set_autonegotiation_state(cfg.ifname, state)
            time.sleep(sleep_time)
            verify_autonegotiation(cfg.ifname, state)
    else:
        raise KsftSkipEx(f"Auto-Negotiation is not supported for interface {cfg.ifname}")

def main() -> None:
    with NetDrvEpEnv(__file__) as cfg:
        ksft_run(globs=globals(), case_pfx={"test_"}, args=(cfg,))
    ksft_exit()

if __name__ == "__main__":
    main()
