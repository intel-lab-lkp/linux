#!/usr/bin/python
# SPDX-License-Identifier: GPL-2.0
# Determine the maximum size of branch stacks in a perf.data file.

import argparse
import sys

import os

script_dir = os.path.dirname(os.path.abspath(__file__))
python_dir = os.path.abspath(os.path.join(script_dir, "../../../python"))
sys.path.insert(0, python_dir)

import perf

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    args = ap.parse_args()

    bmax = 0

    def process_event(sample):
        nonlocal bmax
        try:
            brstack = sample.brstack
            if brstack:
                n = sum(1 for _ in brstack)
                if n > bmax:
                    bmax = n
        except AttributeError:
            pass

    try:
        session = perf.session(perf.data(args.input), sample=process_event)
        session.process_events()
        print("max brstack", bmax)
    except Exception as e:
        print(f"Error processing events: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
