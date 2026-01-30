#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0
import argparse
import re
import shutil
import subprocess
import sys
import os

script = os.path.relpath(__file__)

DESCRIPTION = f"""
For Intel CPUs, update the microcode revisions that determine
X86_BUG_OLD_MICROCODE.

This script is intended to be run in response to releases of the
official Intel microcode GitHub repository:
https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files.git

It takes the Intel microcode files as input and uses iucode-tool to
extract the revision information. It prints the output in the format
expected by intel-ucode-defs.h.

Usage:
    ./{script} /path/to/microcode/files > /path/to/intel-ucode-defs.h

Typically, someone at Intel would see a new public release, wait for at
least three months to ensure the update is stable, run this script to
refresh the intel-ucode-defs.h file, and send a patch upstream to update
the mainline and stable versions.

As always, there can be exceptions to the above process which should be
supported by appropriate justification.
"""

parser = argparse.ArgumentParser(description=DESCRIPTION,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('ucode_files', nargs='+', help='Path(s) to the microcode files')

args = parser.parse_args()

# Process the microcode files using iucode-tool
if shutil.which("iucode-tool") is None:
    print("Error: iucode-tool not found, please install it", file=sys.stderr)
    sys.exit(1)

cmd = ['iucode-tool', '--list-all' ]
cmd.extend(args.ucode_files)

process = subprocess.Popen(cmd, stdout=subprocess.PIPE, universal_newlines=True)
process.wait()
if process.returncode != 0:
    print("Error: iucode-tool ran into an error, exiting", file=sys.stderr)
    sys.exit(1)

# Functions to extract family, model, and stepping
def bits(val, bottom, top):
    mask = (1 << (top + 1 - bottom)) - 1
    mask = mask << bottom
    return (val & mask) >> bottom

def family(sig):
    if bits(sig, 8, 11) == 0xf:
        return bits(sig, 8, 11) + bits(sig, 20, 27)
    return bits(sig, 8, 11)

def model(sig):
    return bits(sig, 4, 7)  | (bits(sig, 16, 19) << 4)

def step(sig):
    return bits(sig, 0, 3)

# Parse the output of iucode-tool
ucode_list = []
for line in process.stdout:
    if line.find(" sig ") == -1:
        continue
    sig = re.search('sig (0x[0-9a-fA-F]+)', line).group(1)
    rev = re.search('rev (0x[0-9a-fA-F]+)', line).group(1)
    pfm = re.search('pf_mask (0x[0-9a-fA-F]+)', line).group(1)
    sig = int(sig, 16)
    rev = int(rev, 16)
    pfm = int(pfm, 16)
    debug_rev = bits(rev, 31, 31)
    if debug_rev != 0:
        print("Error: Debug ucode file found, exiting", file=sys.stderr)
        sys.exit(1)

    ucrev = {}
    ucrev['sig']       = sig
    ucrev['platforms'] = pfm
    ucrev['rev']       = rev
    ucode_list.append(ucrev)

# Remove duplicates, if any
ucode_dict = {}
for rev in ucode_list:
    ucode_key = (rev['sig'], rev['platforms'])
    existing = ucode_dict.get(ucode_key)
    if existing is not None:
        # If the existing one is newer, just move on:
        if existing['rev'] > rev['rev']:
            continue
    ucode_dict[ucode_key] = rev

# Prepare the microcode entries
ucode_entries = []
for ucode_key in ucode_dict:
    rev = ucode_dict[ucode_key]
    sig = rev['sig']
    ucode_entries.append({
        'family': family(sig),
        'model': model(sig),
        'steppings': 1 << step(sig),
        'platforms': rev['platforms'],
        'rev': rev['rev'],
        'sig': sig
    })

if not ucode_entries:
    print("Error: No valid microcode files found, exiting", file=sys.stderr)
    sys.exit(1)

# Sort and print the microcode entries
ucode_entries.sort(key=lambda x: (x['family'], x['model'], x['steppings'], x['platforms']))
for entry in ucode_entries:
    print("{ .flags = X86_CPU_ID_FLAG_ENTRY_VALID, .vendor = X86_VENDOR_INTEL, .family = 0x%x,  .model = 0x%02x, .steppings = 0x%04x, .platform_mask = 0x%02x, .driver_data = 0x%x }," %
          (entry['family'], entry['model'], entry['steppings'], entry['platforms'], entry['rev']))
