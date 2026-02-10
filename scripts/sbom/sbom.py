#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2025 TNG Technology Consulting GmbH

"""
Compute software bill of materials in SPDX format describing a kernel build.
"""

import logging
import sys
import sbom.sbom_logging as sbom_logging
from sbom.config import get_config


def main():
    # Read config
    config = get_config()

    # Configure logging
    logging.basicConfig(
        level=logging.DEBUG if config.debug else logging.INFO,
        format="[%(levelname)s] %(message)s",
    )

    # Report collected warnings and errors in case of failure
    warning_summary = sbom_logging.summarize_warnings()
    error_summary = sbom_logging.summarize_errors()

    if warning_summary:
        logging.warning(warning_summary)
    if error_summary:
        logging.error(error_summary)
        sys.exit(1)


# Call main method
if __name__ == "__main__":
    main()
