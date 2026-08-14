#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Chen Miao <chenmiao.ku@gmail.com>

"""Detect a supported GNU Make executable."""

import re
import shutil
import subprocess
import sys

from kdoc.python_version import PythonVersion


MIN_GMAKE_VERSION = PythonVersion("4.0").version


def get_gmake_version(cmd):
    """Return the GNU Make version for *cmd*, or ``None`` otherwise."""
    if not cmd:
        return None

    kwargs = {}
    if sys.version_info < (3, 7):
        kwargs["universal_newlines"] = True
    else:
        kwargs["text"] = True

    try:
        result = subprocess.run(
            [cmd, "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
            **kwargs,
        )
    except (OSError, subprocess.CalledProcessError):
        return None

    match = re.search(
        r"^GNU Make\s+([0-9]+(?:\.[0-9]+)*)", result.stdout, re.MULTILINE
    )
    if not match:
        return None

    return PythonVersion.parse_version(match.group(1))


def find_gmake(make=None):
    """Return the first GNU Make 4.0+ from MAKE, gmake, or make."""
    candidates = (
        make,
        shutil.which("gmake"),
        shutil.which("make"),
    )

    for cmd in candidates:
        version = get_gmake_version(cmd)
        if version and version >= MIN_GMAKE_VERSION:
            return cmd

    return None
