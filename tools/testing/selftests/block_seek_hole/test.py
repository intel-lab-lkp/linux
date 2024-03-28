#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# test.py
#
# Test SEEK_HOLE/SEEK_DATA support in block drivers

import os
import subprocess
import sys
from contextlib import contextmanager

KB = 1024
MB = 1024 * KB

def run(args):
    try:
        cmd = subprocess.run(args, check=True, capture_output=True)
    except subprocess.CalledProcessError as e:
        print(e)
        print(e.stderr.decode('utf-8').strip())
        sys.exit(1)
    return cmd

@contextmanager
def test_file(layout_fn, prefix='test'):
    '''A context manager that creates a test file and produces its path'''
    path = f'{prefix}-{os.getpid()}'
    with open(path, 'w+b') as f:
        layout_fn(f)

    try:
        yield path
    finally:
        os.unlink(path)

@contextmanager
def loop_device(file_path):
    '''A context manager that attaches a loop device for a given file and produces the path of the loop device'''
    cmd = run(['losetup', '--show', '-f', file_path])
    loop_path = os.fsdecode(cmd.stdout.strip())

    try:
        yield loop_path
    finally:
        run(['losetup', '-d', loop_path])

def test(layout, dev_context_manager):
    with test_file(layout) as file_path, dev_context_manager(file_path) as dev_path:
        cmd = run(['./map_holes.py', file_path])
        file_output = cmd.stdout.decode('utf-8').strip()

        cmd = run(['./map_holes.py', dev_path])
        dev_output = cmd.stdout.decode('utf-8').strip()

        if file_output != dev_output:
            print(f'FAIL {dev_context_manager.__name__} {layout.__name__}')
            print('File output:')
            print(file_output)
            print('Does not match device output:')
            print(dev_output)
            sys.exit(1)

def test_all(layouts, dev_context_managers):
    for dev_context_manager in dev_context_managers:
        for layout in layouts:
            test(layout, dev_context_manager)

# Different data layouts to test

def data_at_beginning_and_end(f):
    f.write(b'A' * 4 * KB)
    f.seek(256 * MB)

    f.write(b'B' * 64 * KB)

    f.seek(1024 * MB - KB)
    f.write(b'C' * KB)

def holes_at_beginning_and_end(f):
    f.seek(128 * MB)
    f.write(b'A' * 4 * KB)

    f.seek(512 * MB)
    f.write(b'B' * 64 * KB)

    f.truncate(1024 * MB)

def no_holes(f):
    # Just 1 MB so test file generation is quick
    mb = b'A' * MB
    f.write(mb)

def empty_file(f):
    f.truncate(1024 * MB)

if __name__ == '__main__':
    layouts = [data_at_beginning_and_end,
               holes_at_beginning_and_end,
               no_holes,
               empty_file]
    dev_context_managers = [loop_device]
    test_all(layouts, dev_context_managers)
