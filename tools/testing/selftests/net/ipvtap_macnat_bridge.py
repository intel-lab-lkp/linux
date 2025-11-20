#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Script to bridge ipvtap and tap,
needed to simulate behaviour of virtual machine using ipvtap.

ipvtap in macnat mode cannot have IP address.
Due to limitations of ipvtap, it also cannot be plugged
into bridge.
Use this script to connect ipvtap and tap and assing IP to tap.
"""

import socket
import os
import select
import sys
import signal
import fcntl
import struct
import subprocess

# Linux TUN/TAP constants
TUNSETIFF = 0x400454ca
IFF_TUN = 0x0001
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000

ns_name = "non-initialized"

class TapBridge:
    """Simple class to bridge ipvtap and tap interfaces"""
    def __init__(self, tap, ipvtap, buffer_size=65536):
        self.tap_name = tap
        self.ipvtap_name = ipvtap
        self.buffer_size = buffer_size
        self.running = False

    def open_ipvtap_sock(self, tap_name):
        """Open a IPVTAP interface using raw socket"""
        try:
            sock = socket.socket(socket.AF_PACKET,
                                 socket.SOCK_RAW,
                                 socket.ntohs(0x0003))
            sock.bind((tap_name, 0))
            sock.setblocking(False)
            print(f"Connected to IPVTAP interface: {tap_name}")
            return sock

        except (OSError, IOError) as e:
            print(f"Error opening IPVTAP interface {tap_name}: {e}")
            return None

    def create_tap_interface(self, tap_name):
        """Create and configure a TAP interface using /dev/net/tun"""
        try:
            # Open the tun device
            tun_fd = os.open('/dev/net/tun', os.O_RDWR)
            if tun_fd < 0:
                raise OSError("Failed to open /dev/net/tun (err: {os.errno})")

            # Prepare the ifr structure
            tap_name_bytes = tap_name.encode('utf-8')
            ifr = struct.pack('16sH', tap_name_bytes, IFF_TAP | IFF_NO_PI)

            # Set the interface name and flags
            result = fcntl.ioctl(tun_fd, TUNSETIFF, ifr)

            # Get the actual interface name that was set
            unpacked = struct.unpack('16sH', result)
            actual_name = unpacked[0].split(b'\x00')[0].decode()
            print(f"Created TAP interface: {actual_name}")

            return tun_fd

        except (OSError, IOError) as e:
            print(f"Error creating TAP interface {tap_name}: {e}")
            return None

    def forward_data(self, from_fd, to_fd, description):
        """Forward data from one file descriptor to another"""
        try:
            data = os.read(from_fd, self.buffer_size)
            if data:
                os.write(to_fd, data)
                return True
            return False

        except BlockingIOError:
            return True
        except (OSError, IOError) as e:
            print(f"Error forwarding data {description}: {e}")
            return False

    def run(self):
        """Main bridge loop"""
        # Create TAP interfaces
        tap1_fd = self.create_tap_interface(self.tap_name)

        sock = self.open_ipvtap_sock(self.ipvtap_name)
        tap2_fd = sock.fileno()

        if tap1_fd is None or tap2_fd is None:
            print("Failed to create TAP interfaces")
            return

        print("Press Ctrl+C to stop\n")

        self.running = True
        stats = {'tap1_to_tap2': 0, 'tap2_to_tap1': 0}
        while self.running:
            try:
                # Use select to monitor both file descriptors
                readable, _, _ = select.select([tap1_fd, tap2_fd], [], [], 1.0)

                for fd in readable:
                    if fd == tap1_fd:
                        descr = f"from {self.tap_name} to {self.ipvtap_name}"
                        if self.forward_data(tap1_fd, tap2_fd, descr):
                            stats['tap1_to_tap2'] += 1
                        else:
                            self.running = False
                    elif fd == tap2_fd:
                        descr = f"from {self.ipvtap_name} to {self.tap_name}"
                        if self.forward_data(tap2_fd, tap1_fd, descr):
                            stats['tap2_to_tap1'] += 1
                        else:
                            self.running = False

            except KeyboardInterrupt:
                print("\nShutting down...")
                self.running = False
            except (OSError, IOError) as e:
                print(f"Error in main loop: {e}")
                self.running = False

        # Cleanup
        os.close(tap1_fd)
        os.close(tap2_fd)
        print(f"Bridge stopped in {ns_name}. Stats: {stats}")


def signal_handler(_sig, _frame):
    """SIGINT handler for macnat bridge"""
    print(f'\nReceived interrupt signal, shutting down bridge in {ns_name}')
    sys.exit(0)


if __name__ == "__main__":
    ns_name = subprocess.getoutput("ip netns identify") or "default"

    signal.signal(signal.SIGINT, signal_handler)

    # Check if running as root
    if os.geteuid() != 0:
        print("ERROR: This script must be run as root!")
        sys.exit(1)

    if len(sys.argv) != 3:
        print("Usage: tap_bridge.py tap_name ipvtap_name")
        sys.exit(1)

    TAP = sys.argv[1]
    IPVTAP = sys.argv[2]

    print(f"Starting TAP bridge between {TAP} and {IPVTAP} in {ns_name}")
    bridge = TapBridge(TAP, IPVTAP)
    bridge.run()
