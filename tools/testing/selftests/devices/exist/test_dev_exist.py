#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2024 Collabora Ltd

import os
import sys
import argparse
import gzip
import json

# Allow ksft module to be imported from different directory
this_dir = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(this_dir, "../../kselftest/"))

import ksft


def generate_ref_metadata():
    metadata = {}

    config_file = "/proc/config.gz"
    if os.path.isfile(config_file):
        with gzip.open(config_file, "r") as f:
            config = str(f.read())
        metadata["config"] = config

    metadata["version"] = os.uname().release

    metadata["platform_ids"] = get_possible_ref_filenames()

    return metadata


def generate_dev_data():
    data = {}

    device_subsys_types = [
        {
            "type": "class",
            "base_dir": "/sys/class",
            "add_path": "",
            "ignored": [],
        },
        {
            "type": "bus",
            "base_dir": "/sys/bus",
            "add_path": "devices",
            "ignored": [],
        },
    ]

    properties = sorted(
        [
            ".",
            "uevent",
            "name",
            "device",
            "firmware_node",
            "driver",
            "device/uevent",
            "firmware_node/uevent",
        ]
    )

    for dev_subsys_type in device_subsys_types:
        subsystems = {}
        for subsys_name in sorted(os.listdir(dev_subsys_type["base_dir"])):
            if subsys_name in dev_subsys_type["ignored"]:
                continue

            devs_path = os.path.join(
                dev_subsys_type["base_dir"], subsys_name, dev_subsys_type["add_path"]
            )
            # Filter out non-symlinks as they're not devices
            dev_dirs = [dev for dev in os.scandir(devs_path) if dev.is_symlink()]
            devs_data = []
            for dev_dir in dev_dirs:
                dev_path = os.path.join(devs_path, dev_dir)
                dev_data = {"info": {}}
                for prop in properties:
                    prop_path = os.path.join(dev_path, prop)
                    if os.path.isfile(prop_path):
                        with open(prop_path) as f:
                            dev_data["info"][prop] = f.read()
                    elif os.path.isdir(prop_path):
                        dev_data["info"][prop] = os.path.realpath(prop_path)
                devs_data.append(dev_data)
            if len(dev_dirs):
                subsystems[subsys_name] = {
                    "count": len(dev_dirs),
                    "devices": devs_data,
                }
        data[dev_subsys_type["type"]] = subsystems

    return data


def generate_reference():
    return {"metadata": generate_ref_metadata(), "data": generate_dev_data()}


def commented(s):
    return s.replace("\n", "\n# ")


def indented(s, n):
    return " " * n + s.replace("\n", "\n" + " " * n)


def stripped(s):
    return s.strip("\n")


def devices_difference(dev1, dev2):
    difference = 0

    for prop in dev1["info"].keys():
        for l1, l2 in zip(
            dev1["info"].get(prop, "").split("\n"),
            dev2["info"].get(prop, "").split("\n"),
        ):
            if l1 != l2:
                difference += 1
    return difference


def guess_missing_devices(cur_subsys_data, ref_subsys_data):
    # Detect what devices on the current system are the most similar to devices
    # on the reference one by one until the leftovers are the most dissimilar
    # devices and therefore most likely the missing ones.
    found_count = cur_subsys_data["count"]
    expected_count = ref_subsys_data["count"]
    missing_count = found_count - expected_count

    diffs = []
    for cur_d in cur_subsys_data["devices"]:
        for ref_d in ref_subsys_data["devices"]:
            diffs.append((devices_difference(cur_d, ref_d), cur_d, ref_d))

    diffs.sort(key=lambda x: x[0])

    assigned_ref_devs = []
    assigned_cur_devs = []
    for diff in diffs:
        if len(assigned_ref_devs) >= expected_count - missing_count:
            break
        if diff[1] in assigned_cur_devs or diff[2] in assigned_ref_devs:
            continue
        assigned_cur_devs.append(diff[1])
        assigned_ref_devs.append(diff[2])

    missing_devices = []
    for d in ref_subsys_data["devices"]:
        if d not in assigned_ref_devs:
            missing_devices.append(d)

    return missing_devices


def dump_devices_info(cur_subsys_data, ref_subsys_data):
    def dump_device_info(dev):
        for name, val in dev["info"].items():
            ksft.print_msg(indented(name + ":", 2))
            val = stripped(val)
            if val:
                ksft.print_msg(commented(indented(val, 4)))
        ksft.print_msg("")

    ksft.print_msg("=================")
    ksft.print_msg("Devices expected:")
    ksft.print_msg("")
    for d in ref_subsys_data["devices"]:
        dump_device_info(d)
    ksft.print_msg("-----------------")
    ksft.print_msg("Devices found:")
    ksft.print_msg("")
    for d in cur_subsys_data["devices"]:
        dump_device_info(d)
    ksft.print_msg("-----------------")
    ksft.print_msg("Devices missing (best guess):")
    ksft.print_msg("")
    missing_devices = guess_missing_devices(cur_subsys_data, ref_subsys_data)
    for d in missing_devices:
        dump_device_info(d)
    ksft.print_msg("=================")


def load_reference(ref_filename):
    with open(ref_filename) as f:
        ref = json.load(f)
    return ref


def run_test(ref_filename):
    ksft.print_msg(f"Using reference file: '{ref_filename}'")

    ref_data = load_reference(ref_filename)["data"]

    num_tests = 0
    for subsys_type in ref_data.values():
        num_tests += len(subsys_type)
    ksft.set_plan(num_tests)

    cur_data = generate_dev_data()

    reference_outdated = False

    for subsys_type_name, ref_subsys_type_data in ref_data.items():
        for subsys_name, ref_subsys_data in ref_subsys_type_data.items():
            test_name = f"{subsys_type_name}.{subsys_name}"
            if not (
                cur_data.get(subsys_type_name)
                and cur_data.get(subsys_type_name).get(subsys_name)
            ):
                ksft.print_msg(f"Device subsystem '{subsys_name}' missing")
                ksft.test_result_fail(test_name)
                continue
            cur_subsys_data = cur_data[subsys_type_name][subsys_name]

            found_count = cur_subsys_data["count"]
            expected_count = ref_subsys_data["count"]
            if found_count < expected_count:
                ksft.print_msg(
                    f"Missing devices for subsystem '{subsys_name}': {expected_count - found_count} (Expected {expected_count}, found {found_count})"
                )
                dump_devices_info(cur_subsys_data, ref_subsys_data)
                ksft.test_result_fail(test_name)
            else:
                ksft.test_result_pass(test_name)
                if found_count > expected_count:
                    reference_outdated = True

        if len(cur_data[subsys_type_name]) > len(ref_subsys_type_data):
            reference_outdated = True

    if reference_outdated:
        ksft.print_msg(
            "Warning: The current system contains more devices and/or subsystems than the reference. Updating the reference is recommended."
        )


def ref_is_superset(new_ref_data, old_ref_data):
    for subsys_type in old_ref_data:
        for subsys in old_ref_data[subsys_type]:
            if subsys not in new_ref_data[subsys_type]:
                return False
            if (
                new_ref_data[subsys_type][subsys]["count"]
                < old_ref_data[subsys_type][subsys]["count"]
            ):
                return False
    return True


def get_possible_ref_filenames():
    filenames = []

    dt_board_compatible_file = "/proc/device-tree/compatible"
    if os.path.exists(dt_board_compatible_file):
        with open(dt_board_compatible_file) as f:
            for line in f:
                compatibles = [compat for compat in line.split("\0") if compat]
                filenames.extend(compatibles)
    else:
        dmi_id_dir = "/sys/devices/virtual/dmi/id"
        vendor_dmi_file = os.path.join(dmi_id_dir, "sys_vendor")
        product_dmi_file = os.path.join(dmi_id_dir, "product_name")

        with open(vendor_dmi_file) as f:
            vendor = f.read().replace("\n", "")
        with open(product_dmi_file) as f:
            product = f.read().replace("\n", "")

        filenames = [vendor + "," + product]

    return filenames


def get_ref_filename(ref_dir, should_exist=True):
    chosen_ref_filename = ""
    full_ref_paths = [
        os.path.join(ref_dir, f + ".json") for f in get_possible_ref_filenames()
    ]
    if not should_exist:
        return full_ref_paths[0]

    for path in full_ref_paths:
        if os.path.exists(path):
            chosen_ref_filename = path
            break

    if not chosen_ref_filename:
        tried_paths = ",".join(["'" + p + "'" for p in full_ref_paths])
        ksft.print_msg(f"No matching reference file found (tried {tried_paths})")
        ksft.exit_fail()

    return chosen_ref_filename


parser = argparse.ArgumentParser()
parser.add_argument(
    "--reference-dir",
    "-d",
    default=".",
    help="Directory containing the reference files",
)
parser.add_argument(
    "--reference-file", "-f", help="File name of the reference to read from or write to"
)
parser.add_argument(
    "--generate-reference",
    "-g",
    action="store_true",
    help="Generate a reference file with the devices on the running system",
)
parser.add_argument(
    "--update-reference",
    "-u",
    action="store_true",
    help="Allow overwriting the reference in-place if the existing reference is a subset of the new one",
)
args = parser.parse_args()

if args.reference_file:
    ref_filename = os.path.join(args.reference_dir, args.reference_file)
    if not os.path.exists(ref_filename) and not args.generate_reference:
        ksft.print_msg(f"Reference file not found: '{ref_filename}'")
        ksft.exit_fail()
else:
    ref_filename = get_ref_filename(args.reference_dir, not args.generate_reference)

if args.generate_reference:
    if os.path.exists(ref_filename) and not args.update_reference:
        print(
            f"Reference file '{ref_filename}' already exists; won't overwrite; aborting"
        )
        sys.exit(1)

    gen_ref = generate_reference()
    if args.update_reference and os.path.exists(ref_filename):
        loaded_ref = load_reference(ref_filename)
        if not ref_is_superset(gen_ref["data"], loaded_ref["data"]):
            print(
                f"New reference is not a superset of the existing one; skipping update for '{ref_filename}'"
            )
            sys.exit(1)

    with open(ref_filename, "w") as f:
        json.dump(gen_ref, f, indent=4)
    print(f"Reference generated to file '{ref_filename}'")
    sys.exit(0)

ksft.print_header()

run_test(ref_filename)

ksft.finished()
