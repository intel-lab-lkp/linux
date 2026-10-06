#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/../../../.." && pwd)
kernel_image=${1:-"$repo_root/arch/x86/boot/bzImage"}
vmlinux=${2:-"$repo_root/vmlinux"}
test_binary="$script_dir/persistent_keyring_expiry"

for tool in cpio qemu-system-x86_64 busybox ldd; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "required tool not found: $tool" >&2
		exit 1
	fi
done

make -C "$script_dir"
if [ ! -r "$kernel_image" ] || [ ! -r "$vmlinux" ]; then
	echo "usage: $0 [bzImage] [vmlinux]" >&2
	exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM
rootfs="$tmpdir/rootfs"
initrd="$tmpdir/initramfs.cpio"
mkdir -p "$rootfs/bin" "$rootfs/dev" "$rootfs/proc" "$rootfs/keys"

copy_binary()
{
	source=$1
	destination=$2
	install -D "$source" "$rootfs$destination"

	ldd "$source" 2>/dev/null |
		awk '$2 == "=>" && $3 ~ /^\// { print $3 }
		     $1 ~ /^\// { print $1 }' |
		sort -u |
		while IFS= read -r library; do
			[ -f "$library" ] || continue
			cp -L --parents "$library" "$rootfs"
		done
}

copy_binary "$(command -v busybox)" /bin/busybox
copy_binary "$test_binary" /keys/persistent_keyring_expiry
install -m 755 "$script_dir/initramfs-init.sh" "$rootfs/init"

(
	cd "$rootfs"
	find . -print0 | cpio --null -o --format=newc
) > "$initrd"

echo "QEMU is paused at startup; attach GDB to localhost:1234 and continue."
qemu-system-x86_64 \
	-kernel "$kernel_image" \
	-initrd "$initrd" \
	-append "console=ttyS0 nokaslr rdinit=/init" \
	-display none \
	-serial stdio \
	-monitor none \
	-gdb tcp::1234 \
	-S
