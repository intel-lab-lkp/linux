# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2025 TNG Technology Consulting GmbH

import re

from sbom.path_utils import PathStr

# Regex patterns for dependencies that should not be tracked in the dependency graph.
_COMPILED_EXCLUDE_PATTERNS: tuple[re.Pattern[str], ...] = (
    # scripts in source tree (except module-common.c, which is compiled into every module)
    re.compile(r"^\.\./scripts/(?!module-common\.c$)"),
    # scripts and tools in obj tree
    re.compile(r"^scripts/"),
    re.compile(r"^tools/"),
    re.compile(r"^arch/[^/]+/tools/"),
    # Host helper scripts
    re.compile(r".*\.sh$"),
    re.compile(r".*\.py$"),
    re.compile(r".*\.pl$"),
    # Host-tool executables
    re.compile(r"^certs/extract-cert$"),
    re.compile(r"^security/selinux/genheaders$"),
    re.compile(r"^usr/gen_init_cpio$"),
    re.compile(r"^drivers/gpu/drm/radeon/mkregtable$"),
    re.compile(r"^drivers/video/logo/pnmtologo$"),
    re.compile(r"^lib/raid/raid6/mktables$"),
    re.compile(r"^\.\./fs/smb/client/gen_smb1_mapping$"),
    re.compile(r"^\.\./fs/smb/client/gen_smb2_mapping$"),
    # Other
    re.compile(r".*\.xsd$"),
    re.compile(r".*purgatory\.chk$"),
    re.compile(r".*\.genkey$"),
    re.compile(r".*\.md$"),
    re.compile(r"^include/config/"),
    # x86
    re.compile(r"^arch/x86/boot/compressed/mkpiggy$"),
    re.compile(r"^arch/x86/boot/mkcpustr$"),
    # arm
    re.compile(r"^arch/arm/vdso/vdsomunge$"),
    # arm64
    re.compile(r"^arch/arm64/kvm/hyp/nvhe/gen-hyprel$"),
    re.compile(r"^arch/arm64/kernel/pi/relacheck$"),
)

def should_include_dependency(path: PathStr) -> bool:
    """
    Return whether a `.cmd` dependency should appear in the tracked dependency graph.

    Track files that are inputs to compiling or linking artifacts that end up in the
    kernel image or loadable modules (e.g. source → object → archive → image/module).
    Exclude host tools and helper scripts whose code is not linked into the kernel.

    Args:
        path: Dependency path relative to the object tree.

    Returns:
        True if the dependency should be included in the dependency graph.
    """
    return not any(pattern.match(path) for pattern in _COMPILED_EXCLUDE_PATTERNS)
