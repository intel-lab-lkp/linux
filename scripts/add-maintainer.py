#! /usr/bin/env python3

import argparse
import logging
import os
import sys
import subprocess
import re

def gather_maintainers_of_file(patch_file):
    all_entities_of_patch = dict()

    # Run get_maintainer.pl on patch file
    logging.info("GET: Patch: {}".format(os.path.basename(patch_file)))
    cmd = ['scripts/get_maintainer.pl']
    cmd.extend([patch_file])

    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, check=True)
    except:
        sys.exit(1)

    logging.debug("\n{}".format(p.stdout.decode()))

    entries = p.stdout.decode().splitlines()

    maintainers = []
    lists = []
    others = []

    for entry in entries:
        entity = entry.split('(')[0].strip()
        if any(role in entry for role in ["maintainer", "reviewer"]):
            maintainers.append(entity)
        elif "list" in entry:
            lists.append(entity)
        else:
            others.append(entity)

    all_entities_of_patch["maintainers"] = set(maintainers)
    all_entities_of_patch["lists"] = set(lists)
    all_entities_of_patch["others"] = set(others)

    return all_entities_of_patch

def find_pattern_in_lines(pattern, lines):
    index = 0
    for line in lines:
        if re.search(pattern, line):
            break;
        index = index + 1

    if index == len(lines):
        logging.error("Couldn't find pattern {} in patch".format(pattern))
        sys.exit(1)

    return index

def add_maintainers_to_file(patch_file, entities_per_file, all_entities_union):
    logging.info("ADD: Patch: {}".format(os.path.basename(patch_file)))

    # For each patch:
    # - Add all lists from all patches in series as Cc:
    # - Add all others from all patches in series as Cc:
    # - Add only maintainers of that patch as To:
    # - Add maintainers of other patches in series as Cc:

    lists = list(all_entities_union["all_lists"])
    others = list(all_entities_union["all_others"])
    file_maintainers = all_entities_union["all_maintainers"].intersection(entities_per_file[os.path.basename(patch_file)].get("maintainers"))
    other_maintainers = all_entities_union["all_maintainers"].difference(entities_per_file[os.path.basename(patch_file)].get("maintainers"))

    # Specify email headers appropriately
    cc_lists        = ["Cc: " + l for l in lists]
    cc_others       = ["Cc: " + o for o in others]
    to_maintainers  = ["To: " + m for m in file_maintainers]
    cc_maintainers  = ["Cc: " + om for om in other_maintainers]
    logging.debug("Cc Lists:\n{}".format('\n'.join(cc_lists)))
    logging.debug("Cc Others:\n{}".format('\n'.join(cc_others)))
    logging.debug("Cc Maintainers:\n{}".format('\n'.join(cc_maintainers) or None))
    logging.debug("To Maintainers:\n{}\n".format('\n'.join(to_maintainers) or None))

    # Edit patch file in place to add maintainers
    with open(patch_file, "r") as pf:
        lines = pf.readlines()

    # Get the index of the first "From: <email address>" line in patch
    from_line = find_pattern_in_lines("^(From: )(.*)<(.*)@(.*)>", lines)

    # Insert our To: and Cc: headers after it.
    next_line_after_from = from_line + 1

    for l in cc_lists:
        lines.insert(next_line_after_from, l + "\n")
    for o in cc_others:
        lines.insert(next_line_after_from, o + "\n")
    for om in cc_maintainers:
        lines.insert(next_line_after_from, om + "\n")
    for m in to_maintainers:
        lines.insert(next_line_after_from, m + "\n")

    with open(patch_file, "w") as pf:
        pf.writelines(lines)

def add_maintainers(patch_files):
    entities_per_file = dict()

    for patch in patch_files:
        entities_per_file[os.path.basename(patch)] = gather_maintainers_of_file(patch)

    all_entities_union = {"all_maintainers": set(), "all_lists": set(), "all_others": set()}
    for patch in patch_files:
        all_entities_union["all_maintainers"] = all_entities_union["all_maintainers"].union(entities_per_file[os.path.basename(patch)].get("maintainers"))
        all_entities_union["all_lists"] = all_entities_union["all_lists"].union(entities_per_file[os.path.basename(patch)].get("lists"))
        all_entities_union["all_others"] = all_entities_union["all_others"].union(entities_per_file[os.path.basename(patch)].get("others"))

    for patch in patch_files:
        add_maintainers_to_file(patch, entities_per_file, all_entities_union)

    logging.info("Maintainers added to all patch files successfully")

def remove_to_cc_from_header(patch_files):
    for patch in patch_files:
        logging.info("UNDO: Patch: {}".format(os.path.basename(patch)))
        with open(patch, "r") as pf:
            lines = pf.readlines()

        # Get the index of the first "From: <email address>" line in patch
        from_line = find_pattern_in_lines("^(From: )(.*)<(.*)@(.*)>", lines)

        # Get the index of the first "Date: " line in patch
        date_line = find_pattern_in_lines("^(Date: )", lines)

        # Delete everything in between From: and Date:
        # These are the lines that this script adds - any To: or Cc: anywhere
        # else in the patch will not be removed.
        del lines[(from_line + 1):date_line]

        with open(patch, "w") as pf:
            pf.writelines(lines)

    logging.info("Maintainers removed from all patch files successfully")

def main():
    parser = argparse.ArgumentParser(description='Add the respective maintainers and mailing lists to patch files')
    parser.add_argument('patches', nargs='+', help="One or more patch files")
    parser.add_argument('-v', '--verbosity', choices=['debug', 'info', 'error'], default='error', help="Verbosity level of script output")
    parser.add_argument('-u', '--undo', action='store_true', help="Remove maintainers added by this script from patch(es)")
    args = parser.parse_args()

    logging.basicConfig(level=args.verbosity.upper(), format='%(levelname)s: %(message)s')

    for patch in args.patches:
        if not os.path.isfile(patch):
            logging.error("File does not exist: {}".format(patch))
            sys.exit(1)

    if args.undo:
        remove_to_cc_from_header(args.patches)
    else:
        add_maintainers(args.patches)

if __name__ == "__main__":
    main()
