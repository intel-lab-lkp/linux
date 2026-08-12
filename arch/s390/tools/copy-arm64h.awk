#!/usr/bin/awk -f
# SPDX-License-Identifier: GPL-2.0
#
# Extract marked sections from ARM64 headers for sharing with s390 KVM
#
# Usage: share-arm64-header.awk <input_file>
#
# Extracts all sections between start/end markers. If no markers found, signals for fallback.
#TODO verification script or c file for arm to verify the ifdef things keep working
BEGIN {
	# Constants
	start_pattern = "^#ifdef ARM64_S390_COMMON$"
	end_pattern = "^#endif \\/\\* ARM64_S390_COMMON \\*\\/$"
	max_guard_line = 25

	# State variables
	copying = found_marker = 0
	guard_name = ""
	file_header_done = 0
}

NR <= max_guard_line && !guard_name && /^#ifndef [A-Za-z0-9_]+$/ {
	guard_name = $2
	filename = FILENAME
	sub(/^.*arch\/arm64\//, "arch/arm64/", filename)
	print "/*"
	print " * This header was automatically generated from " filename
	print " * Do not modify this file directly."
	print " */"
	print "#ifndef " guard_name
	print "#define " guard_name
	print ""
	next
}

NR > max_guard_line && !guard_name && !file_header_done {
	print "error: no include guard found in first " max_guard_line " lines" > "/dev/stderr"
	file_header_done = 1
	exit 1
}

!guard_name {
	print
	next
}

$0 ~ start_pattern {
	if (copying) { exit 1 }
	copying = found_marker = 1
	next
}

guard_name && !found_marker {
	next
}

$0 ~ end_pattern { copying = 0; next }

copying {
	gsub(/#include <asm\//, "#include <arm64/")
	gsub(/#include <uapi\/asm\//, "#include <uapi/arm64/")
	print
	next
}

END {
	if (found_marker) {
		print ""
		print "#endif /* " guard_name " */"
	}
	exit !found_marker || copying
}
