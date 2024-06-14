#!/usr/bin/gawk -f
# SPDX-License-Identifier: GPL-2.0
# generate_builtin_ranges.awk: Generate address range data for builtin modules
# Written by Kris Van Hees <kris.van.hees@oracle.com>
#
# Usage: generate_builtin_ranges.awk modules.builtin vmlinux.map \
#		vmlinux.o.map > modules.builtin.ranges
#

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
		if (match(s, /DKBUILD_MODFILE=['"]+[^'"]+/) > 0) {
			mfn = substr(s, RSTART + 16, RLENGTH - 16);
			gsub(/['"]/, "", mfn);

			mod = mfn;
			gsub(/([^/ ]*\/)+/, "", mod);
			gsub(/-/, "_", mod);
		}
	}
	close(fn);

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

# (1) Build a lookup map of built-in module names.
#
# The first file argument is used as input (modules.builtin).
#
# Lines will be like:
#	kernel/crypto/lzo-rle.ko
# and we derive the built-in module name from this as "lzo_rle" and associate
# it with object name "crypto/lzo-rle".
#
FC == 1 {
	sub(/kernel\//, "");			# strip off "kernel/" prefix
	sub(/\.ko$/, "");			# strip off .ko suffix

	mod = $1;
	sub(/([^/]*\/)+/, "", mod);		# mod = basename($1)
	gsub(/-/, "_", mod);			# Convert - to _

	mods[mod] = $1;
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

# First determine whether we are dealing with a GNU ld or LLVM lld linker map.
#
FC == 2 && FNR == 1 && NF == 7 && $1 == "VMA" && $7 == "Symbol" {
	map_is_lld = 1;
	next;
}

# (LLD) Convert a section record fronm lld format to ld format.
#
FC == 2 && map_is_lld && NF == 5 && /[0-9] \./ {
	$0 = $5 " 0x"$1 " dummy";
}

# (LLD) Convert an anchor record from lld format to ld format.
#
FC == 2 && map_is_lld && !anchor && NF == 7 && raw_addr == "0x"$1 && $6 == "=" && $7 == "." {
	$0 = "0x"$1 " " $5 " = " $7;
}

# (LLD) Convert an object record from lld format to ld format.
#
FC == 2 && map_is_lld && NF == 5 && $5 ~ /:\(\./ {
	gsub(/\)/, "");
	sub(/:\(/, " ");
	sub(/ vmlinux\.a\(/, " ");
	$0 = " "$6 " 0x"$1 " 0x"$3 " " $5;
}

FC == 2 && /^\./ && NF > 2 {
	if (type)
		delete sect_addend[type];

	if ($1 ~ /\.percpu/)
		next;

	raw_addr = $2;
	addr_prefix = "^" substr($2, 1, 6);
	sub(addr_prefix, "0x", $2);
	base = strtonum($2);
	type = $1;
	tpat = "^ \\"type"[\\. ]";
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

FC == 2 && /^ \./ && NF == 1 {
	# If the section name is long, the remainder of the entry is found on
	# the next line.
	s = $0;
	getline;
	$0 = s " " $0;
}

FC == 2 && base && $0 ~ tpat && NF == 4 {
	# If the first object is vmlinux.o then we need vmlinux.o.map to get
	# the offsets of the actual objects.  That is valid because in this
	# case the vmlinux.o is linked into vmlinux verbatim (per section).
	if ($4 == "vmlinux.o")
		need_o_map = 1;

	sub(addr_prefix, "0x", $2);
	addr = strtonum($2);
	sect_addend[type] = addr - base;

	if (anchor)
		base = 0;
	if (need_o_map)
		type = 0;

	next;
}

FC == 2 && !need_o_map && $0 ~ tpat && NF == 4 {
	if ($1 ~ /\.percpu/ || !(type in sect_addend))
		next;

	sub(addr_prefix, "0x", $2);
	addr = strtonum($2) - sect_base[type];

	mod = get_module_info($4);
	if (mod == mod_name)
		next;

	if (mod_name) {
		idx = mod_start + sect_base[type];
		entries[idx] = sprintf("%s %08x-%08x %s", type, mod_start, addr, mod_name);
		count[type]++;
	}

	mod_name = mod;
	mod_start = addr;

	next;
}

# If we do not need to parse the vmlinux.o.map file, we are done.
FC == 3 && !need_o_map {
	exit;
}

# (4) Collect offset ranges (relative to the section base address) for built-in
# modules.
#

# (LLD) Convert an object record from lld format to ld format.
#
FC == 3 && map_is_lld && NF == 5 && $5 ~ /:\(\./ {
	gsub(/\)/, "");
	sub(/:\(/, " ");

	type = $6;
	if (!(type in sect_addend))
		next;

	sub(/ vmlinux\.a\(/, " ");
	$0 = " "type " 0x"$1 " 0x"$3 " " $5;
}

FC == 3 && /^ \./ && NF == 4 && $3 != "0x0" {
	type = $1;
	if (!(type in sect_addend))
		next;

	sub(addr_prefix, "0x", $2);
	addr = strtonum($2) + sect_addend[type];

	mod = get_module_info($4);
	if (mod == mod_name)
		next;

	if (mod_name) {
		idx = mod_start + sect_base[type] + sect_addend[type];
		entries[idx] = sprintf("%s %08x-%08x %s", type, mod_start, addr, mod_name);
		count[type]++;
	}

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
