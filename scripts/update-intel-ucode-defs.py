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

The script takes the Intel microcode files as input and uses the
iucode-tool to extract the revision information. It formats the output
and writes it to intel-ucode-defs.h which holds the minimum expected
revision for each family-model-stepping.

A typical usage is to get the desired release of the Intel Microcode
Update Package at:
https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files.git

And run:
    ./{script} -u /path/to/microcode/files

Note: The microcode revisions are usually updated shortly after a new
microcode package is released, allowing a reasonable time for systems to
get the update.
"""

# Get the path to the microcode header defines
script_path = os.path.abspath(sys.argv[0])
linux_root = os.path.dirname(os.path.dirname(script_path))
ucode_hdr = os.path.join(linux_root, 'arch/x86/kernel/cpu/microcode/intel-ucode-defs.h')

parser = argparse.ArgumentParser(description=DESCRIPTION,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('-u','--ucode_path', required=True,
                    help='Path to the microcode files')
parser.add_argument('-o','--output', dest='header', default=ucode_hdr,
                    help='The microcode header file to be updated (default: intel-ucode-defs.h)')

args = parser.parse_args()

# Process the microcode files using iucode-tool
if shutil.which("iucode-tool") is None:
    print("Error: iucode-tool not found, please install it")
    sys.exit(1)

cmd = ['iucode-tool', '--list-all' ]
cmd.append(args.ucode_path)

process = subprocess.Popen(cmd, stdout=subprocess.PIPE, universal_newlines=True)
process.wait()
if process.returncode != 0:
    print("Error: iucode-tool ran into an error, exiting")
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
siglist = []
for line in process.stdout:
    if line.find(" sig ") == -1:
        continue
    sig = re.search('sig (0x[0-9a-fA-F]+)', line).group(1)
    rev = re.search('rev (0x[0-9a-fA-F]+)', line).group(1)
    sig = int(sig, 16)
    rev = int(rev, 16)
    debug_rev = bits(rev, 31, 31)
    if debug_rev != 0:
        print("Error: Debug ucode file found, exiting")
        sys.exit(1);

    sigrev = {}
    sigrev['sig'] = sig
    sigrev['rev'] = rev
    siglist = siglist + [ sigrev ]

# Remove duplicates, if any
sigdict = {}
for sr in siglist:
    existing = sigdict.get(sr['sig'])
    if existing != None:
        # If the existing one is newer, just move on:
        if existing['rev'] > sr['rev']:
            continue
    sigdict[sr['sig']] = sr

# Prepare and sort the microcode entries
ucode_entries = []
for sig in sigdict:
    rev = sigdict[sig]
    ucode_entries.append({
        'family': family(sig),
        'model': model(sig),
        'steppings': 1 << step(sig),
        'rev': rev['rev'],
        'sig': sig
    })

if not ucode_entries:
    print("Error: No valid microcode files found, exiting")
    sys.exit(1)

ucode_entries.sort(key=lambda x: (x['family'], x['model'], x['steppings']))

# Update the microcode header file
header_path = args.header
if not os.path.exists(header_path):
    print(f"Error: '{header_path}' does not exist, use the '-o' option to specify a file to update")
    sys.exit(1)

with open(header_path, "w") as f:
    for entry in ucode_entries:
        f.write("{ .flags = X86_CPU_ID_FLAG_ENTRY_VALID, .vendor = X86_VENDOR_INTEL, .family = 0x%x,  .model = 0x%02x, .steppings = 0x%04x, .driver_data = 0x%x },\n" %
                (entry['family'], entry['model'], entry['steppings'], entry['rev']))
