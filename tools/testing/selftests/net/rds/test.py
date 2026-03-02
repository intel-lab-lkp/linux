#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import argparse
import os
import signal
import subprocess
import sys
from pwd import getpwuid
from os import stat
import rds_basic
import rds_stress

# Allow utils module to be imported from different directory
this_dir = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(this_dir, "../"))
from lib.py.utils import ip

net0 = 'net0'
net1 = 'net1'

veth0 = 'veth0'
veth1 = 'veth1'

def increment_ports(addrs, inc):
    """Increment port numbers in the addrs list by inc.
       Use between tests to make the port numbers unique

    addrs: list of (ip, port) tuples
    inc: int
    """
    new_addrs = []

    for addr, port in addrs:
        new_addrs.append((addr, port + inc))

    return new_addrs

def signal_handler(sig, frame):
    print('Test timed out')
    sys.exit(1)

#Parse out command line arguments.  We take an optional
# timeout parameter and an optional log output folder
parser = argparse.ArgumentParser(description="init script args",
                  formatter_class=argparse.ArgumentDefaultsHelpFormatter)
parser.add_argument("-d", "--logdir", action="store",
                    help="directory to store logs", default="/tmp")
parser.add_argument("-b", "--rds_basic", action="store_true",
                    help="Run rds basic tests")
parser.add_argument("-s", "--rds_stress", action="store_true",
                    help="Run rds stress tests")
parser.add_argument('--timeout', help="timeout to terminate hung test",
                    type=int, default=0)
parser.add_argument('-l', '--loss', help="Simulate tcp packet loss",
                    type=int, default=0)
parser.add_argument('-c', '--corruption', help="Simulate tcp packet corruption",
                    type=int, default=0)
parser.add_argument('-u', '--duplicate', help="Simulate tcp packet duplication",
                    type=int, default=0)
args = parser.parse_args()
logdir=args.logdir
packet_loss=str(args.loss)+'%'
packet_corruption=str(args.corruption)+'%'
packet_duplicate=str(args.duplicate)+'%'

ip(f"netns add {net0}")
ip(f"netns add {net1}")
ip("link add type veth")

addrs = [
    # we technically don't need different port numbers, but this will
    # help identify traffic in the network analyzer
    ('10.0.0.1', 10000),
    ('10.0.0.2', 20000),
]

# move interfaces to separate namespaces so they can no longer be
# bound directly; this prevents rds from switching over from the tcp
# transport to the loop transport.
ip(f"link set {veth0} netns {net0} up")
ip(f"link set {veth1} netns {net1} up")



# add addresses
ip(f"-n {net0} addr add {addrs[0][0]}/32 dev {veth0}")
ip(f"-n {net1} addr add {addrs[1][0]}/32 dev {veth1}")

# add routes
ip(f"-n {net0} route add {addrs[1][0]}/32 dev {veth0}")
ip(f"-n {net1} route add {addrs[0][0]}/32 dev {veth1}")

# sanity check that our two interfaces/addresses are correctly set up
# and communicating by doing a single ping
ip(f"netns exec {net0} ping -c 1 {addrs[1][0]}")

# Start a packet capture on each network
for net in [net0, net1]:
    tcpdump_pid = os.fork()
    if tcpdump_pid == 0:
        pcap = logdir+'/'+net+'.pcap'
        subprocess.check_call(['touch', pcap])
        user = getpwuid(stat(pcap).st_uid).pw_name
        ip(f"netns exec {net} /usr/sbin/tcpdump -Z {user} -i any -w {pcap}")
        sys.exit(0)

# simulate packet loss, duplication and corruption
for net, iface in [(net0, veth0), (net1, veth1)]:
    ip(f"netns exec {net} /usr/sbin/tc qdisc add dev {iface} root netem  \
         corrupt {packet_corruption} loss {packet_loss} duplicate  \
         {packet_duplicate}")

# add a timeout
if args.timeout > 0:
    signal.alarm(args.timeout)
    signal.signal(signal.SIGALRM, signal_handler)

env = {
    'addrs': addrs,
    'netns': [net0, net1],
}

ret = 0
if args.rds_basic:
    ret = rds_basic.run_test(env)

if ret == 0 and args.rds_stress:
    env['addrs'] = increment_ports(env['addrs'], 1000)
    ret = rds_stress.run_test(env)

print("Stopping network packet captures")
subprocess.check_call(['killall', '-q', 'tcpdump'])

sys.exit(ret)
