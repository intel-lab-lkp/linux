#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
This script helps track the translation status of the documentation
in different locales, e.g., zh_CN. More specially, it uses `git log`
command to find the latest English commit from the translation commit
(order by author date) and the latest English commits from HEAD. If
differences occur, report the file and commits that need to be updated.
"""

import os
from argparse import ArgumentParser, BooleanOptionalAction
from datetime import datetime

flag_p_c = False
flag_p_uf = False
flag_debug = False


def dprint(*args, **kwargs):
    """Print debug information if the debug flag is set"""
    if flag_debug:
        print("[DEBUG] ", end="")
        print(*args, **kwargs)


def get_origin_path(file_path):
    """Get the origin path from the translation path"""
    paths = file_path.split("/")
    tidx = paths.index("translations")
    opaths = paths[:tidx]
    opaths += paths[tidx + 2 :]
    return "/".join(opaths)


def get_latest_commit_from(file_path, commit):
    """Get the latest commit from the specified commit for the specified file"""
    command = f"git log --pretty=format:%H%n%aD%n%cD%n%n%B {commit} -1 -- {file_path}"
    dprint(command)
    pipe = os.popen(command)
    result = pipe.read()
    result = result.split("\n")
    if len(result) <= 1:
        return None

    dprint(f"Result: {result[0]}")

    return {
        "hash": result[0],
        "author_date": datetime.strptime(result[1], "%a, %d %b %Y %H:%M:%S %z"),
        "commit_date": datetime.strptime(result[2], "%a, %d %b %Y %H:%M:%S %z"),
        "message": result[4:],
    }


def get_origin_from_trans(origin_path, t_from_head):
    """Get the latest origin commit from the translation commit"""
    o_from_t = get_latest_commit_from(origin_path, t_from_head["hash"])
    while o_from_t is not None and o_from_t["author_date"] > t_from_head["author_date"]:
        o_from_t = get_latest_commit_from(origin_path, o_from_t["hash"] + "^")
    if o_from_t is not None:
        dprint(f"tracked origin commit id: {o_from_t['hash']}")
    return o_from_t


def get_commits_count_between(opath, commit1, commit2):
    """Get the commits count between two commits for the specified file"""
    command = f"git log --pretty=format:%H {commit1}...{commit2} -- {opath}"
    dprint(command)
    pipe = os.popen(command)
    result = pipe.read().split("\n")
    # filter out empty lines
    result = list(filter(lambda x: x != "", result))
    return result


def pretty_output(commit):
    """Pretty print the commit message"""
    command = f"git log --pretty='format:%h (\"%s\")' -1 {commit}"
    dprint(command)
    pipe = os.popen(command)
    return pipe.read()


def check_per_file(file_path):
    """Check the translation status for the specified file"""
    opath = get_origin_path(file_path)

    if not os.path.isfile(opath):
        dprint(f"Error: Cannot find the origin path for {file_path}")
        return

    o_from_head = get_latest_commit_from(opath, "HEAD")
    t_from_head = get_latest_commit_from(file_path, "HEAD")

    if o_from_head is None or t_from_head is None:
        print(f"Error: Cannot find the latest commit for {file_path}")
        return

    o_from_t = get_origin_from_trans(opath, t_from_head)

    if o_from_t is None:
        print(f"Error: Cannot find the latest origin commit for {file_path}")
        return

    if o_from_head["hash"] == o_from_t["hash"]:
        if flag_p_uf:
            print(f"No update needed for {file_path}")
    else:
        print(f"{file_path}", end="\t")
        commits = get_commits_count_between(
            opath, o_from_t["hash"], o_from_head["hash"]
        )
        print(f"({len(commits)} commits)")
        if flag_p_c:
            for commit in commits:
                msg = pretty_output(commit)
                if "Merge tag" not in msg:
                    print(f"commit {msg}")


def main():
    """Main function of the script"""
    script_path = os.path.dirname(os.path.abspath(__file__))
    linux_path = os.path.join(script_path, "..")

    parser = ArgumentParser(description="Check the translation update")
    parser.add_argument(
        "-l",
        "--locale",
        help="Locale to check when files are not specified",
    )
    parser.add_argument(
        "--print-commits",
        action=BooleanOptionalAction,
        default=True,
        help="Print commits between the origin and the translation",
    )

    parser.add_argument(
        "--print-updated-files",
        action=BooleanOptionalAction,
        default=False,
        help="Print files that do no need to be updated",
    )

    parser.add_argument(
        "--debug",
        action=BooleanOptionalAction,
        help="Print debug information",
        default=False,
    )

    parser.add_argument(
        "files", nargs="*", help="Files to check, if not specified, check all files"
    )
    args = parser.parse_args()

    global flag_p_c, flag_p_uf, flag_debug
    flag_p_c = args.print_commits
    flag_p_uf = args.print_updated_files
    flag_debug = args.debug

    # get files related to linux path
    files = args.files
    if len(files) == 0:
        if args.locale is not None:
            files = (
                os.popen(
                    f"find {linux_path}/Documentation/translations/{args.locale} -type f"
                )
                .read()
                .split("\n")
            )
        else:
            files = (
                os.popen(
                    f"find {linux_path}/Documentation/translations -type f"
                )
                .read()
                .split("\n")
            )

    files = list(filter(lambda x: x != "", files))
    files = list(map(lambda x: os.path.relpath(os.path.abspath(x), linux_path), files))

    # cd to linux root directory
    os.chdir(linux_path)

    for file in files:
        check_per_file(file)


if __name__ == "__main__":
    main()
