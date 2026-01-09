#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""generate_rust_analyzer - Generates the `rust-project.json` file for `rust-analyzer`.
"""

import argparse
from datetime import datetime
import enum
import json
import logging
import os
import pathlib
import re
import subprocess
import sys

def args_crates_cfgs(cfgs):
    crates_cfgs = {}
    for cfg in cfgs:
        crate, vals = cfg.split("=", 1)
        crates_cfgs[crate] = vals.split()

    return crates_cfgs

def generate_crates(ctx, srctree, objtree, sysroot_src, external_src, cfgs, core_edition):
    # Generate the configuration list.
    cfg = []
    with open(objtree / "include" / "generated" / "rustc_cfg") as fd:
        for line in fd:
            line = line.replace("--cfg=", "")
            line = line.replace("\n", "")
            cfg.append(line)

    # Now fill the crates list -- dependencies need to come first.
    #
    # Avoid O(n^2) iterations by keeping a map of indexes.
    crates = []
    crates_indexes = {}
    crates_cfgs = args_crates_cfgs(cfgs)

    def append_crate(display_name, root_module, deps, cfg=[], crate_attrs=[], is_workspace_member=True, is_proc_macro=False, edition="2021"):
        crate = {
            "display_name": display_name,
            "root_module": str(root_module),
            "is_workspace_member": is_workspace_member,
            "is_proc_macro": is_proc_macro,
            "deps": [{"crate": crates_indexes[dep], "name": dep} for dep in deps],
            "cfg": cfg,
            "edition": edition,
            "env": {
                "RUST_MODFILE": "This is only for rust-analyzer"
            }
        }
        if ctx["use_crate_attrs"] and len(crate_attrs) > 0:
            crate["crate_attrs"] = crate_attrs
        if is_proc_macro:
            proc_macro_dylib_name = subprocess.check_output(
                [os.environ["RUSTC"], "--print", "file-names", "--crate-name", display_name, "--crate-type", "proc-macro", "-"],
                stdin=subprocess.DEVNULL,
            ).decode('utf-8').strip()
            crate["proc_macro_dylib_path"] = f"{objtree}/rust/{proc_macro_dylib_name}"
        crates_indexes[display_name] = len(crates)
        crates.append(crate)

    def append_sysroot_crate(
        display_name,
        deps,
        cfg=[],
        edition="2021",
    ):
        append_crate(
            display_name,
            sysroot_src / display_name / "src" / "lib.rs",
            deps,
            cfg,
            is_workspace_member=False,
            edition=edition,
        )

    def sysroot_deps(*deps):
        return list(deps) if ctx["add_sysroot_crates"] else []

    if ctx["add_sysroot_crates"]:
        # NB: sysroot crates reexport items from one another so setting up our transitive dependencies
        # here is important for ensuring that rust-analyzer can resolve symbols. The sources of truth
        # for this dependency graph are `(sysroot_src / crate / "Cargo.toml" for crate in crates)`.
        append_sysroot_crate("core", [], cfg=crates_cfgs.get("core", []), edition=core_edition)
        append_sysroot_crate("alloc", ["core"])
        append_sysroot_crate("std", ["alloc", "core"])
        append_sysroot_crate("proc_macro", ["core", "std"])

    append_crate(
        "compiler_builtins",
        srctree / "rust" / "compiler_builtins.rs",
        [],
        crate_attrs=["no_std"],
    )

    append_crate(
        "proc_macro2",
        srctree / "rust" / "proc-macro2" / "lib.rs",
        sysroot_deps("core", "alloc", "std", "proc_macro"),
        cfg=crates_cfgs["proc_macro2"],
    )

    append_crate(
        "quote",
        srctree / "rust" / "quote" / "lib.rs",
        sysroot_deps("alloc", "proc_macro") + ["proc_macro2"],
        cfg=crates_cfgs["quote"],
    )

    append_crate(
        "syn",
        srctree / "rust" / "syn" / "lib.rs",
        sysroot_deps("proc_macro") + ["proc_macro2", "quote"],
        cfg=crates_cfgs["syn"],
    )

    append_crate(
        "macros",
        srctree / "rust" / "macros" / "lib.rs",
        sysroot_deps("std", "proc_macro") + ["proc_macro2", "quote", "syn"],
        is_proc_macro=True,
    )

    append_crate(
        "build_error",
        srctree / "rust" / "build_error.rs",
        sysroot_deps("core") + ["compiler_builtins"],
        crate_attrs=["no_std"],
    )

    append_crate(
        "pin_init_internal",
        srctree / "rust" / "pin-init" / "internal" / "src" / "lib.rs",
        [],
        cfg=["kernel"],
        crate_attrs=["no_std"],
        is_proc_macro=True,
    )

    append_crate(
        "pin_init",
        srctree / "rust" / "pin-init" / "src" / "lib.rs",
        sysroot_deps("core") + ["pin_init_internal", "macros"],
        cfg=["kernel"],
        crate_attrs=["no_std"],
    )

    append_crate(
        "ffi",
        srctree / "rust" / "ffi.rs",
        sysroot_deps("core") + ["compiler_builtins"],
        crate_attrs=["no_std"],
    )

    def append_crate_with_generated(
        display_name,
        deps,
        crate_attrs=[]
    ):
        append_crate(
            display_name,
            srctree / "rust"/ display_name / "lib.rs",
            deps,
            cfg=cfg,
            crate_attrs=crate_attrs
        )
        crates[-1]["env"]["OBJTREE"] = str(objtree.resolve(True))
        crates[-1]["source"] = {
            "include_dirs": [
                str(srctree / "rust" / display_name),
                str(objtree / "rust")
            ],
            "exclude_dirs": [],
        }

    append_crate_with_generated(
        "bindings",
        sysroot_deps("core") + ["ffi", "pin_init"],
        crate_attrs=["no_std"],
    )
    append_crate_with_generated(
        "uapi",
        sysroot_deps("core") + ["ffi", "pin_init"],
        crate_attrs=["no_std"],
    )
    append_crate_with_generated(
        "kernel",
        sysroot_deps("core") + ["macros", "build_error", "pin_init", "ffi", "bindings", "uapi"],
        crate_attrs=["no_std"],
    )

    def is_root_crate(build_file, target):
        try:
            return f"{target}.o" in open(build_file).read()
        except FileNotFoundError:
            return False

    # Then, the rest outside of `rust/`.
    #
    # We explicitly mention the top-level folders we want to cover.
    extra_dirs = map(lambda dir: srctree / dir, ("samples", "drivers"))
    if external_src is not None:
        extra_dirs = [external_src]
    for folder in extra_dirs:
        for path in folder.rglob("*.rs"):
            logging.info("Checking %s", path)
            name = path.name.replace(".rs", "")

            # Skip those that are not crate roots.
            if not is_root_crate(path.parent / "Makefile", name) and \
               not is_root_crate(path.parent / "Kbuild", name):
                continue

            logging.info("Adding %s", name)
            append_crate(
                name,
                path,
                sysroot_deps("core") + ["kernel"],
                cfg=cfg,
                crate_attrs=["no_std"]
            )

    return crates

@enum.unique
class RaVersion(enum.Enum):
    """
    Represents rust-analyzer compatibility baselines. Concrete versions are mapped to the most
    recent baseline they have reached. Must be in release order.
    """

    # v0.3.1940, released on 2024-04-29; bundled with the rustup 1.78 toolchain.
    V20240429 = 0

    @staticmethod
    def baselines():
        assert len(RaVersion) == 1, "Exhaustiveness check: update baseline list!"

        return [
            (datetime.strptime("2024-04-29", "%Y-%m-%d"), (0, 3, 1940), RaVersion.V20240429),
        ]

    @staticmethod
    def default():
        # The default is the 2024-04-29 release, aligning with our MSRV policy.
        return RaVersion.V20240429

    def __str__(self):
        assert len(RaVersion) == 1, "Exhaustiveness check: update if branches!"

        if self == RaVersion.V20240429:
            return "v0.3.1940 (2024-04-29)"
        else:
            assert False, "Unreachable"

def generate_rust_project(
    ra_version,
    srctree,
    objtree,
    sysroot,
    sysroot_src,
    external_src,
    cfgs,
    core_edition
):
    assert len(RaVersion) == 1, "Exhaustiveness check: update if branches!"

    if ra_version == RaVersion.V20240429:
        ctx = {
            "use_crate_attrs": False,
            "add_sysroot_crates": True,
        }
        return {
            "crates": generate_crates(ctx, srctree, objtree, sysroot_src, external_src, cfgs, core_edition),
            "sysroot": str(sysroot),
        }
    else:
        assert False, "Unreachable"

def query_ra_version():
    try:
        # Use the rust-analyzer binary found in $PATH.
        ra_version_output = subprocess.check_output(
            ["rust-analyzer", "--version"],
            stdin=subprocess.DEVNULL,
        ).decode('utf-8').strip()
        return ra_version_output
    except FileNotFoundError:
        logging.warning("Failed to find rust-analyzer in $PATH")
        return None

def map_ra_version_baseline(ra_version_output):
    checkpoints = reversed(RaVersion.baselines())

    # First, attempt to resolve to our known checkpoint using the release date.
    # This covers patterns like "rust-analyzer 1.78.0 (9b00956e 2024-04-29)".
    date_match = re.search(r"\d{4}-\d{2}-\d{2}", ra_version_output)
    if date_match:
        found_date = datetime.strptime(date_match.group(), "%Y-%m-%d")
        for date, ver, enum in checkpoints:
            if found_date >= date:
                return enum

    # Otherwise, attempt to resolve to our known checkpoint using the rust-analyzer version.
    # This covers patterns like "rust-analyzer 0.3.2743-standalone".
    version_match = re.search(r"\d+\.\d+\.\d+", ra_version_output)
    if version_match:
        found_version = tuple(map(int, version_match.group().split(".")))
        for date, ver, enum in checkpoints:
            if found_version >= ver:
                return enum

    return RaVersion.default()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--verbose', '-v', action='store_true')
    parser.add_argument('--cfgs', action='append', default=[])
    parser.add_argument("core_edition")
    parser.add_argument("srctree", type=pathlib.Path)
    parser.add_argument("objtree", type=pathlib.Path)
    parser.add_argument("sysroot", type=pathlib.Path)
    parser.add_argument("sysroot_src", type=pathlib.Path)
    parser.add_argument("exttree", type=pathlib.Path, nargs="?")
    args = parser.parse_args()

    logging.basicConfig(
        format="[%(asctime)s] [%(levelname)s] %(message)s",
        level=logging.INFO if args.verbose else logging.WARNING
    )

    # Making sure that the `sysroot` and `sysroot_src` belong to the same toolchain.
    assert args.sysroot in args.sysroot_src.parents

    output = query_ra_version()
    if output:
        compatible_ra_version = map_ra_version_baseline(output)
    else:
        default = RaVersion.default()
        logging.warning("Falling back to `rust-project.json` for rust-analyzer %s", default)
        compatible_ra_version = default

    rust_project = generate_rust_project(
        compatible_ra_version,
        args.srctree,
        args.objtree,
        args.sysroot,
        args.sysroot_src,
        args.exttree,
        args.cfgs,
        args.core_edition,
    )

    json.dump(rust_project, sys.stdout, sort_keys=True, indent=4)

if __name__ == "__main__":
    main()
