#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
TTM EFI Variable Configuration Utility

This script allows users to read, write, and delete the TTMPageLimit EFI
variable, which controls the GPU memory page limit for the TTM subsystem.

Usage:
    sudo python3 ttm_efi_config.py --read
    sudo python3 ttm_efi_config.py --set 2G
    sudo python3 ttm_efi_config.py --set-pages 524288
    sudo python3 ttm_efi_config.py --delete
"""

import argparse
import fcntl
import os
import struct
import sys

# EFI variable configuration
GUID = "8be4df61-93ca-11ec-b909-0800200c9a66"
VAR_NAME = "TTMPageLimit"
EFIVARS_PATH = "/sys/firmware/efi/efivars"
VAR_FILE = f"{EFIVARS_PATH}/{VAR_NAME}-{GUID}"

# Standard EFI variable attributes
# NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS
EFI_ATTRS = 0x07

# System page size (typically 4KB)
PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")

# Linux ioctl constant for getting/setting file attributes
# From linux/fs.h
FS_IOC_GETFLAGS = 0x80086601
FS_IOC_SETFLAGS = 0x40086602
FS_IMMUTABLE_FL = 0x00000010


def check_root():
    """Verify the script is running as root."""
    if os.geteuid() != 0:
        print("Error: This script must be run as root", file=sys.stderr)
        sys.exit(1)


def check_efi_available():
    """Check if EFI variables filesystem is available."""
    if not os.path.exists(EFIVARS_PATH):
        print(f"Error: EFI variables filesystem not found at {EFIVARS_PATH}",
              file=sys.stderr)
        print("Ensure your system booted with EFI and the efivarfs is mounted.",
              file=sys.stderr)
        sys.exit(1)


def remove_immutable(filepath):
    """
    Remove the immutable attribute from an EFI variable file.

    Args:
        filepath: Path to the EFI variable file

    Returns:
        True if successful, False otherwise
    """
    try:
        fd = os.open(filepath, os.O_RDONLY)

        flags = fcntl.ioctl(fd, FS_IOC_GETFLAGS, struct.pack('i', 0))
        flags_value = struct.unpack('i', flags)[0]

        if flags_value & FS_IMMUTABLE_FL:
            new_flags = flags_value & ~FS_IMMUTABLE_FL
            fcntl.ioctl(fd, FS_IOC_SETFLAGS, struct.pack('i', new_flags))

        os.close(fd)
        return True
    except Exception as e:
        print(f"Warning: Could not remove immutable attribute: {e}", file=sys.stderr)
        return False


def parse_size(size_str):
    """
    Parse a size string into page count.

    Args:
        size_str: String like "2G", "512M", or raw page count

    Returns:
        Number of pages (integer)
    """
    size_str = size_str.strip().upper()

    multipliers = {
        'K': 1024,
        'M': 1024 * 1024,
        'G': 1024 * 1024 * 1024,
        'KB': 1024,
        'MB': 1024 * 1024,
        'GB': 1024 * 1024 * 1024,
    }

    for suffix, multiplier in multipliers.items():
        if size_str.endswith(suffix):
            try:
                value = float(size_str[:-len(suffix)])
                bytes_total = int(value * multiplier)
                return bytes_total // PAGE_SIZE
            except ValueError:
                print(f"Error: Invalid size format: {size_str}", file=sys.stderr)
                sys.exit(1)

    try:
        return int(size_str)
    except ValueError:
        print(f"Error: Invalid page count: {size_str}", file=sys.stderr)
        sys.exit(1)


def format_pages(pages):
    """
    Format page count as human-readable size.

    Args:
        pages: Number of pages

    Returns:
        Formatted string (e.g., "2.0 GB (524288 pages)")
    """
    bytes_total = pages * PAGE_SIZE

    if bytes_total >= 1024 * 1024 * 1024:
        size_gb = bytes_total / (1024 * 1024 * 1024)
        return f"{size_gb:.1f} GB ({pages} pages)"
    elif bytes_total >= 1024 * 1024:
        size_mb = bytes_total / (1024 * 1024)
        return f"{size_mb:.1f} MB ({pages} pages)"
    elif bytes_total >= 1024:
        size_kb = bytes_total / 1024
        return f"{size_kb:.1f} KB ({pages} pages)"
    else:
        return f"{bytes_total} bytes ({pages} pages)"


def read_efi_variable():
    """Read the TTMPageLimit EFI variable."""
    if not os.path.exists(VAR_FILE):
        print(f"TTMPageLimit EFI variable is not set")
        return

    try:
        with open(VAR_FILE, 'rb') as f:
            data = f.read()

        # EFI variable format: 4-byte attributes + data
        if len(data) < 4:
            print("Error: Invalid EFI variable (too short)", file=sys.stderr)
            sys.exit(1)

        attrs = struct.unpack('<I', data[:4])[0]
        value_data = data[4:]

        if len(value_data) != 8:
            print(f"Warning: Variable size is {len(value_data)} bytes (expected 8)",
                  file=sys.stderr)

        if len(value_data) >= 8:
            page_limit = struct.unpack('<Q', value_data[:8])[0]
            print(f"TTMPageLimit: {format_pages(page_limit)}")
            print(f"Attributes: 0x{attrs:02x}")
        else:
            print(f"Error: Variable data too short ({len(value_data)} bytes)",
                  file=sys.stderr)
            sys.exit(1)

    except PermissionError:
        print("Error: Permission denied. Run as root.", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error reading EFI variable: {e}", file=sys.stderr)
        sys.exit(1)


def write_efi_variable(pages):
    """
    Write the TTMPageLimit EFI variable.

    Args:
        pages: Number of pages to set
    """
    if pages <= 0:
        print("Error: Page count must be positive", file=sys.stderr)
        sys.exit(1)

    try:
        with open('/proc/meminfo', 'r') as f:
            for line in f:
                if line.startswith('MemTotal:'):
                    mem_kb = int(line.split()[1])
                    total_pages = (mem_kb * 1024) // PAGE_SIZE
                    if pages > total_pages:
                        print(f"Warning: Page count ({pages}) exceeds system RAM ({total_pages} pages)",
                              file=sys.stderr)
                        print("The kernel will cap this to system RAM.", file=sys.stderr)
                    break
    except Exception as e:
        print(f"Warning: Could not validate against system RAM: {e}", file=sys.stderr)

    attrs = struct.pack('<I', EFI_ATTRS)
    value = struct.pack('<Q', pages)
    data = attrs + value

    try:
        if os.path.exists(VAR_FILE):
            remove_immutable(VAR_FILE)
            os.remove(VAR_FILE)

        fd = os.open(VAR_FILE, os.O_WRONLY | os.O_CREAT, 0o600)
        os.write(fd, data)
        os.close(fd)

        print(f"Successfully set TTMPageLimit to {format_pages(pages)}")
        print("Reboot required for changes to take effect.")

    except PermissionError:
        print("Error: Permission denied. Run as root and ensure efivarfs is writable.",
              file=sys.stderr)
        print("You may need to remount efivarfs with write access:", file=sys.stderr)
        print("  mount -o remount,rw /sys/firmware/efi/efivars", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error writing EFI variable: {e}", file=sys.stderr)
        sys.exit(1)


def delete_efi_variable():
    """Delete the TTMPageLimit EFI variable."""
    if not os.path.exists(VAR_FILE):
        print("TTMPageLimit EFI variable is not set (nothing to delete)")
        return

    try:
        remove_immutable(VAR_FILE)
        os.remove(VAR_FILE)
        print("Successfully deleted TTMPageLimit EFI variable")
        print("Reboot required for changes to take effect.")
    except PermissionError:
        print("Error: Permission denied. Run as root and ensure efivarfs is writable.",
              file=sys.stderr)
        print("You may need to remount efivarfs with write access:", file=sys.stderr)
        print("  mount -o remount,rw /sys/firmware/efi/efivars", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error deleting EFI variable: {e}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description='Manage TTM EFI page limit configuration',
        epilog='''
Examples:
  %(prog)s --read                  # Read current value
  %(prog)s --set 2G                # Set to 2 GB
  %(prog)s --set 512M              # Set to 512 MB
  %(prog)s --set-pages 524288      # Set to 524288 pages
  %(prog)s --delete                # Delete the variable
        ''',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--read', action='store_true',
                       help='Read the current TTMPageLimit value')
    group.add_argument('--set', metavar='SIZE',
                       help='Set TTMPageLimit (e.g., 2G, 512M)')
    group.add_argument('--set-pages', metavar='PAGES', type=int,
                       help='Set TTMPageLimit as raw page count')
    group.add_argument('--delete', action='store_true',
                       help='Delete the TTMPageLimit variable')

    args = parser.parse_args()

    check_root()
    check_efi_available()

    if args.read:
        read_efi_variable()
    elif args.set:
        pages = parse_size(args.set)
        write_efi_variable(pages)
    elif args.set_pages:
        write_efi_variable(args.set_pages)
    elif args.delete:
        delete_efi_variable()


if __name__ == '__main__':
    main()
