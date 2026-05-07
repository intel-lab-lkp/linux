#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""generate_rust_analyzer - Generates the `rust-project.json` file for `rust-analyzer`.
"""

import argparse
from dataclasses import dataclass
from datetime import date
import enum
import re
import json
import logging
import os
import pathlib
import subprocess
import sys
from typing import Dict, Iterable, List, Literal, Optional, TypedDict

def invoke_rustc(args: List[str]) -> str:
    return subprocess.check_output(
        [os.environ["RUSTC"]] + args,
        stdin=subprocess.DEVNULL,
    ).decode('utf-8').strip()

def args_crates_cfgs(cfgs: List[str]) -> Dict[str, List[str]]:
    crates_cfgs = {}
    for cfg in cfgs:
        crate, vals = cfg.split("=", 1)
        crates_cfgs[crate] = vals.split()

    return crates_cfgs

class Dependency(TypedDict):
    crate: int
    name: str


class Source(TypedDict):
    include_dirs: List[str]
    exclude_dirs: List[str]


# TODO: clean up once Python 3.11 is adopted.
if sys.version_info < (3, 11):
    class Crate(TypedDict, total=False):
        display_name: str
        root_module: str
        is_workspace_member: bool
        deps: List[Dependency]
        cfg: List[str]
        crate_attrs: List[str]
        edition: str
        env: Dict[str, str]
else:
    from typing import NotRequired
    class Crate(TypedDict):
        display_name: str
        root_module: str
        is_workspace_member: bool
        deps: List[Dependency]
        cfg: List[str]
        crate_attrs: NotRequired[List[str]]
        edition: str
        env: Dict[str, str]


class ProcMacroCrate(Crate):
    is_proc_macro: Literal[True]
    proc_macro_dylib_path: str  # `pathlib.Path` is not JSON serializable.


class CrateWithGenerated(Crate):
    source: Source


@dataclass(frozen=True)
class RaVersionCtx:
    manual_sysroot_crates: bool
    use_crate_attrs: bool


def generate_crates(
    ctx: RaVersionCtx,
    srctree: pathlib.Path,
    objtree: pathlib.Path,
    sysroot_src: pathlib.Path,
    external_src: Optional[pathlib.Path],
    cfgs: List[str],
    core_edition: str,
) -> List[Crate]:
    # Generate the configuration list.
    generated_cfg = []
    with open(objtree / "include" / "generated" / "rustc_cfg") as fd:
        for line in fd:
            line = line.replace("--cfg=", "")
            line = line.replace("\n", "")
            generated_cfg.append(line)

    # Now fill the crates list.
    crates: List[Crate] = []
    crates_cfgs = args_crates_cfgs(cfgs)

    def get_crate_name(path: pathlib.Path) -> str:
        return invoke_rustc(["--print", "crate-name", str(path)])

    def build_crate(
        display_name: str,
        root_module: pathlib.Path,
        deps: List[Optional[Dependency]],
        *,
        cfg: Optional[List[str]],
        crate_attrs: Optional[List[str]],
        is_workspace_member: Optional[bool],
        edition: Optional[str],
    ) -> Crate:
        filtered_deps = [dep for dep in deps if dep is not None]
        cfg = cfg if cfg is not None else crates_cfgs.get(display_name, [])
        is_workspace_member = (
            is_workspace_member if is_workspace_member is not None else True
        )
        edition = edition if edition is not None else "2021"
        crate: Crate = {
            "display_name": display_name,
            "root_module": str(root_module),
            "is_workspace_member": is_workspace_member,
            "deps": filtered_deps,
            "cfg": cfg,
            "edition": edition,
            "env": {
                "RUST_MODFILE": "This is only for rust-analyzer"
            }
        }

        if ctx.use_crate_attrs and crate_attrs is not None:
            crate["crate_attrs"] = crate_attrs

        return crate

    def append_proc_macro_crate(
        display_name: str,
        root_module: pathlib.Path,
        deps: List[Optional[Dependency]],
        *,
        cfg: Optional[List[str]] = None,
        is_workspace_member: Optional[bool] = None,
        edition: Optional[str] = None,
    ) -> Dependency:
        crate = build_crate(
            display_name,
            root_module,
            deps,
            cfg=cfg,
            crate_attrs=None,
            is_workspace_member=is_workspace_member,
            edition=edition,
        )
        proc_macro_dylib_name = invoke_rustc([
            "--print",
            "file-names",
            "--crate-name",
            display_name,
            "--crate-type",
            "proc-macro",
            "-",
        ])
        proc_macro_crate: ProcMacroCrate = {
            **crate,
            "is_proc_macro": True,
            "proc_macro_dylib_path": str(objtree / "rust" / proc_macro_dylib_name),
        }
        return register_crate(proc_macro_crate)

    def register_crate(crate: Crate) -> Dependency:
        index = len(crates)
        crates.append(crate)
        return {"crate": index, "name": crate["display_name"]}

    def append_crate(
        display_name: str,
        root_module: pathlib.Path,
        deps: List[Optional[Dependency]],
        *,
        cfg: Optional[List[str]] = None,
        crate_attrs: Optional[List[str]] = None,
        is_workspace_member: Optional[bool] = None,
        edition: Optional[str] = None,
    ) -> Dependency:
        return register_crate(
            build_crate(
                display_name,
                root_module,
                deps,
                cfg=cfg,
                crate_attrs=crate_attrs,
                is_workspace_member=is_workspace_member,
                edition=edition,
            )
        )

    def append_sysroot_crate(
        display_name: str,
        deps: List[Optional[Dependency]],
        *,
        cfg: Optional[List[str]] = None,
    ) -> Optional[Dependency]:
        if not ctx.manual_sysroot_crates:
            return None
        return append_crate(
            display_name,
            sysroot_src / display_name / "src" / "lib.rs",
            deps,
            cfg=cfg,
            is_workspace_member=False,
            # Miguel Ojeda writes:
            #
            # > ... in principle even the sysroot crates may have different
            # > editions.
            # >
            # > For instance, in the move to 2024, it seems all happened at once
            # > in 1.87.0 in these upstream commits:
            # >
            # >     0e071c2c6a58 ("Migrate core to Rust 2024")
            # >     f505d4e8e380 ("Migrate alloc to Rust 2024")
            # >     0b2489c226c3 ("Migrate proc_macro to Rust 2024")
            # >     993359e70112 ("Migrate std to Rust 2024")
            # >
            # > But in the previous move to 2021, `std` moved in 1.59.0, while
            # > the others in 1.60.0:
            # >
            # >     b656384d8398 ("Update stdlib to the 2021 edition")
            # >     06a1c14d52a8 ("Switch all libraries to the 2021 edition")
            #
            # Link: https://lore.kernel.org/all/CANiq72kd9bHdKaAm=8xCUhSHMy2csyVed69bOc4dXyFAW4sfuw@mail.gmail.com/
            #
            # At the time of writing all rust versions we support build the
            # sysroot crates with the same edition. We may need to relax this
            # assumption if future edition moves span multiple rust versions.
            edition=core_edition,
        )

    # NB: sysroot crates reexport items from one another so setting up our transitive dependencies
    # here is important for ensuring that rust-analyzer can resolve symbols. The sources of truth
    # for this dependency graph are `(sysroot_src / crate / "Cargo.toml" for crate in crates)`.
    core = append_sysroot_crate("core", [])
    alloc = append_sysroot_crate("alloc", [core])
    std = append_sysroot_crate("std", [alloc, core])
    proc_macro = append_sysroot_crate("proc_macro", [core, std])

    compiler_builtins = append_crate(
        "compiler_builtins",
        srctree / "rust" / "compiler_builtins.rs",
        [core],
    )

    proc_macro2 = append_crate(
        "proc_macro2",
        srctree / "rust" / "proc-macro2" / "lib.rs",
        [core, alloc, std, proc_macro],
    )

    quote = append_crate(
        "quote",
        srctree / "rust" / "quote" / "lib.rs",
        [core, alloc, std, proc_macro, proc_macro2],
        edition="2018",
    )

    syn = append_crate(
        "syn",
        srctree / "rust" / "syn" / "lib.rs",
        [std, proc_macro, proc_macro2, quote],
    )

    macros = append_proc_macro_crate(
        "macros",
        srctree / "rust" / "macros" / "lib.rs",
        [std, proc_macro, proc_macro2, quote, syn],
    )

    build_error = append_crate(
        "build_error",
        srctree / "rust" / "build_error.rs",
        [core, compiler_builtins],
    )

    pin_init_internal = append_proc_macro_crate(
        "pin_init_internal",
        srctree / "rust" / "pin-init" / "internal" / "src" / "lib.rs",
        [std, proc_macro, proc_macro2, quote, syn],
    )

    pin_init = append_crate(
        "pin_init",
        srctree / "rust" / "pin-init" / "src" / "lib.rs",
        [core, compiler_builtins, pin_init_internal, macros],
    )

    ffi = append_crate(
        "ffi",
        srctree / "rust" / "ffi.rs",
        [core, compiler_builtins],
    )

    def append_crate_with_generated(
        display_name: str,
        deps: List[Optional[Dependency]],
    ) -> Dependency:
        crate = build_crate(
            display_name,
            srctree / "rust"/ display_name / "lib.rs",
            deps,
            cfg=generated_cfg,
            crate_attrs=None,
            is_workspace_member=True,
            edition=None,
        )
        crate["env"]["OBJTREE"] = str(objtree.resolve(True))
        crate_with_generated: CrateWithGenerated = {
            **crate,
            "source": {
                "include_dirs": [
                    str(srctree / "rust" / display_name),
                    str(objtree / "rust"),
                ],
                "exclude_dirs": [],
            },
        }
        return register_crate(crate_with_generated)

    bindings = append_crate_with_generated("bindings", [core, ffi, pin_init])
    uapi = append_crate_with_generated("uapi", [core, ffi, pin_init])
    kernel = append_crate_with_generated(
        "kernel", [core, macros, build_error, pin_init, ffi, bindings, uapi]
    )

    scripts = srctree / "scripts"
    makefile = (scripts / "Makefile").read_text()
    for path in scripts.glob("*.rs"):
        name = path.stem
        if f"{name}-rust" not in makefile:
            continue
        append_crate(
            name,
            path,
            [std],
        )

    def is_root_crate(build_file: pathlib.Path, target: str) -> bool:
        try:
            contents = build_file.read_text()
        except FileNotFoundError:
            return False
        return f"{target}.o" in contents

    # Then, the rest outside of `rust/`.
    #
    # We explicitly mention the top-level folders we want to cover.
    extra_dirs: Iterable[pathlib.Path] = (
        srctree / dir for dir in ("samples", "drivers")
    )
    if external_src is not None:
        extra_dirs = [external_src]
    for folder in extra_dirs:
        for path in folder.rglob("*.rs"):
            logging.info("Checking %s", path)
            file_name = path.stem

            # Skip those that are not crate roots.
            if not is_root_crate(path.parent / "Makefile", file_name) and \
               not is_root_crate(path.parent / "Kbuild", file_name):
                continue

            crate_name = get_crate_name(path)
            logging.info("Adding %s", crate_name)
            append_crate(
                crate_name,
                path,
                [core, kernel, pin_init],
                cfg=generated_cfg,
                crate_attrs=["no_std"],
            )

    return crates


Version = tuple[int, int, int]


@enum.unique
class RaVersionInfo(enum.Enum):
    """
    Represents rust-analyzer compatibility baselines. Concrete versions are
    mapped to the most recent baseline they have reached. Must be in release
    order.
    """

    # NOTE:
    # This rust-analyzer release should be kept in sync with our MSRV (currently
    # 1.85.0). When the MSRV is bumped, follow the steps below to retrieve the
    # information needed to update this:
    #
    #   1) Clone both Rust and rust-analyzer repositories.
    #      ```console
    #      $ git clone https://github.com/rust-lang/rust.git
    #      $ git clone https://github.com/rust-lang/rust-analyzer.git
    #      ```
    #   2) Run the following script, providing the new MSRV as an argument. It
    #      prints a link to the matching [1] rust-analyzer release page.
    #      ```bash
    #      #!/usr/bin/env bash
    #
    #      set -euo pipefail
    #
    #      RUST_VERSION=$1
    #
    #      grep_args=()
    #      while IFS= read -r subject; do
    #        grep_args+=(--grep "$subject")
    #      done < <(git -C rust log \
    #          --fixed-strings \
    #          --format='%s' \
    #          --grep 'Merge pull request #' \
    #          --merges \
    #          --no-follow \
    #          -n 10 \
    #          "$RUST_VERSION" \
    #          -- src/tools/rust-analyzer
    #      )
    #
    #      tag_predates=$(
    #        git -C rust-analyzer log \
    #          --fixed-strings \
    #          --format='%(describe:tags,abbrev=0)' \
    #          --merges \
    #          -n 1 \
    #          "${grep_args[@]}"
    #      )
    #
    #      link_prefix="https://github.com/rust-lang/rust-analyzer/releases/tag"
    #      echo "$link_prefix/$tag_predates"
    #      ```
    #   3) Grab the release date and the version string.
    #
    # [1] Note that rust-analyzer releases may not perfectly align with those
    #     shipped in upstream Rust. We take a conservative approach here: use
    #     the tag that directly predates the latest merge commit found upstream.
    #
    # v0.3.2228, released on 2024-12-23;
    # shipped with the rustup 1.85.0 toolchain.
    MSRV = (
        date(2024, 12, 23),
        (0, 3, 2228),
        (1, 85, 0),
        RaVersionCtx(
            use_crate_attrs=False,
            manual_sysroot_crates=True,
        ),
    )
    # v0.3.2727, released on 2025-12-22;
    # v0.3.2743 is shipped with the rustup 1.94.0 toolchain.
    SUPPROTS_CRATE_ATTRS = (
        date(2025, 12, 22),
        (0, 3, 2727),
        (1, 94, 0),
        RaVersionCtx(
            use_crate_attrs=True,
            manual_sysroot_crates=False,
        ),
    )

    def __init__(
        self,
        release_date: date,
        ra_version: Version,
        rust_version: Version,
        ctx: RaVersionCtx,
    ) -> None:
        self.release_date = release_date
        self.ra_version = ra_version
        self.rust_version = rust_version
        self.ctx = ctx


# TODO: clean up once Python 3.11 is adopted.
if sys.version_info < (3, 11):
    class RustProject(TypedDict, total=False):
        crates: List[Crate]
        sysroot: str
        sysroot_src: str
else:
    from typing import NotRequired
    class RustProject(TypedDict):
        crates: List[Crate]
        sysroot: str
        sysroot_src: NotRequired[str]


def generate_rust_project(
    version_info: RaVersionInfo,
    srctree: pathlib.Path,
    objtree: pathlib.Path,
    sysroot: pathlib.Path,
    sysroot_src: pathlib.Path,
    external_src: Optional[pathlib.Path],
    cfgs: List[str],
    core_edition: str,
) -> RustProject:
    ctx = version_info.ctx
    rust_project: RustProject = {
        "crates": generate_crates(
            ctx, srctree, objtree, sysroot_src, external_src, cfgs, core_edition
        ),
        "sysroot": str(sysroot),
    }

    if not ctx.manual_sysroot_crates:
        rust_project["sysroot_src"] = str(sysroot_src)

    return rust_project

def query_ra_version() -> Optional[str]:
    try:
        # Use the rust-analyzer binary found in $PATH.
        ra_version_output = (
            subprocess.check_output(
                ["rust-analyzer", "--version"],
                stdin=subprocess.DEVNULL,
            )
            .decode("utf-8")
            .strip()
        )
        return ra_version_output
    except FileNotFoundError:
        return None

def map_ra_version_baseline(ra_version_output: str) -> RaVersionInfo:
    baselines = list(reversed(RaVersionInfo))

    version_match = re.search(r"\d+\.\d+\.\d+", ra_version_output)
    if version_match:
        version_string = version_match.group()
        found_version = tuple(map(int, version_string.split(".")))

        # `rust-analyzer --version` shows a different version string depending
        # on how the binary is built: it may print either the Rust version or
        # the rust-analyzer version itself. To distinguish between them, we
        # leverage rust-analyzer's versioning convention.
        #
        # See:
        # - https://github.com/rust-lang/rust-analyzer/blob/fad5c3d2d642/xtask/src/dist.rs#L19-L21
        is_ra_version = version_string.startswith(("0.3", "0.4", "0.5"))
        if is_ra_version:
            for info in baselines:
                if found_version >= info.ra_version:
                    return info
        else:
            for info in baselines:
                if found_version >= info.rust_version:
                    return info

    date_match = re.search(r"\d{4}-\d{2}-\d{2}", ra_version_output)
    if date_match:
        found_date = date.fromisoformat(date_match.group())
        for info in baselines:
            if found_date >= info.release_date:
                return info

    return RaVersionInfo.MSRV

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--verbose', '-v', action='store_true')
    parser.add_argument('--cfgs', action='append', default=[])
    parser.add_argument("core_edition")
    parser.add_argument("srctree", type=pathlib.Path)
    parser.add_argument("objtree", type=pathlib.Path)
    parser.add_argument("sysroot", type=pathlib.Path)
    parser.add_argument("sysroot_src", type=pathlib.Path)
    parser.add_argument("exttree", type=pathlib.Path, nargs="?")

    class Args(argparse.Namespace):
        verbose: bool
        cfgs: List[str]
        srctree: pathlib.Path
        objtree: pathlib.Path
        sysroot: pathlib.Path
        sysroot_src: pathlib.Path
        exttree: Optional[pathlib.Path]
        core_edition: str

    args = parser.parse_args(namespace=Args())

    logging.basicConfig(
        format="[%(asctime)s] [%(levelname)s] %(message)s",
        level=logging.INFO if args.verbose else logging.WARNING
    )

    ra_version_output = query_ra_version()
    if ra_version_output:
        compatible_ra_version = map_ra_version_baseline(ra_version_output)
    else:
        logging.warning(
            "Failed to find rust-analyzer in $PATH; " \
            "falling back to `rust-project.json` for rust-analyzer " \
            "%s, %s (shipped with Rust %s)",
            ".".join(map(str, RaVersionInfo.MSRV.ra_version)),
            RaVersionInfo.MSRV.release_date,
            ".".join(map(str, RaVersionInfo.MSRV.rust_version)),
        )
        compatible_ra_version = RaVersionInfo.MSRV

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
