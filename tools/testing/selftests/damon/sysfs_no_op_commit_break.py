#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import json
import os
import subprocess
import sys

import _damon_sysfs

def dump_damon_status_dict(pid):
    try:
        subprocess.check_output(['which', 'drgn'], stderr=subprocess.DEVNULL)
    except:
        return None, 'drgn not found'
    file_dir = os.path.dirname(os.path.abspath(__file__))
    dump_script = os.path.join(file_dir, 'drgn_dump_damon_status.py')
    rc = subprocess.call(['drgn', dump_script, pid, 'damon_dump_output'],
        stderr=subprocess.DEVNULL)

    if rc != 0:
        return None, f'drgn fail : return code({rc})'
    try:
        with open('damon_dump_output', 'r') as f:
            return json.load(f), None
    except Exception as e:
        return None, 'json.load fail (%s)' % e

def main():
    # We can extend this test to change only the Given and reuse the When/Then.

    # Given : setup filter with allow = True
    proc = subprocess.Popen(['sleep', '10'])
    kdamonds = _damon_sysfs.Kdamonds(
        [_damon_sysfs.Kdamond(
            contexts=[_damon_sysfs.DamonCtx(
                targets=[_damon_sysfs.DamonTarget(pid=proc.pid)],
                schemes=[_damon_sysfs.Damos(
                    ops_filters=[
                        _damon_sysfs.DamosFilter(type_='anon', matching=True, allow=True)
                    ]
                )],
            )])]
    )

    err = kdamonds.start()
    if err is not None:
        print('kdamond start failed: %s' % err)
        exit(1)

    before_commit_status, err = dump_damon_status_dict(kdamonds.kdamonds[0].pid)
    if err is not None:
        print(err)
        exit(1)

    # When : No-op commit, it should not break anything
    kdamonds.kdamonds[0].commit()

    # Then : Check value with drgn
    after_commit_status, err = dump_damon_status_dict(kdamonds.kdamonds[0].pid)
    if err is not None:
        print(err)
        exit(1)

    before_commit_dump = json.dumps(before_commit_status, sort_keys=True, indent=2)
    after_commit_dump = json.dumps(after_commit_status, sort_keys=True, indent=2)
    if before_commit_dump != after_commit_dump:
        print("[TEST FAILED] : no-op commit break damon status")
        print("===== before_commit =====")
        print(before_commit_dump)
        print("===== after_commit =====")
        print(after_commit_dump)
        exit(1)

    kdamonds.stop()

if __name__ == '__main__':
    main()
