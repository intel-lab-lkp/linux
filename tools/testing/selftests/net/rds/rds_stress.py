#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import subprocess
import time

def run_test(env):
    """Run RDS stress selftest.

    env is a dictionary provided by test.py and is expected to contain:
      - 'addrs':   list of (ip, port) tuples matching the sockets
      - 'netns':   list of network namespace names (for sysctl exercises)
    """
    addrs = env['addrs']      # [('10.0.0.1', 10000), ('10.0.0.2', 20000)]
    netns_list = env['netns'] # ['net0', 'net1']

    a0, a1 = addrs
    recv_addr = a0[0]
    send_addr = a1[0]
    port = a0[1]

    nr_tasks = 1  # max child tasks created
    q_depth = 1   # max outstanding messages
    duration = 60 # duration of test in seconds

    # server side
    p0 = subprocess.Popen([
        'ip', 'netns', 'exec', netns_list[0],
        'rds-stress',
        '-r', str(recv_addr),
        '-p', str(port),
        '-t', str(nr_tasks),
        '-d', str(q_depth),
        '-T', str(duration+5) # add some extra time to let the client finish
    ])

    time.sleep(1) # delay to allow server time to come up

    # client side
    p1 = subprocess.Popen([
        'ip', 'netns', 'exec', netns_list[1],
        'rds-stress',
        '-r', str(send_addr), '-s', str(recv_addr),
        '-p', str(port),
        '-t', str(nr_tasks),
        '-d', str(q_depth),
        '-T', str(duration)
    ])

    rc1 = p1.wait() # wait for client
    rc0 = p0.wait() # then wait for the server

    if rc0 != 0 or rc1 != 0:
        print(f"rds-stress failed: server={rc0} client={rc1}")
        return 1

    print("Success")
    return 0
