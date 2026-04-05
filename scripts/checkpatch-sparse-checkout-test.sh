#!/bin/sh -eu
# SPDX-License-Identifier: GPL-2.0

srctree=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

mkdir -p "$tmpdir/Documentation/process" "$tmpdir/scripts"

for file in COPYING CREDITS Kbuild MAINTAINERS Makefile README; do
	cp "$srctree/$file" "$tmpdir/$file"
done

cp "$srctree/Documentation/process/submitting-patches.rst" \
	"$tmpdir/Documentation/process/"
cp "$srctree/scripts/checkpatch.pl" "$tmpdir/scripts/"
cp "$srctree/scripts/get_maintainer.pl" "$tmpdir/scripts/"
cp "$srctree/scripts/spdxcheck.py" "$tmpdir/scripts/"
cp "$srctree/scripts/spelling.txt" "$tmpdir/scripts/"

git -C "$tmpdir" init -q
git -C "$tmpdir" add \
	COPYING CREDITS Kbuild MAINTAINERS Makefile README \
	Documentation/process/submitting-patches.rst \
	scripts/checkpatch.pl scripts/get_maintainer.pl \
	scripts/spdxcheck.py scripts/spelling.txt

checkpatch_out=$(
	cd "$tmpdir" &&
	perl scripts/checkpatch.pl --file Documentation/process/submitting-patches.rst \
		2>&1 || true
)

echo "$checkpatch_out" |
	grep -q "Must be run from the top-level dir. of a kernel tree" &&
		exit 1

get_maintainer_out=$(
	cd "$tmpdir" &&
	perl scripts/get_maintainer.pl -f Documentation/process/submitting-patches.rst \
		2>&1
)

echo "$get_maintainer_out" |
	grep -q "The current directory does not appear to be a linux kernel source tree" &&
		exit 1

echo "$get_maintainer_out" | grep -q "Jonathan Corbet" || exit 1
