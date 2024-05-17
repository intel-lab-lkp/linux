#!/usr/bin/gawk -f
# SPDX-License-Identifier: GPL-2.0
# generate_builtin_ranges.awk: Generate address range data for builtin modules
# Written by Kris Van Hees <kris.van.hees@oracle.com>
#
# Usage: generate_builtin_ranges.awk modules.builtin.modinfo vmlinux.map \
#		vmlinux.o.map > modules.builtin.ranges
#

BEGIN {
	# modules.builtin.modinfo uses \0 as record separator
	# All other files use \n.
	RS = "[\n\0]";
}

# Return the module name(s) (if any) associated with the given object.
#
# If we have seen this object before, return information from the cache.
# Otherwise, retrieve it from the corresponding .cmd file.
#
function get_module_info(fn, mod, obj, mfn, s) {
	if (fn in omod)
		return omod[fn];

	if (match(fn, /\/[^/]+$/) == 0)
		return "";

	obj = fn;
	mod = "";
	mfn = "";
	fn = substr(fn, 1, RSTART) "." substr(fn, RSTART + 1) ".cmd";
	if (getline s <fn == 1) {
		if (match(s, /DKBUILD_MODNAME=[^ ]+/) > 0) {
			mod = substr(s, RSTART + 17, RLENGTH - 17);
			gsub(/['"]/, "", mod);
			gsub(/:/, " ", mod);
		}

		if (match(s, /DKBUILD_MODFILE=[^ ]+/) > 0) {
			mfn = substr(s, RSTART + 17, RLENGTH - 17);
			gsub(/['"]/, "", mfn);
			gsub(/:/, " ", mfn);
		}
	}
	close(fn);

# tmp = $0;
# $0 = s;
# print mod " " mfn " " obj " " $NF;
# $0 = tmp;

	# A single module (common case) also reflects objects that are not part
	# of a module.  Some of those objects have names that are also a module
	# name (e.g. core).  We check the associated module file name, and if
	# they do not match, the object is not part of a module.
	if (mod !~ / /) {
		if (!(mod in mods))
			return "";
		if (mods[mod] != mfn)
			return "";
	}

	# At this point, mod is a single (valid) module name, or a list of
	# module names (that do not need validation).
	omod[obj] = mod;
	close(fn);

	return mod;
}

FNR == 1 {
	FC++;
}

# (1-old) Build a mapping to associate object files with built-in module names.
#
# The first file argument is used as input (modules.builtin.objs).
#
FC == 1 && old_behaviour {
	sub(/:/, "");
	mod = $1;
	sub(/([^/]*\/)+/, "", mod);
	sub(/\.o$/, "", mod);
	gsub(/-/, "_", mod);

	if (NF > 1) {
		for (i = 2; i <= NF; i++) {
			if ($i in mods)
				mods[$i] = mods[$i] " " mod;
			else
				mods[$i] = mod;
		}
	} else
		mods[$1] = mod;

	next;
}

# (1) Build a lookup map of built-in module names.
#
# The first file argument is used as input (modules.builtin.modinfo).
#
# We are interested in lines that follow the format
#     <modname>.file=<path>
# and use them to record <modname>
#
FC == 1 && /^[^\.]+.file=/ {
	gsub(/[\.=]/, " ");
# print $1 " -> " $3;
	mods[$1] = $3;
	next;
}

# (2) Determine the load address for each section.
#
# The second file argument is used as input (vmlinux.map).
#
# Since some AWK implementations cannot handle large integers, we strip of the
# first 4 hex digits from the address.  This is safe because the kernel space
# is not large enough for addresses to extend into those digits.
#
FC == 2 && /^\./ && NF > 2 {
	if (type)
		delete sect_addend[type];

	if ($1 ~ /percpu/)
		next;

	raw_addr = $2;
	addr_prefix = "^" substr($2, 1, 6);
	sub(addr_prefix, "0x", $2);
	base = strtonum($2);
	type = $1;
	anchor = 0;
	sect_base[type] = base;

	next;
}

!type {
	next;
}

# (3) We need to determine the base address of the section so that ranges can
# be expressed based on offsets from the base address.  This accommodates the
# kernel sections getting loaded at different addresses than what is recorded
# in vmlinux.map.
#
# At runtime, we will need to determine the base address of each section we are
# interested in.  We do that by recording the offset of the first symbol in the
# section.  Once we know the address of this symbol in the running kernel, we
# can calculate the base address of the section.
#
# If possible, we use an explicit anchor symbol (sym = .) listed at the base
# address (offset 0).
#
# If there is no such symbol, we record the first symbol in the section along
# with its offset.
#
# We also determine the offset of the first member in the section in case the
# final linking inserts some content between the start of the section and the
# first member.  I.e. in that case, vmlinux.map will list the first member at
# a non-zero offset whereas vmlinux.o.map will list it at offset 0.  We record
# the addend so we can apply it when processing vmlinux.o.map (next).
#
FC == 2 && !anchor && raw_addr == $1 && $3 == "=" && $4 == "." {
	anchor = sprintf("%s %08x-%08x = %s", type, 0, 0, $2);
	sect_anchor[type] = anchor;

	next;
}

FC == 2 && !anchor && $1 ~ /^0x/ && $2 !~ /^0x/ && NF <= 4 {
	sub(addr_prefix, "0x", $1);
	addr = strtonum($1) - base;
	anchor = sprintf("%s %08x-%08x = %s", type, addr, addr, $2);
	sect_anchor[type] = anchor;

	next;
}

FC == 2 && base && /^ \./ && $1 == type && NF == 4 {
	sub(addr_prefix, "0x", $2);
	addr = strtonum($2);
	sect_addend[type] = addr - base;

	if (anchor) {
		base = 0;
		type = 0;
	}

	next;
}

# (4) Collect offset ranges (relative to the section base address) for built-in
# modules.
#
FC == 3 && /^ \./ && NF == 4 && $3 != "0x0" {
	type = $1;
	if (!(type in sect_addend))
		next;

	sub(addr_prefix, "0x", $2);
	addr = strtonum($2) + sect_addend[type];

	mod = get_module_info($4);
# printf "[%s, %08x] %s [%s] %08x\n", mod_name, mod_start, $4, mod, addr;
	if (mod == mod_name)
		next;

	if (mod_name) {
		idx = mod_start + sect_base[type] + sect_addend[type];
		entries[idx] = sprintf("%s %08x-%08x %s", type, mod_start, addr, mod_name);
		count[type]++;
	}
# if (mod == "")
# printf "ENTRY WITHOUT MOD - MODULE MAY END AT %08x\n", addr

	mod_name = mod;
	mod_start = addr;
}

END {
	for (type in count) {
		if (type in sect_anchor)
			entries[sect_base[type]] = sect_anchor[type];
	}

	n = asorti(entries, indices);
	for (i = 1; i <= n; i++)
		print entries[indices[i]];
}
