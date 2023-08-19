#!/usr/bin/env python3
# SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)

from argparse import ArgumentParser
from argparse import FileType
import os
import sys
import tpm2

def main():
    parser = ArgumentParser(description='Parse a TPM error code')
    parser.add_argument('rc', type=(lambda x: int(x, 0)))
    args = parser.parse_args()
    print(str(tpm2.ProtocolError(None, args.rc)))

if __name__ == '__main__':
    main()
