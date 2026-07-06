#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# gen-mod-lineinfo.sh - Embed source line info into a kernel module (.ko)
#
# Reads DWARF from the .ko, generates a .mod_lineinfo section that contains
# an ELF relocation against the module's .text section symbol, and partial-
# links the result back into the .ko via "ld -r" so the relocation rides
# along to the module loader.  Modeled on scripts/gen-btf.sh.

set -e

if [ $# -ne 1 ]; then
	echo "Usage: $0 <module.ko>" >&2
	exit 1
fi

KO="$1"

cleanup() {
	rm -f "${KO}.lineinfo.S" "${KO}.lineinfo.o" "${KO}.lineinfo.tmp"
}
trap cleanup EXIT

case "${KBUILD_VERBOSE}" in
*1*)
	set -x
	;;
esac

# Generate assembly from DWARF -- if it fails (no DWARF), silently skip
if ! ${objtree}/scripts/gen_lineinfo --module "${KO}" > "${KO}.lineinfo.S"; then
	exit 0
fi

# Compile assembly to object file
${CC} ${NOSTDINC_FLAGS} ${LINUXINCLUDE} ${KBUILD_CPPFLAGS} \
	${KBUILD_AFLAGS} ${KBUILD_AFLAGS_MODULE} \
	-c -o "${KO}.lineinfo.o" "${KO}.lineinfo.S"

# Partial-link lineinfo.o INTO the .ko.  Order matters: lineinfo.o must come
# FIRST so its empty .text contributes 0 bytes at offset 0 of the merged
# .text, which keeps the .quad .text relocation (against lineinfo.o's local
# .text symbol, which after merge points at offset 0 of merged .text)
# resolving to the start of the module's .text.  Reversing inputs here
# silently breaks lookup correctness.
${LD} -r "${KO}.lineinfo.o" "${KO}" -o "${KO}.lineinfo.tmp"
mv "${KO}.lineinfo.tmp" "${KO}"

exit 0
