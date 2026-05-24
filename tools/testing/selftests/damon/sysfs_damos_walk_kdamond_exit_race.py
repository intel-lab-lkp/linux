#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Regression test for damos_walk() vs kdamond_fn() exit race.
import subprocess
import time
import _damon_sysfs
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
    proc.wait()
    completed = False
    timeout = time.time() + 5
    while time.time() < timeout:
        err = kdamonds.kdamonds[0].update_schemes_tried_regions()
        if err is not None:
            completed = True
            break
        try:
            with open('/sys/kernel/mm/damon/admin/kdamonds/0/state') as f:
                state = f.read().strip()
            if state == 'off':
                completed = True
                break
        except:
            completed = True
            break
    if not completed:
        print('FAIL: test timed out - possible damos_walk/kdamond exit race deadlock')
        kdamonds.stop()
        exit(1)
    print('PASS: damos_walk() vs kdamond exit race not triggered')
if __name__ == '__main__':
    main()
