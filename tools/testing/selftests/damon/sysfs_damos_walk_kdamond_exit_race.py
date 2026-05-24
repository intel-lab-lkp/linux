#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Regression test for damos_walk() vs kdamond_fn() exit race.
#
# When kdamond_fn() finishes its main loop, it cancels remaining damos_walk()
# requests and unsets damon_ctx->kdamond. If damos_walk() is called right
# after cancellation but before kdamond pointer unset, it could wait forever
# for handling that never comes, causing a deadlock.
#
# This test verifies the fix by rapidly calling update_schemes_tried_regions
# while kdamond is naturally terminating (monitored process exits).
# Without the fix (commit 33c3f6c2b48c), this would hang indefinitely.

import os
import subprocess
import threading
import time
import _damon_sysfs

def call_update(kdamond, result):
    err = kdamond.update_schemes_tried_regions()
    result['err'] = err
    result['done'] = True

def main():
    proc = subprocess.Popen(['sleep', '0.3'])

    kdamonds = _damon_sysfs.Kdamonds([_damon_sysfs.Kdamond(
            contexts=[_damon_sysfs.DamonCtx(
                ops='vaddr',
                targets=[_damon_sysfs.DamonTarget(pid=proc.pid)],
                schemes=[_damon_sysfs.Damos(
                    action='stat',
                    access_pattern=_damon_sysfs.DamosAccessPattern(
                        nr_accesses=[0, 200]))]
                )]
            )])

    err = kdamonds.start()
    if err is not None:
        print('kdamond start failed: %s' % err)
        exit(1)

    # Wait for monitored process to die naturally
    proc.wait()

    # Rapidly call damos_walk() while kdamond is exiting
    # Use a thread with real timeout to detect kernel-level deadlock
    deadline = time.time() + 5
    while time.time() < deadline:
        result = {'done': False, 'err': None}
        t = threading.Thread(target=call_update,
                             args=(kdamonds.kdamonds[0], result))
        t.daemon = True
        t.start()
        t.join(timeout=5)

        if not result['done']:
            print('FAIL: update_schemes_tried_regions hung - '
                  'possible damos_walk/kdamond exit race deadlock')
            exit(1)

        if result['err'] is not None:
            # kdamond stopped cleanly - expected
            break

        # Check kdamond state via sysfs using dynamic path
        state_path = os.path.join(
                kdamonds.kdamonds[0].sysfs_dir(), 'state')
        try:
            with open(state_path) as f:
                if f.read().strip() == 'off':
                    break
        except OSError as e:
            print('failed to read kdamond state: %s' % e)
            exit(1)

    print('PASS: damos_walk() vs kdamond exit race not triggered')

if __name__ == '__main__':
    main()
