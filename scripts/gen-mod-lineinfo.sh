#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# gen-mod-lineinfo.sh - Build a kernel module's source line info object
#
# Reads DWARF from a module's partially linked image and emits an object
# holding the module's .mod_lineinfo section.  The section carries an ELF
# relocation against the module's .text section symbol, so the caller has to
# link this object into the module for the relocation to ride along to the
# module loader.  Modeled on scripts/gen-btf.sh.

set -e

if [ $# -ne 2 ]; then
	echo "Usage: $0 <module-prelink.o> <output.o>" >&2
	exit 1
fi

IN="$1"
OUT="$2"

cleanup() {
	rm -f "${OUT}.S"
}
trap cleanup EXIT

case "${KBUILD_VERBOSE}" in
*1*)
	set -x
	;;
esac

# Generate assembly from DWARF -- if it fails (no DWARF), silently skip.
# Leaving no output behind is how the caller is told there is no table.
if ! ${objtree}/scripts/gen_lineinfo --module "${IN}" > "${OUT}.S"; then
	exit 0
fi

# KBUILD_AFLAGS carries the target flags the generated .S has to agree with,
# e.g. -m32 on a biarch host.
${CC} ${NOSTDINC_FLAGS} ${LINUXINCLUDE} ${KBUILD_CPPFLAGS} \
	${KBUILD_AFLAGS} ${KBUILD_AFLAGS_MODULE} \
	-c -o "${OUT}" "${OUT}.S"

exit 0
