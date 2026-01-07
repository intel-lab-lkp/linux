#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

if [ $# -ne 2 ]
then
	echo "Usage: headers_install.sh INFILE OUTFILE"
	echo
	echo "Prepares kernel header files for use by user space, by removing"
	echo "all compiler.h definitions and #includes, removing any"
	echo "#ifdef __KERNEL__ sections, and putting __underscores__ around"
	echo "asm/inline/volatile keywords."
	echo
	echo "INFILE: header file to operate on"
	echo "OUTFILE: output file which the processed header is written to"

	exit 1
fi

# Grab arguments
INFILE=$1
OUTFILE=$2
TMPFILE=$OUTFILE.tmp

trap 'rm -f $OUTFILE $TMPFILE' EXIT

# SPDX-License-Identifier with GPL variants must have "WITH Linux-syscall-note"
if [ -n "$(sed -n -e "/SPDX-License-Identifier:.*GPL-/{/WITH Linux-syscall-note/!p}" $INFILE)" ]; then
	echo "error: $INFILE: missing \"WITH Linux-syscall-note\" for SPDX-License-Identifier" >&2
	exit 1
fi

sed -E -e '
	s/([[:space:](])(__user|__force|__iomem)[[:space:]]/\1/g
	s/__attribute_const__([[:space:]]|$)/\1/g
	s@^#include <linux/compiler.h>@@
	s/(^|[^a-zA-Z0-9])__packed([^a-zA-Z0-9_]|$)/\1__attribute__((packed))\2/g
	s/(^|[[:space:](])(inline|asm|volatile)([[:space:](]|$)/\1__\2__\3/g
	s@#(ifndef|define|endif[[:space:]]*/[*])[[:space:]]*_UAPI@#\1 @
' $INFILE > $TMPFILE || exit 1

scripts/unifdef -U__KERNEL__ -D__EXPORTED_HEADERS__ $TMPFILE > $OUTFILE
[ $? -gt 1 ] && exit 1

# Remove /* ... */ style comments, and find CONFIG_ references in code
sed -e '
:comment
	s:/\*[^*][^*]*:/*:
	s:/\*\*\**\([^/]\):/*\1:
	t comment
	s:/\*\*/: :
	t comment
	/\/\*/! b check
	N
	b comment
:print
	# The entries in the following list do not result in an error.
	# Please do not add a new entry. This list is only for existing ones.
	# The list will be reduced gradually, and deleted eventually.
	#
	# The format is s@<file-name>:<CONFIG-option>\n@@ in each line.
	s@arch/arc/include/uapi/asm/swab.h:CONFIG_ARC_HAS_SWAPE\n@@
	s@arch/arm/include/uapi/asm/ptrace.h:CONFIG_CPU_ENDIAN_BE8\n@@
	s@arch/nios2/include/uapi/asm/swab.h:CONFIG_NIOS2_CI_SWAB_NO\n@@
	s@arch/nios2/include/uapi/asm/swab.h:CONFIG_NIOS2_CI_SWAB_SUPPORT\n@@
	s@arch/x86/include/uapi/asm/auxvec.h:CONFIG_IA32_EMULATION\n@@
	s@arch/x86/include/uapi/asm/auxvec.h:CONFIG_X86_64\n@@

	# Jump if any of the above filters applied, otherwise error out.
	t check
	s@^\(.*\)\n.*@error: \1 leak to user-space@
	P
	Q2
:check
	s@^\(CONFIG_[[:alnum:]_]*\)@'"$INFILE"':\1\n@
	t print
	s:^[[:alnum:]_][[:alnum:]_]*::
	s:^[^[:alnum:]_][^[:alnum:]_]*::
	t check
	d
' $OUTFILE >&2 || exit 1

rm -f $TMPFILE
trap - EXIT
