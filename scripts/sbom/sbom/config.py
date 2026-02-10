# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2025 TNG Technology Consulting GmbH

import argparse
from dataclasses import dataclass


@dataclass
class KernelSbomConfig:
    debug: bool
    """Whether to enable debug logging."""


def _parse_cli_arguments() -> dict[str, bool]:
    """
    Parse command-line arguments using argparse.

    Returns:
        Dictionary of parsed arguments.
    """
    parser = argparse.ArgumentParser(
        description="Generate SPDX SBOM documents for kernel builds",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        default=False,
        help="Enable debug logs (default: False)",
    )

    args = vars(parser.parse_args())
    return args


def get_config() -> KernelSbomConfig:
    """
    Parse command-line arguments and construct the configuration object.

    Returns:
        KernelSbomConfig: Configuration object with all settings for SBOM generation.
    """
    # Parse cli arguments
    args = _parse_cli_arguments()

    debug = args["debug"]

    return KernelSbomConfig(debug=debug)
