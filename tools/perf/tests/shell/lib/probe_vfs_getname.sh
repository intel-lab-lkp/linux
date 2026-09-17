#!/bin/bash
# Arnaldo Carvalho de Melo <acme@kernel.org>, 2017

# The name of the getname_flags probe added and removed below.
#
# It is scoped to the pid so that tests running in parallel do not collide,
# and it deliberately does not start with "vfs_getname": perf trace calls
# evlist__add_vfs_getname(), which opens everything matching the hardcoded
# "probe:vfs_getname*" wildcard, so a perf trace running in another test would
# otherwise pin this probe and make the 'perf probe -d' below fail with -EBUSY.
#
# trace+probe_vfs_getname.sh is the one test that does want to be found that
# way, so it sets vfs_getname itself before sourcing this file, and is
# (exclusive) as a result.
: "${vfs_getname:=getname_flags_$$}"

# Print the probes add_probe_vfs_getname() created. perf probe appends _1, _2,
# ... when getname_flags is inlined at more than one call site, so there can be
# several. Match them exactly rather than with a "${vfs_getname}*" glob: the
# name ends in a pid, so such a glob would also match the probes of a test
# whose pid merely starts with this one's, e.g. 123 and 1234.
probes_vfs_getname() {
	perf probe -l 2>/dev/null | awk '{print $1}' |
		grep -E "^probe:${vfs_getname}(_[[:digit:]]+)?$"
}

[ -n "$(probes_vfs_getname)" ]
had_vfs_getname=$?

cleanup_probe_vfs_getname() {
	if [ $had_vfs_getname -eq 1 ] ; then
		local probe
		for probe in $(probes_vfs_getname); do
			perf probe -q -d "$probe"
		done
	fi
}

add_probe_vfs_getname() {
	add_probe_verbose=$1
	if [ $had_vfs_getname -eq 1 ] ; then
		result_initname_re="[[:space:]]+([[:digit:]]+)[[:space:]]+initname.*"
		line=$(perf probe -L getname_flags 2>&1 | grep -E "$result_initname_re" | sed -r "s/$result_initname_re/\1/")

		# Search the old regular expressions so that this will
		# pass on older kernels as well.
		if [ -z "$line" ] ; then
			result_filename_re="[[:space:]]+([[:digit:]]+)[[:space:]]+result->uptr.*"
			line=$(perf probe -L getname_flags 2>&1 | grep -E "$result_filename_re" | sed -r "s/$result_filename_re/\1/")
		fi

		if [ -z "$line" ] ; then
			result_aname_re="[[:space:]]+([[:digit:]]+)[[:space:]]+result->aname = NULL;"
			line=$(perf probe -L getname_flags 2>&1 | grep -E "$result_aname_re" | sed -r "s/$result_aname_re/\1/")
		fi

		if [ -z "$line" ] ; then
			echo "Could not find probeable line"
			return 2
		fi

		perf probe -q       "${vfs_getname}=getname_flags:${line} pathname=result->name:string" || \
		perf probe $add_probe_verbose "${vfs_getname}=getname_flags:${line} pathname=filename:ustring" || return 1
	fi
}

skip_if_no_debuginfo() {
	add_probe_vfs_getname -v 2>&1 | grep -E -q "^(Failed to find the path for the kernel|Debuginfo-analysis is not supported)|(file has no debug information)" && return 2
	return 1
}

# check if perf is compiled with libtraceevent support
skip_no_probe_record_support() {
	if [ $had_vfs_getname -eq 1 ] ; then
		perf check feature -q libtraceevent && return 1
		return 2
	fi
}
