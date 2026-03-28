#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# One-time setup script for the NFS test server.
# Run this once before running benchmarks.
#
# Usage: sudo ./setup-server.sh [EXPORT_PATH]

set -euo pipefail

EXPORT_PATH="${1:-/export}"
FSTYPE="ext4"

log() {
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

if [ "$(id -u)" -ne 0 ]; then
	echo "ERROR: must run as root"
	exit 1
fi

# Check for required tools
for cmd in fio exportfs showmount jq; do
	if ! command -v "$cmd" &>/dev/null; then
		echo "WARNING: $cmd not found, attempting install"
		dnf install -y "$cmd" 2>/dev/null || \
		apt-get install -y "$cmd" 2>/dev/null || \
		echo "ERROR: cannot install $cmd, please install manually"
	fi
done

# Check fio has nfs ioengine
if ! fio --enghelp=nfs &>/dev/null; then
	echo "ERROR: fio nfs ioengine not available."
	echo "You may need to install fio with libnfs support."
	echo "Try: dnf install fio libnfs-devel  (or build fio from source with --enable-nfs)"
	exit 1
fi

# Create export directory if needed
if [ ! -d "$EXPORT_PATH" ]; then
	log "Creating export directory: $EXPORT_PATH"
	mkdir -p "$EXPORT_PATH"
fi

# Create subdirectories for multi-client tests
for i in 1 2 3 4; do
	mkdir -p "${EXPORT_PATH}/client${i}"
	mkdir -p "${EXPORT_PATH}/reader${i}"
done
mkdir -p "${EXPORT_PATH}/bulk"

# Check if already exported
if ! exportfs -s 2>/dev/null | grep -q "$EXPORT_PATH"; then
	log "Adding NFS export for $EXPORT_PATH"
	if ! grep -q "$EXPORT_PATH" /etc/exports 2>/dev/null; then
		echo "${EXPORT_PATH} 127.0.0.1/32(rw,sync,no_root_squash,no_subtree_check)" >> /etc/exports
	fi
	exportfs -ra
fi

# Ensure NFS server is running
if ! systemctl is-active --quiet nfs-server 2>/dev/null; then
	log "Starting NFS server"
	systemctl start nfs-server
fi

# Verify export
log "Current exports:"
showmount -e localhost

# Check debugfs knobs
log "Checking debugfs knobs:"
DEBUGFS_BASE="/sys/kernel/debug/nfsd"
for knob in io_cache_read io_cache_write disable-splice-read; do
	if [ -f "${DEBUGFS_BASE}/${knob}" ]; then
		echo "  ${knob} = $(cat "${DEBUGFS_BASE}/${knob}")"
	else
		echo "  ${knob}: NOT FOUND (kernel may be too old)"
	fi
done

# Print system summary
echo ""
log "=== System Summary ==="
echo "Kernel:      $(uname -r)"
echo "RAM:         $(awk '/MemTotal/ {printf "%.1f GB", $2/1024/1024}' /proc/meminfo)"
echo "Export:      $EXPORT_PATH"
echo "Filesystem:  $(df -T "$EXPORT_PATH" | awk 'NR==2 {print $2}')"
echo "Disk:        $(df -h "$EXPORT_PATH" | awk 'NR==2 {print $2, "total,", $4, "free"}')"
echo ""
log "Setup complete. Run benchmarks with:"
echo "  sudo ./scripts/run-benchmarks.sh -e $EXPORT_PATH"
