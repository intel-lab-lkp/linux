#!/usr/bin/awk -f
# SPDX-License-Identifier: GPL-2.0
#
# Extract marked sections from ARM64 C files for sharing with s390 KVM
#
# Usage: share-arm64-cfile.awk <input_file>
#
# Extracts all sections between start/end markers. If no markers found, signals failure.

BEGIN {
	# Constants
	start_pattern = "^#ifdef ARM64_S390_COMMON$"
	end_pattern = "^#endif /\\* ARM64_S390_COMMON \\*/$"

	# State variables
	copying = found_marker = 0
	file_header_done = 0
}

!file_header_done {
	if (/^\/\*/ || /^\/\/ SPDX-License-Identifier:/) {
		print
		next
	}
	if (/[[:space:]]\*([[:space:]]|$)/) {
		print
		next
	}
	if (/\*\//) {
		print " *"
	} else {
		print "/*"
	}

	filename = FILENAME
	sub(/^.*arch\/arm64\//, "arch/arm64/", filename)
	print " * This file was automatically generated from " filename
	print " * Do not modify this file directly."
	print " */"
	print ""
	print "#ifndef __INCL_GEN_ARM_FILE"
	print "#error included .inc file w/o proper guard definition"
	print "#else"
	print "#undef __INCL_GEN_ARM_FILE"
	print "#endif /* __INCL_GEN_ARM_FILE */"
	print ""

	file_header_done = 1
}

$0 ~ start_pattern {
	if (copying) { exit 1 }
	copying = found_marker = 1
	next
}

$0 ~ end_pattern {
	if (!copying) { exit 1 }
	copying = 0
	next
}

copying {
	gsub(/#include <asm\//, "#include <arm64/")
	gsub(/#include <uapi\/asm\//, "#include <uapi/arm64/")
	print
	next
}

END {
	exit !found_marker || copying
}
