#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Generates compressed kernel headers archive for CONFIG_IKHEADERS
# Supports incremental builds by tracking MD5 checksums of inputs

set -e

sfile="$(readlink -f "$0")"
outdir="$(pwd)"
tarfile="$1"
tmpdir="$outdir/${tarfile%/*}/.tmp_dir"

# Target header directories
dir_list="
include/
arch/$SRCARCH/include/
"

all_dirs=
if [ "$building_out_of_srctree" ]; then
    for d in $dir_list; do
        all_dirs="$all_dirs $srctree/$d"  # Preserve original directory order
    done
fi
all_dirs="$all_dirs $dir_list"

# Checksum calculation excludes generated files that change frequently but don't
# affect header functionality. This prevents unnecessary rebuilds when:
# - Only autoconf.h timestamps change (content remains identical)
# - utsversion.h gets regenerated (contains volatile build info)
headers_md5="$(find $all_dirs -name "*.h" -a \
        ! -path "include/generated/utsversion.h" -a \
        ! -path "include/generated/autoconf.h" 2>/dev/null |
    xargs ls -l | md5sum | cut -d ' ' -f1)"  # ls -l captures timestamps and sizes
this_file_md5="$(ls -l "$sfile" | md5sum | cut -d ' ' -f1)"  # Track script changes

# Three-layer incremental build check: headers, script, and final archive
if [ -f "$tarfile" ] && [ -f "kernel/kheaders.md5" ]; then
    tarfile_md5="$(md5sum "$tarfile" | cut -d ' ' -f1)"
    if [ "$(head -n 1 kernel/kheaders.md5)" = "$headers_md5" ] &&  # Header content
       [ "$(head -n 2 kernel/kheaders.md5 | tail -n 1)" = "$this_file_md5" ] &&  # Script
       [ "$(tail -n 1 kernel/kheaders.md5)" = "$tarfile_md5" ]; then  # Archive
        exit 0
    fi
fi

echo "  GEN     $tarfile"

rm -rf "${tmpdir}"
mkdir "${tmpdir}"

# Build processing
if [ "$building_out_of_srctree" ]; then
    (
        cd "$srctree"
        for f in $dir_list; do
            find "$f" -name "*.h" 2>/dev/null  # Silent but fails on major errors
        done | tar -c -f - -T - 2>/dev/null  # Stream to avoid temp files
    ) | tar -xf - -C "${tmpdir}" 2>/dev/null  # Extract directly to target
fi

# In-tree processing uses same streaming approach for consistency
for f in $dir_list; do
    find "$f" -name "*.h" 2>/dev/null
done | tar -c -f - -T - 2>/dev/null | tar -xf - -C "${tmpdir}" 2>/dev/null

# Remove volatile utsversion.h to ensure reproducible builds
rm -f "${tmpdir}/include/generated/utsversion.h" 2>/dev/null

# Use a temporary file to store directory contents to prevent find/xargs from
# seeing temporary files created by perl.
find "${tmpdir}" -type f -print0 2>/dev/null | xargs -0 -P8 -n1 \
    perl -pi -e 'BEGIN {undef $/;}; s/\/\*((?!SPDX).)*?\*\///smg;' 2>/dev/null

# Create final archive with normalized metadata for reproducibility using
# fixed timestamps (when KBUILD_BUILD_TIMESTAMP set)
tar "${KBUILD_BUILD_TIMESTAMP:+--mtime=$KBUILD_BUILD_TIMESTAMP}" \
    --owner=0 --group=0 --sort=name --numeric-owner --mode=u=rw,go=r,a+X \
    -I "$XZ" -cf "$tarfile" -C "${tmpdir}/" . >/dev/null 2>&1

# Atomic checksum update (all three values written together)
mkdir -p kernel
{
    echo "$headers_md5"  # Header content fingerprint
    echo "$this_file_md5"  # Script version marker
    md5sum "$tarfile" | cut -d ' ' -f1  # Final archive integrity
} > kernel/kheaders.md5

rm -rf "${tmpdir}"