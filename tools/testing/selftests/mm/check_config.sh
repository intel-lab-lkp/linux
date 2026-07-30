#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Probe for libraries and create header files to record the results. Both C
# header files and Makefile include fragments are created.

OUTPUT_H_FILE=local_config.h
OUTPUT_MKFILE=local_config.mk

# Truncate output files so that re-running this script does not
# accumulate duplicate content (all writes below use append ">>").
: > $OUTPUT_H_FILE
: > $OUTPUT_MKFILE

tmpname=$(mktemp)
tmpfile_c=${tmpname}.c
tmpfile_o=${tmpname}.o

trap 'rm -f ${tmpname} ${tmpname}.*' EXIT

# liburing
echo "#include <sys/types.h>"        > $tmpfile_c
echo "#include <liburing.h>"        >> $tmpfile_c
echo "int func(void) { return 0; }" >> $tmpfile_c

$CC $CFLAGS -c $tmpfile_c -o $tmpfile_o >/dev/null 2>&1

if [ -f $tmpfile_o ]; then
    echo "#define LOCAL_CONFIG_HAVE_LIBURING 1"  >> $OUTPUT_H_FILE
    echo "IOURING_EXTRA_LIBS = -luring"          >> $OUTPUT_MKFILE
else
    echo "// No liburing support found"          >> $OUTPUT_H_FILE
    echo "# No liburing support found, so:"      >> $OUTPUT_MKFILE
    echo "IOURING_EXTRA_LIBS = "                >> $OUTPUT_MKFILE
fi

rm -f $tmpfile_o

# libnuma
echo "#include <numa.h>"            > $tmpfile_c
echo "#include <numaif.h>"         >> $tmpfile_c
echo "int func(void) { return 0; }" >> $tmpfile_c
echo "int main(void) { return 0; }" >> $tmpfile_c

$CC $CFLAGS -c $tmpfile_c -o $tmpfile_o >/dev/null 2>&1

if [ -f $tmpfile_o ]; then
    tmplink=${tmpname}.bin
    # Also check linking, not just compilation, because <numa.h> may be
    # available without the shared library (libnuma-dev vs libnuma)
    if $CC $CFLAGS $tmpfile_c -o $tmplink -lnuma >/dev/null 2>&1; then
        echo "#define LOCAL_CONFIG_HAVE_LIBNUMA 1"  >> $OUTPUT_H_FILE
        echo "NUMA_EXTRA_LIBS = -lnuma"             >> $OUTPUT_MKFILE
    else
        echo "// No libnuma support found"          >> $OUTPUT_H_FILE
        echo "# No libnuma support found, so:"      >> $OUTPUT_MKFILE
        echo "NUMA_EXTRA_LIBS = "                   >> $OUTPUT_MKFILE
    fi
else
    echo "// No libnuma support found"              >> $OUTPUT_H_FILE
    echo "# No libnuma support found, so:"          >> $OUTPUT_MKFILE
    echo "NUMA_EXTRA_LIBS = "                       >> $OUTPUT_MKFILE
fi

