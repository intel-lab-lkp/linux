#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Build a small documentation subset and verify the advanced search artifacts.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from pathlib import Path


SCRIPT = Path(__file__).resolve()
SRCTREE = SCRIPT.parents[2]
DEFAULT_SPHINXDIRS = ("kernel-hacking", "PCI")
REQUIRED_SEARCH_IDS = (
    'id="kernel-search-form"',
    'id="kernel-search-query"',
    'id="search-progress"',
    'id="kernel-search-advanced"',
    'id="kernel-search-advanced-flag"',
    'id="kernel-search-area"',
    'id="kernel-search-objtype"',
    'id="kernel-search-results"',
)
REQUIRED_SEARCH_INDEX_KEYS = (
    "docnames",
    "filenames",
    "titles",
    "objects",
    "objnames",
    "objtypes",
    "terms",
    "titleterms",
)
OPTIONAL_SEARCH_INDEX_KEYS = (
    "alltitles",
    "indexentries",
)
REQUIRED_RUNTIME_SNIPPETS = (
    "const SUMMARY_RESULT_LIMIT = 50;",
    'setSummaryPlaceholder(payload, "Loading summary...", "is-loading");',
    'setSummaryPlaceholder(payload, "Summary unavailable.", "is-error");',
    "pageSummaryLimitEnabled && payload.summaryIndex >= SUMMARY_RESULT_LIMIT",
    "documentTextCache.set(payload.requestUrl, htmlText);",
    "highlightTerms: queryState.highlightTerms,",
)


def fail(message):
    """Raise a readable assertion failure."""

    raise AssertionError(message)


def read_text(path):
    """Read a UTF-8 text file or fail with context."""

    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"Failed to read {path}: {exc}")


def ensure_file(path, description):
    """Ensure a generated file exists."""

    if not path.is_file():
        fail(f"Missing {description}: {path}")


def parse_args():
    """Parse command-line arguments."""

    parser = argparse.ArgumentParser(
        description=(
            "Build a small docs subset and verify the generated advanced "
            "search page, static assets, and search index contract."
        )
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        help=(
            "Out-of-tree build directory passed to make via O=. "
            "If omitted, a temporary directory is created."
        ),
    )
    parser.add_argument(
        "--keep-build-dir",
        action="store_true",
        help="Keep the temporary build directory after the test completes.",
    )
    parser.add_argument(
        "--make",
        default="make",
        help="Path to the make executable. Default: make.",
    )
    parser.add_argument(
        "--sphinxdirs",
        nargs="+",
        default=list(DEFAULT_SPHINXDIRS),
        help=(
            "Documentation subtrees to build via SPHINXDIRS. "
            f"Default: {' '.join(DEFAULT_SPHINXDIRS)}."
        ),
    )
    return parser.parse_args()


def prepare_build_dir(args):
    """Prepare the build directory and return it with cleanup metadata."""

    if args.build_dir:
        build_dir = args.build_dir.resolve()
        if build_dir.exists() and any(build_dir.iterdir()):
            fail(f"Build directory is not empty: {build_dir}")
        build_dir.mkdir(parents=True, exist_ok=True)
        return build_dir, False

    build_dir = Path(tempfile.mkdtemp(prefix="advanced-search-docs-"))
    return build_dir, not args.keep_build_dir


def find_sphinx_build():
    """Find a usable sphinx-build binary for the documentation build."""

    env_sphinx = os.environ.get("SPHINXBUILD")
    if env_sphinx:
        path = Path(env_sphinx).expanduser()
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)

    local_venv_sphinx = SRCTREE / ".venv" / "bin" / "sphinx-build"
    if local_venv_sphinx.is_file() and os.access(local_venv_sphinx, os.X_OK):
        return str(local_venv_sphinx)

    return shutil.which("sphinx-build")


def run_build(args, build_dir):
    """Build the configured documentation subset."""

    command = [args.make, f"O={build_dir}"]
    sphinx_build = find_sphinx_build()
    if sphinx_build:
        command.append(f"SPHINXBUILD={sphinx_build}")

    command += [
        f"SPHINXDIRS={' '.join(args.sphinxdirs)}",
        "htmldocs",
    ]
    print("$", shlex.join(command))

    subprocess.run(command, cwd=SRCTREE, check=True)

    output_dir = build_dir / "Documentation" / "output"
    if not output_dir.is_dir():
        fail(f"Expected documentation output directory was not created: {output_dir}")

    return output_dir


def find_search_roots(output_dir):
    """Find all generated HTML roots that expose advanced search."""

    roots = []
    for search_html in sorted(output_dir.rglob("search.html")):
        if search_html.parent.joinpath("searchindex.js").is_file():
            roots.append(search_html.parent)

    if not roots:
        fail(f"No generated search roots were found under {output_dir}")

    return roots


def check_search_html(search_root):
    """Verify the generated search page wiring and DOM anchors."""

    search_html_path = search_root / "search.html"
    ensure_file(search_html_path, "generated search page")
    search_html = read_text(search_html_path)

    script_markers = (
        "_static/language_data.js",
        "_static/kernel-search.js",
        "searchindex.js",
    )
    positions = []
    for marker in script_markers:
        position = search_html.find(marker)
        if position < 0:
            fail(f"search.html is missing required script reference: {marker}")
        positions.append(position)

    if positions != sorted(positions):
        fail("search.html does not keep the expected search script load order")

    for required_id in REQUIRED_SEARCH_IDS:
        if required_id not in search_html:
            fail(f"search.html is missing required advanced-search markup: {required_id}")


def check_search_assets(search_root):
    """Verify generated search artifacts and copied static assets."""

    ensure_file(search_root / "searchindex.js", "generated search index")
    ensure_file(search_root / "_static" / "language_data.js", "generated language data")

    built_kernel_search = search_root / "_static" / "kernel-search.js"
    source_kernel_search = SRCTREE / "Documentation" / "sphinx-static" / "kernel-search.js"
    ensure_file(built_kernel_search, "generated kernel-search runtime")

    built_runtime = read_text(built_kernel_search)
    source_runtime = read_text(source_kernel_search)

    if built_runtime != source_runtime:
        fail(f"Generated kernel-search.js does not match the source asset: {built_kernel_search}")

    # Keep the smoke test aligned with the hardening contract that the
    # runtime now relies on: bounded summary loading, visible summary
    # states, and the highlight-term wiring needed for summary generation.
    for snippet in REQUIRED_RUNTIME_SNIPPETS:
        if snippet not in built_runtime:
            fail(f"kernel-search.js is missing required runtime snippet: {snippet}")


def check_search_index_contract(search_root):
    """Verify that generated searchindex.js exposes the runtime keys we use."""

    search_index_path = search_root / "searchindex.js"
    search_index = read_text(search_index_path)

    if "Search.setIndex(" not in search_index:
        fail("searchindex.js does not initialize the shared Search index")

    for key in REQUIRED_SEARCH_INDEX_KEYS:
        if not re.search(rf'(?:"{re.escape(key)}"|{re.escape(key)})\s*:', search_index):
            fail(f"searchindex.js is missing required key: {key}")

    # Older supported Sphinx versions omit these keys, and the runtime falls
    # back to empty objects when they are absent.
    for key in OPTIONAL_SEARCH_INDEX_KEYS:
        if key in search_index and not re.search(
            rf'(?:"{re.escape(key)}"|{re.escape(key)})\s*:', search_index
        ):
            fail(f"searchindex.js contains malformed optional key: {key}")


def check_advanced_search_link(search_root):
    """Verify that a built non-search page exposes the advanced-search link."""

    for page in sorted(search_root.rglob("*.html")):
        if page.name in {"search.html", "genindex.html"}:
            continue

        contents = read_text(page)
        if "Advanced search" in contents and "?advanced=1" in contents:
            return

    fail("No generated documentation page exposes the Advanced search sidebar link")


def main():
    """Build docs and run the advanced-search smoke checks."""

    args = parse_args()
    build_dir, cleanup = prepare_build_dir(args)

    try:
        output_dir = run_build(args, build_dir)
        search_roots = find_search_roots(output_dir)
        for search_root in search_roots:
            check_search_html(search_root)
            check_search_assets(search_root)
            check_search_index_contract(search_root)
            check_advanced_search_link(search_root)
    except Exception:
        print(f"Preserving build directory for inspection: {build_dir}", file=sys.stderr)
        cleanup = False
        raise
    finally:
        if cleanup:
            shutil.rmtree(build_dir)

    print(
        "Advanced search smoke test passed "
        f"for SPHINXDIRS={' '.join(args.sphinxdirs)} "
        f"across {len(search_roots)} generated search trees."
    )
    if cleanup:
        print(f"Removed temporary build directory: {build_dir}")
    else:
        print(f"Build directory: {build_dir}")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.CalledProcessError) as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
