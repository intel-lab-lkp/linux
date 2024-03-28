#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# map_holes.py <filename>
#
# Print the holes and data ranges in a file.

import errno
import os
import sys

def map_holes(fd):
    end = os.lseek(fd, 0, os.SEEK_END)
    offset = 0

    print('TYPE START END SIZE')

    while offset < end:
        contents = 'DATA'
        new_offset = os.lseek(fd, offset, os.SEEK_HOLE)
        if new_offset == offset:
            contents = 'HOLE'
            try:
              new_offset = os.lseek(fd, offset, os.SEEK_DATA)
            except OSError as err:
                if err.errno == errno.ENXIO:
                    new_offset = end
                else:
                    raise err
            assert new_offset != offset
        print(f'{contents} {offset} {new_offset} {new_offset - offset}')
        offset = new_offset

if __name__ == '__main__':
    with open(sys.argv[1], 'rb') as f:
        fd = f.fileno()
        map_holes(fd)
