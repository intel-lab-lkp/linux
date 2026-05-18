#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Test the update_schemes_quota_goals sysfs command.

Start DAMON with a scheme that has a some_mem_psi_us quota goal.  Write a
physically impossible dummy value to the goal's current_value sysfs file.
Wait for a while, ensure the dummy value is not overwritten asynchronously,
then write 'update_schemes_quota_goals' to the state file and verify that
the dummy value is overwritten by the kernel.
"""

import os
import time

import _damon_sysfs


def main():
    goal = _damon_sysfs.DamosQuotaGoal(
            metric=_damon_sysfs.qgoal_metric_some_mem_psi_us,
            target_value=1000)
    kdamonds = _damon_sysfs.Kdamonds([_damon_sysfs.Kdamond(
            contexts=[_damon_sysfs.DamonCtx(
                ops='paddr',
                schemes=[_damon_sysfs.Damos(
                    action='stat',
                    quota=_damon_sysfs.DamosQuota(
                        goals=[goal], reset_interval_ms=100),
                    )]  # schemes
                )]  # contexts
            )])  # kdamonds

    err = kdamonds.start()
    if err is not None:
        print('kdamond start failed: %s' % err)
        exit(1)

    # Write a dummy value to current_value to ensure the command actually
    # overwrites it. We use 2x the quota reset interval in microseconds,
    # which is a physically impossible value for the kernel to measure.
    impossible_value = goal.quota.reset_interval_ms * 2000
    err = _damon_sysfs.write_file(
            os.path.join(goal.sysfs_dir(), 'current_value'),
            '%d' % impossible_value)
    if err is not None:
        kdamonds.stop()
        print('Writing dummy current_value failed: %s' % err)
        exit(1)

    # wait a couple of aggregation intervals so that the kernel has a chance
    # to compute the first current_value measurement
    time.sleep(0.5)

    content, err = _damon_sysfs.read_file(
            os.path.join(goal.sysfs_dir(), 'current_value'))
    if err is not None:
        kdamonds.stop()
        print('Reading current_value before update failed: %s' % err)
        exit(1)
    if int(content) != impossible_value:
        kdamonds.stop()
        print('current_value changed before update (%s)' % content)
        exit(1)

    err = kdamonds.kdamonds[0].update_schemes_quota_goals()
    if err is not None:
        kdamonds.stop()
        print('update_schemes_quota_goals failed: %s' % err)
        exit(1)

    # current_value must be updated and different from our dummy value
    if goal.current_value is None or goal.current_value == impossible_value:
        kdamonds.stop()
        print('update_schemes_quota_goals failed to update current_value')
        exit(1)

    print('current_value after update_schemes_quota_goals: %d' %
          goal.current_value)

    kdamonds.stop()


if __name__ == '__main__':
    main()
