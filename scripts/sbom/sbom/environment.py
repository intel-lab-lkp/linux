# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2025 TNG Technology Consulting GmbH

import os


class Environment:
    """
    Read-only accessor for kernel build environment variables.
    """

    @classmethod
    def SRCARCH(cls) -> str | None:
        return os.getenv("SRCARCH")
