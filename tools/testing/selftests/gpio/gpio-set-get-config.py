#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2024 Collabora Ltd

#
# This test validates GPIO pin configuration. It takes a test plan in YAML (see
# gpio-set-get-config-example-test-plan.yaml) and sets and gets back each pin
# configuration described in the plan and checks that they match in order to
# validate that they are being applied correctly.
#
# When the file name for the test plan is not provided through --test-plan, it
# will be guessed based on the platform ID (DT compatible or DMI).
#

import time
import os
import sys
import argparse
import re
import subprocess
import glob
import signal

import yaml

# Allow ksft module to be imported from different directory
this_dir = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(this_dir, "../kselftest/"))

import ksft


def config_pin(chip_dev, pin_config):
    flags = []
    if pin_config.get("bias"):
        flags += f"-b {pin_config['bias']}".split()
    flags += ["-w", chip_dev, str(pin_config["pin"])]
    gpio_mockup_cdev_path = os.path.join(this_dir, "gpio-mockup-cdev")
    return subprocess.Popen([gpio_mockup_cdev_path] + flags)


def get_bias_debugfs(chip_debugfs_path, pin):
    with open(os.path.join(chip_debugfs_path, "pinconf-pins")) as f:
        for l in f:
            m = re.match(rf"pin {pin}.*bias (?P<bias>(pull )?\w+)", l)
            if m:
                return m.group("bias")


def check_config_pin(chip, chip_debugfs_dir, pin_config):
    test_passed = True

    if pin_config.get("bias"):
        bias = get_bias_debugfs(chip_debugfs_dir, pin_config["pin"])
        # Convert "pull up" / "pull down" to "pull-up" / "pull-down"
        bias = bias.replace(" ", "-")
        if bias != pin_config["bias"]:
            ksft.print_msg(
                f"Bias doesn't match: Expected {pin_config['bias']}, read {bias}."
            )
            test_passed = False

    ksft.test_result(
        test_passed,
        f"{chip['label']}.{pin_config['pin']}.{pin_config['bias']}",
    )


def get_devfs_chip_file(chip_dict):
    gpio_chip_info_path = os.path.join(this_dir, 'gpio-chip-info')
    for f in glob.glob("/dev/gpiochip*"):
        proc = subprocess.run(
            f"{gpio_chip_info_path} {f} label".split(), capture_output=True, text=True
        )
        if proc.returncode:
            ksft.print_msg(f"Error opening gpio device {f}: {proc.returncode}")
            ksft.exit_fail()

        if chip_dict["label"] in proc.stdout:
            return f


def get_debugfs_chip_dir(chip):
    pinctrl_debugfs = "/sys/kernel/debug/pinctrl/"

    for name in os.listdir(pinctrl_debugfs):
        if chip["label"] in name:
            return os.path.join(pinctrl_debugfs, name)


def run_test(test_plan_filename):
    ksft.print_msg(f"Using test plan file: {test_plan_filename}")

    with open(test_plan_filename) as f:
        plan = yaml.safe_load(f)

    num_tests = 0
    for chip in plan:
        num_tests += len(chip["tests"])

    ksft.set_plan(num_tests)

    for chip in plan:
        chip_dev = get_devfs_chip_file(chip)
        if not chip_dev:
            ksft.print_msg("Couldn't find /dev file for GPIO chip")
            ksft.exit_fail()
        chip_debugfs_dir = get_debugfs_chip_dir(chip)
        if not chip_debugfs_dir:
            ksft.print_msg("Couldn't find pinctrl folder in debugfs for GPIO chip")
            ksft.exit_fail()
        for pin_config in chip["tests"]:
            proc = config_pin(chip_dev, pin_config)
            time.sleep(0.1)  # Give driver some time to update pin
            check_config_pin(chip, chip_debugfs_dir, pin_config)
            proc.send_signal(signal.SIGTERM)
            proc.wait()


def get_possible_test_plan_filenames():
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


def get_test_plan_filename(test_plan_dir):
    chosen_test_plan_filename = ""
    full_test_plan_paths = [
        os.path.join(test_plan_dir, f + ".yaml")
        for f in get_possible_test_plan_filenames()
    ]
    for path in full_test_plan_paths:
        if os.path.exists(path):
            chosen_test_plan_filename = path
            break

    if not chosen_test_plan_filename:
        tried_paths = ",".join(["'" + p + "'" for p in full_test_plan_paths])
        ksft.print_msg(f"No matching test plan file found (tried {tried_paths})")
        ksft.print_cnts()
        sys.exit(4)

    return chosen_test_plan_filename


parser = argparse.ArgumentParser()
parser.add_argument(
    "--test-plan-dir", default=".", help="Directory containing the test plan files"
)
parser.add_argument("--test-plan", help="Test plan file to use")
args = parser.parse_args()

ksft.print_header()

if args.test_plan:
    test_plan_filename = os.path.join(args.test_plan_dir, args.test_plan)
    if not os.path.exists(test_plan_filename):
        ksft.print_msg(f"Test plan file not found: {test_plan_filename}")
        ksft.exit_fail()
else:
    test_plan_filename = get_test_plan_filename(args.test_plan_dir)

run_test(test_plan_filename)

ksft.finished()
