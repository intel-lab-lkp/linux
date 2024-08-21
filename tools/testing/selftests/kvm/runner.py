#!/usr/bin/env python3

import argparse
import json
import subprocess
import os
import platform
import logging
import contextlib
import textwrap
import shutil

from pathlib import Path
from multiprocessing import Pool

logging.basicConfig(level=logging.INFO,
                    format = "%(asctime)s | %(process)d | %(levelname)8s | %(message)s")

class Command:
    """Executes a command

    Execute a command.
    """
    def __init__(self, id, command, timeout=None, command_artifacts_dir=None):
        self.id = id
        self.args = command
        self.timeout = timeout
        self.command_artifacts_dir = command_artifacts_dir

    def __run(self, command, timeout=None, output=None, error=None):
            proc=subprocess.run(command, stdout=output,
                                stderr=error, universal_newlines=True,
                                shell=True, timeout=timeout)
            return proc.returncode

    def run(self):
        output = None
        error = None
        with contextlib.ExitStack() as stack:
            if self.command_artifacts_dir is not None:
                output_path = os.path.join(self.command_artifacts_dir, f"{self.id}.stdout")
                error_path = os.path.join(self.command_artifacts_dir, f"{self.id}.stderr")
                output = stack.enter_context(open(output_path, encoding="utf-8", mode = "w"))
                error = stack.enter_context(open(error_path, encoding="utf-8", mode = "w"))
            return self.__run(self.args, self.timeout, output, error)

COMMAND_TIMED_OUT = "TIMED_OUT"
COMMAND_PASSED = "PASSED"
COMMAND_FAILED = "FAILED"
COMMAND_SKIPPED = "SKIPPED"
SETUP_FAILED = "SETUP_FAILED"
TEARDOWN_FAILED = "TEARDOWN_FAILED"

def run_command(command):
    if command is None:
        return COMMAND_PASSED

    try:
        ret = command.run()
        if ret == 0:
            return COMMAND_PASSED
        elif ret == 4:
            return COMMAND_SKIPPED
        else:
            return COMMAND_FAILED
    except subprocess.TimeoutExpired as e:
        logging.error(type(e).__name__ + str(e))
        return COMMAND_TIMED_OUT

class Test:
    """A single test.

    A test which can be run on its own.
    """
    def __init__(self, test_json, timeout=None, suite_dir=None):
        self.name = test_json["name"]
        self.test_artifacts_dir = None
        self.setup_command = None
        self.teardown_command = None

        if suite_dir is not None:
            self.test_artifacts_dir = os.path.join(suite_dir, self.name)

        test_timeout = test_json.get("timeout_s", timeout)

        self.test_command = Command("command", test_json["command"], test_timeout, self.test_artifacts_dir)
        if "setup" in test_json:
            self.setup_command = Command("setup", test_json["setup"], test_timeout, self.test_artifacts_dir)
        if "teardown" in test_json:
            self.teardown_command = Command("teardown", test_json["teardown"], test_timeout, self.test_artifacts_dir)

    def run(self):
        if self.test_artifacts_dir is not None:
            Path(self.test_artifacts_dir).mkdir(parents=True, exist_ok=True)

        setup_status = run_command(self.setup_command)
        if setup_status != COMMAND_PASSED:
            return SETUP_FAILED

        try:
            status = run_command(self.test_command)
            return status
        finally:
            teardown_status = run_command(self.teardown_command)
            if (teardown_status != COMMAND_PASSED
                    and (status == COMMAND_PASSED or status == COMMAND_SKIPPED)):
                return TEARDOWN_FAILED

def run_test(test):
    return test.run()

class Suite:
    """Collection of tests to run

    Group of tests.
    """
    def __init__(self, suite_json, platform_arch, artifacts_dir, test_filter):
        self.suite_name = suite_json["suite"]
        self.suite_artifacts_dir = None
        self.setup_command = None
        self.teardown_command = None
        timeout = suite_json.get("timeout_s", None)

        if artifacts_dir is not None:
            self.suite_artifacts_dir = os.path.join(artifacts_dir, self.suite_name)

        if "setup" in suite_json:
            self.setup_command = Command("setup", suite_json["setup"], timeout, self.suite_artifacts_dir)
        if "teardown" in suite_json:
            self.teardown_command = Command("teardown", suite_json["teardown"], timeout, self.suite_artifacts_dir)

        self.tests = []
        for test_json in suite_json["tests"]:
            if len(test_filter) > 0 and test_json["name"] not in test_filter:
                continue;
            if test_json.get("arch") is None or test_json["arch"] == platform_arch:
                self.tests.append(Test(test_json, timeout, self.suite_artifacts_dir))

    def run(self, jobs=1):
        result = {}
        if len(self.tests) == 0:
            return COMMAND_PASSED, result

        if self.suite_artifacts_dir is not None:
            Path(self.suite_artifacts_dir).mkdir(parents = True, exist_ok = True)

        setup_status = run_command(self.setup_command)
        if setup_status != COMMAND_PASSED:
            return SETUP_FAILED, result


        if jobs > 1:
            with Pool(jobs) as p:
                tests_status = p.map(run_test, self.tests)
            for i,test in enumerate(self.tests):
                logging.info(f"{tests_status[i]}: {self.suite_name}/{test.name}")
                result[test.name] = tests_status[i]
        else:
            for test in self.tests:
                status = run_test(test)
                logging.info(f"{status}: {self.suite_name}/{test.name}")
                result[test.name] = status

        teardown_status = run_command(self.teardown_command)
        if teardown_status != COMMAND_PASSED:
            return TEARDOWN_FAILED, result


        return COMMAND_PASSED, result

def load_tests(path):
    with open(path) as f:
        tests = json.load(f)
    return tests


def run_suites(suites, jobs):
    """Runs the tests.

    Run test suits in the tests file.
    """
    result = {}
    for suite in suites:
        result[suite.suite_name] = suite.run(jobs)
    return result

def parse_test_filter(test_suite_or_test):
    test_filter = {}
    if len(test_suite_or_test) == 0:
        return test_filter
    for test in test_suite_or_test:
        test_parts = test.split("/")
        if len(test_parts) > 2:
            raise ValueError("Incorrect format of suite/test_name combo")
        if test_parts[0] not in test_filter:
            test_filter[test_parts[0]] = []
        if len(test_parts) == 2:
            test_filter[test_parts[0]].append(test_parts[1])

    return test_filter

def parse_suites(suites_json, platform_arch, artifacts_dir, test_suite_or_test):
    suites = []
    test_filter = parse_test_filter(test_suite_or_test)
    for suite_json in suites_json:
        if len(test_filter) > 0 and suite_json["suite"] not in test_filter:
            continue
        if suite_json.get("arch") is None or suite_json["arch"] == platform_arch:
            suites.append(Suite(suite_json,
                                platform_arch,
                                artifacts_dir,
                                test_filter.get(suite_json["suite"], [])))
    return suites


def pretty_print(result):
    logging.info("--------------------------------------------------------------------------")
    if not result:
        logging.warning("No test executed.")
        return
    logging.info("Test runner result:")
    suite_count = 0
    test_count = 0
    for suite_name, suite_result in result.items():
        suite_count += 1
        logging.info(f"{suite_count}) {suite_name}:")
        if suite_result[0] != COMMAND_PASSED:
            logging.info(f"\t{suite_result[0]}")
        test_count = 0
        for test_name, test_result in suite_result[1].items():
            test_count += 1
            if test_result == "PASSED":
                logging.info(f"\t{test_count}) {test_result}: {test_name}")
            else:
                logging.error(f"\t{test_count}) {test_result}: {test_name}")
    logging.info("--------------------------------------------------------------------------")

def args_parser():
    parser = argparse.ArgumentParser(
        prog = "KVM Selftests Runner",
        description = "Run KVM selftests with different configurations",
        formatter_class=argparse.RawTextHelpFormatter
    )

    parser.add_argument("-o","--output",
                        help="Creates a folder to dump test results.")
    parser.add_argument("-j", "--jobs", default = 1, type = int,
                        help="Number of parallel executions in a suite")
    parser.add_argument("test_suites_json",
                        help = "File containing test suites to run")

    test_suite_or_test_help = textwrap.dedent("""\
                               Run specific test suite or specific test from the test suite.
                               If nothing specified then run all of the tests.

                               Example:
                                   runner.py tests.json A/a1 A/a4 B C/c1

                               Assuming capital letters are test suites and small letters are tests.
                               Runner will:
                               - Run test a1 and a4 from the test suite A
                               - Run all tests from the test suite B
                               - Run test c1 from the test suite C"""
                               )
    parser.add_argument("test_suite_or_test", nargs="*", help=test_suite_or_test_help)


    return parser.parse_args();

def main():
    args = args_parser()
    suites_json = load_tests(args.test_suites_json)
    suites = parse_suites(suites_json, platform.machine(),
                          args.output, args.test_suite_or_test)

    if args.output is not None:
        shutil.rmtree(args.output, ignore_errors=True)
    result = run_suites(suites, args.jobs)
    pretty_print(result)

if __name__ == "__main__":
    main()
