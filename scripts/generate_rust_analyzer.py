#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""generate_rust_analyzer - Generates the `rust-project.json` file for `rust-analyzer`.
"""

import argparse
import json
import logging
import os
import pathlib
import subprocess
import sys
import typing as T

def args_crates_cfgs(cfgs: T.Iterable[str]) -> dict[str, list[str]]:
    crates_cfgs = {}
    for cfg in cfgs:
        crate, vals = cfg.split("=", 1)
        crates_cfgs[crate] = vals.replace("--cfg", "").split()

    return crates_cfgs

class Dependency(T.TypedDict):
    crate: int
    name: str

class Source(T.TypedDict):
    include_dirs: list[str]
    exclude_dirs: list[str]

class Crate(T.TypedDict):
    display_name: str
    root_module: str
    is_workspace_member: bool
    is_proc_macro: bool
    deps: list[Dependency]
    cfg: list[str]
    edition: T.Literal["2021"]
    env: dict[str, str]
    # `NotRequired` would be better but was added in 3.11.
    proc_macro_dylib_path: T.Optional[str]
    source: T.Optional[Source]

def generate_crates(
    srctree: pathlib.Path,
    objtree: pathlib.Path,
    sysroot_src: pathlib.Path,
    external_src: pathlib.Path,
    cfgs: list[str],
) -> list[Crate]:
    # Generate the configuration list.
    cfg = []
    with open(objtree / "include" / "generated" / "rustc_cfg") as fd:
        for line in fd:
            line = line.replace("--cfg=", "")
            line = line.replace("\n", "")
            cfg.append(line)

    # Now fill the crates list.
    crates: list[Crate] = []
    crates_cfgs = args_crates_cfgs(cfgs)

    def append_crate(
        display_name: str,
        root_module: pathlib.Path,
        deps: list[Dependency],
        cfg: list[str] = [],
        is_workspace_member: bool = True,
        is_proc_macro: bool = False,
    ) -> Dependency:
        proc_macro_dylib_path = None
        if is_proc_macro:
            proc_macro_dylib_name = subprocess.check_output(
                [os.environ["RUSTC"], "--print", "file-names", "--crate-name", display_name, "--crate-type", "proc-macro", "-"],
                stdin=subprocess.DEVNULL,
            ).decode('utf-8').strip()
            proc_macro_dylib_path = f"{objtree}/rust/{proc_macro_dylib_name}"
        index = len(crates)
        crates.append(
            {
                "display_name": display_name,
                "root_module": str(root_module),
                "is_workspace_member": is_workspace_member,
                "is_proc_macro": is_proc_macro,
                "deps": deps,
                "cfg": cfg,
                "edition": "2021",
                "env": {"RUST_MODFILE": "This is only for rust-analyzer"},
                "proc_macro_dylib_path": proc_macro_dylib_path,
                "source": None,
            }
        )
        return {"crate": index, "name": display_name}

    # First, the ones in `rust/` since they are a bit special.
    core = append_crate(
        "core",
        sysroot_src / "core" / "src" / "lib.rs",
        [],
        cfg=crates_cfgs.get("core", []),
        is_workspace_member=False,
    )

    compiler_builtins = append_crate(
        "compiler_builtins",
        srctree / "rust" / "compiler_builtins.rs",
        [],
    )

    macros = append_crate(
        "macros",
        srctree / "rust" / "macros" / "lib.rs",
        [],
        is_proc_macro=True,
    )

    build_error = append_crate(
        "build_error",
        srctree / "rust" / "build_error.rs",
        [core, compiler_builtins],
    )

    bindings = append_crate(
        "bindings",
        srctree / "rust"/ "bindings" / "lib.rs",
        [core],
        cfg=cfg,
    )
    crates[-1]["env"]["OBJTREE"] = str(objtree.resolve(True))

    kernel = append_crate(
        "kernel",
        srctree / "rust" / "kernel" / "lib.rs",
        [core, macros, build_error, bindings],
        cfg=cfg,
    )
    crates[-1]["source"] = {
        "include_dirs": [
            str(srctree / "rust" / "kernel"),
            str(objtree / "rust")
        ],
        "exclude_dirs": [],
    }

    def is_root_crate(build_file: pathlib.Path, target: str) -> bool:
        try:
            return f"{target}.o" in open(build_file).read()
        except FileNotFoundError:
            return False

    # Then, the rest outside of `rust/`.
    #
    # We explicitly mention the top-level folders we want to cover.
    extra_dirs: T.Iterable[pathlib.Path] = map(
        lambda dir: srctree / dir, ("samples", "drivers")
    )
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
                [core, kernel],
                cfg=cfg,
            )

    return crates

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--verbose', '-v', action='store_true')
    parser.add_argument('--cfgs', action='append', default=[])
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

    rust_project = {
        "crates": generate_crates(args.srctree, args.objtree, args.sysroot_src, args.exttree, args.cfgs),
        "sysroot": str(args.sysroot),
    }

    json.dump(rust_project, sys.stdout, sort_keys=True, indent=4)

if __name__ == "__main__":
    main()
