#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Test port split configuration using devlink-port lanes attribute.
The test is skipped in case the attribute is not available.

First, check that all the ports with 1 lane fail to split.
Second, check that all the ports with more than 1 lane can be split
to all valid configurations (e.g., split to 2, split to 4 etc.)
"""

import collections
import json
import os

from lib.py import ksft_run, ksft_exit, ksft_pr
from lib.py import ksft_eq, ksft_disruptive
from lib.py import NetDrvEnv
from lib.py import KsftSkipEx
from lib.py import cmd


Port = collections.namedtuple('Port', 'bus_info name')


def get_devlink_ports(dev):
    """
    Get a list of physical devlink ports.
    Return: Array of tuples (bus_info, name).
    """

    arr = []

    result = cmd("devlink -j port show")
    ports = json.loads(result.stdout)['port']

    validate_devlink_output(ports, 'flavour')

    for port in ports:
        if dev in port:
            if ports[port]['flavour'] == 'physical':
                arr.append(Port(bus_info=port, name=ports[port]['netdev']))

    return arr


def get_max_lanes(port):
    """
    Get the $port's maximum number of lanes.
    Return: number of lanes, e.g. 1, 2, 4 and 8.
    """

    result = cmd(f"devlink -j port show {port}")
    values = list(json.loads(result.stdout)['port'].values())[0]

    if 'lanes' in values:
        lanes = values['lanes']
    else:
        lanes = 0
    return lanes


def get_split_ability(port):
    """
    Get the $port split ability.
    Return: split ability, true or false.
    """

    result = cmd(f"devlink -j port show {port.name}")
    values = list(json.loads(result.stdout)['port'].values())[0]

    return values['splittable']


def exists(port, dev):
    """
    Check if $port exists in the devlink ports.
    Return: True is so, False otherwise.
    """

    return any(dev_port.name == port
               for dev_port in get_devlink_ports(dev))


def exists_and_lanes(ports, lanes, dev):
    """
    Check if every port in the list $ports exists in the devlink ports and has
    $lanes number of lanes after splitting.
    Return: True if both are True, False otherwise.
    """

    for port in ports:
        max_lanes = get_max_lanes(port)
        if not exists(port, dev):
            ksft_pr(f"port {port} doesn't exist in devlink ports")
            return False
        if max_lanes != lanes:
            ksft_pr(f"port {port} has {max_lanes} lanes, "
                    f"but {lanes} were expected")
            return False
    return True


def create_split_group(port, k):
    """
    Create the split group for $port.
    Return: Array with $k elements, which are the split port group.
    """

    return list(port.name + "s" + str(i) for i in range(k))


def split_unsplittable_port(port, k):
    """
    Test that splitting of unsplittable port fails.
    """

    result = cmd(f"devlink port split {port.bus_info} count {k}", fail=False)
    if result.ret == 0:
        ksft_pr(f"split an unsplittable port {port.name}")
        cmd(f"devlink port unsplit {port.bus_info}")
    ksft_eq(result.ret != 0, True, f"{port.name} is unsplittable")


def split_splittable_port(port, k, lanes, dev):
    """
    Test that splitting of splittable port passes correctly.
    """

    result = cmd(f"devlink port split {port.bus_info} count {k}", fail=False)

    if result.ret != 0:
        ksft_pr(f"didn't split a splittable port {port.name}")
        return

    # Once the split command ends, it takes some time to the sub ifaces'
    # to get their names. Use udevadm to continue only when all current udev
    # events are handled.
    cmd("udevadm settle")

    new_split_group = create_split_group(port, k)
    ksft_eq(exists_and_lanes(new_split_group, lanes / k, dev), True,
            f"split port {port.name} into {k}")

    cmd(f"devlink port unsplit {port.bus_info}")


def validate_devlink_output(devlink_data, target_property=None):
    """
    Determine if test should be skipped by checking:
      1. devlink_data contains values
      2. The target_property exist in devlink_data
    """
    skip_reason = None
    if any(devlink_data.values()):
        if target_property:
            skip_reason = f"{target_property} not found in devlink output, test skipped"
            for key in devlink_data:
                if target_property in devlink_data[key]:
                    skip_reason = None
    else:
        skip_reason = 'devlink output is empty, test skipped'

    if skip_reason:
        raise KsftSkipEx(skip_reason)


@ksft_disruptive
def test_port_split(cfg):
    """Test port split configuration using devlink-port lanes attribute."""
    dev = f"pci/{cfg.pci}"

    result = cmd(f"devlink dev show {dev}", fail=False)
    if result.ret != 0:
        raise KsftSkipEx(f"devlink device {dev} can not be found")

    ports = get_devlink_ports(dev)

    found_max_lanes = False
    for port in ports:
        max_lanes = get_max_lanes(port.name)

        # If max lanes is 0, do not test port splitting at all
        if max_lanes == 0:
            continue

        # If 1 lane, shouldn't be able to split
        elif max_lanes == 1:
            ksft_eq(get_split_ability(port), False,
                    f"{port.name} should not be able to split")
            split_unsplittable_port(port, max_lanes)

        # Else, splitting should pass and all the split ports should exist.
        else:
            lane = max_lanes
            ksft_eq(get_split_ability(port), True,
                    f"{port.name} should be able to split")
            while lane > 1:
                split_splittable_port(port, lane, max_lanes, dev)

                lane //= 2
        found_max_lanes = True

    if not found_max_lanes:
        raise KsftSkipEx(f"No port of device {dev} reports max_lanes")


def main() -> None:
    """Ksft boiler plate main"""
    with NetDrvEnv(__file__, nsim_test=False) as cfg:
        cfg.pci = os.path.basename(
            os.path.realpath(f"/sys/class/net/{cfg.ifname}/device")
        )
        ksft_run([test_port_split], args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
