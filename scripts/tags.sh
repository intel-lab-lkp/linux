#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Generate tags or cscope files
# Usage tags.sh <mode>
#
# mode may be any of: tags, gtags, TAGS, cscope
#
# Uses the following environment variables:
# SUBARCH, SRCARCH, srctree

if [[ "$KBUILD_VERBOSE" =~ 1 ]]; then
	set -x
fi

# RCS_FIND_IGNORE has escaped ()s -- remove them.
ignore="$(echo "$RCS_FIND_IGNORE" | sed 's|\\||g' )"
# tags and cscope files should also ignore MODVERSION *.mod.c files
ignore="$ignore ( -name *.mod.c ) -prune -o"

# ignore arbitrary directories
if [ -n "${IGNORE_DIRS}" ]; then
	for i in ${IGNORE_DIRS}; do
		ignore="${ignore} ( -path $i ) -prune -o"
	done
fi

# Use make KBUILD_ABS_SRCTREE=1 {tags|cscope}
# to force full paths for a non-O= build
if [ "${srctree}" = "." -o -z "${srctree}" ]; then
	tree=
else
	tree=${srctree}/
fi

# gtags(1) refuses to index any file outside of its current working dir.
# If gtags indexing is requested and the build output directory is not
# the kernel source tree, index all files in absolute-path form.
if [[ "$1" == "gtags" && -n "${tree}" ]]; then
	tree=$(realpath "$tree")/
fi

# Detect if ALLSOURCE_ARCHS is set. If not, we assume SRCARCH
if [ "${ALLSOURCE_ARCHS}" = "" ]; then
	ALLSOURCE_ARCHS=${SRCARCH}
elif [ "${ALLSOURCE_ARCHS}" = "all" ]; then
	ALLSOURCE_ARCHS=$(find ${tree}arch/ -mindepth 1 -maxdepth 1 -type d -printf '%f ')
fi

setup_name_pattern()
{
	pattern=()
	for ext; do
		if [ ${#pattern[@]} -gt 0 ]; then
			pattern+=("-o" "-name" "$ext")
		else
			pattern+=("(" "-name" "$ext")
		fi
	done
	if [ ${#pattern[@]} -gt 0 ]; then
		pattern+=(")")
	fi
}

# find sources in arch/$1
find_arch_sources()
{
	for i in $archincludedir; do
		local prune="$prune ( -path $i ) -prune -o"
	done
	local src=${tree}arch/$1
	shift
	setup_name_pattern "$@"
	find $src $ignore $prune "${pattern[@]}" -not -type l -print;
}

# find sources in arch/$1/include
find_arch_include_sources()
{
	local include=$(find ${tree}arch/$1/ -name include -type d -print);
	if [ -n "$include" ]; then
		archincludedir="$archincludedir $include"
		shift
		setup_name_pattern "$@"
		find $include $ignore "${pattern[@]}" -not -type l -print;
	fi
}

# find sources in include/
find_include_sources()
{
	setup_name_pattern "$@"
	find ${tree}include $ignore -name config -prune -o "${pattern[@]}" \
		-not -type l -print;
}

# find sources in rest of tree
# we could benefit from a list of dirs to search in here
find_other_sources()
{
	setup_name_pattern "$@"
	find ${tree}* $ignore \
	     \( -path ${tree}include -o -path ${tree}arch -o -name '.tmp_*' \) -prune -o \
	       "${pattern[@]}" -not -type l -print;
}

all_sources()
{
	find_arch_include_sources ${SRCARCH} '*.[chS]' '*.rs'
	if [ -n "$archinclude" ]; then
		find_arch_include_sources $archinclude '*.[chS]' '*.rs'
	fi
	find_include_sources '*.[chS]' '*.rs'
	for arch in $ALLSOURCE_ARCHS
	do
		find_arch_sources $arch '*.[chS]' '*.rs'
	done
	find_other_sources '*.[chS]' '*.rs'
}

all_compiled_sources()
{
	local file
	{
		echo include/generated/autoconf.h
		# A .cmd batch with no source references is not an error.
		find $ignore -name "*.cmd" -exec sh -c '
			pattern=$1
			shift
			grep -Poh "$pattern" "$@" || [ "$?" -eq 1 ]
		' sh '(?<=^  )\S+\.([chS]|rs)(?=\s)|(?<== )\S+\.(?1)(?=$)' {} \+ |
		awk '!a[$0]++'
	} | while IFS= read -r file; do
		# Old .cmd files may reference sources removed since the last build.
		if [ -f "$file" ]; then
			printf '%s\n' "$file"
		fi
	done | xargs -r realpath -esq $([ -z "$KBUILD_ABS_SRCTREE" ] && echo --relative-to=.) |
	sort -u
}

all_target_sources()
{
	if [ -n "$tags_input" ]; then
		sed -n 's/^s //p' "$tags_input.files"
	elif [ -n "$COMPILED_SOURCE" ]; then
		all_compiled_sources
	else
		all_sources
	fi
}

all_kconfigs()
{
	if [ -n "$tags_input" ]; then
		sed -n 's/^k //p' "$tags_input.files"
		return
	fi
	find ${tree}arch/ -maxdepth 1 $ignore \
	       -name "Kconfig*" -not -type l -print;
	for arch in $ALLSOURCE_ARCHS; do
		find_arch_sources $arch 'Kconfig*'
	done
	find_other_sources 'Kconfig*'
}

docscope()
{
	(echo \-k; echo \-q; all_target_sources) > cscope.files
	cscope -b -f cscope.out
}

dogtags()
{
	all_target_sources | gtags -i -C "${tree:-.}" -f - "$PWD"
}

# Basic regular expressions with an optional /kind-spec/ for ctags and
# the following limitations:
# - No regex modifiers
# - Use \{0,1\} instead of \?, because etags expects an unescaped ?
# - \s is not working with etags, use a space or [ \t]
# - \w works, but does not match underscores in etags
# - etags regular expressions have to match at the start of a line;
#   a ^[^#] is prepended by setup_regex unless an anchor is already present
regex_asm=(
	'/^\(ENTRY\|_GLOBAL\)([[:space:]]*\([[:alnum:]_\\]*\)).*/\2/'
	'/^SYM_[[:alnum:]_]*START[[:alnum:]_]*([[:space:]]*\([[:alnum:]_\\]*\)).*/\1/'
)
regex_c=(
	'/^SYSCALL_DEFINE[0-9]([[:space:]]*\([[:alnum:]_]*\).*/sys_\1/'
	'/^BPF_CALL_[0-9]([[:space:]]*\([[:alnum:]_]*\).*/\1/'
	'/^COMPAT_SYSCALL_DEFINE[0-9]([[:space:]]*\([[:alnum:]_]*\).*/compat_sys_\1/'
	'/^TRACE_EVENT([[:space:]]*\([[:alnum:]_]*\).*/trace_\1/'
	'/^DEFINE_EVENT([^,)]*,[[:space:]]*\([[:alnum:]_]*\).*/trace_\1/'
	'/^DEFINE_INSN_CACHE_OPS([[:space:]]*\([[:alnum:]_]*\).*/get_\1_slot/'
	'/^DEFINE_INSN_CACHE_OPS([[:space:]]*\([[:alnum:]_]*\).*/free_\1_slot/'
	'/^PAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/Page\1/'
	'/^PAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/SetPage\1/'
	'/^PAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/ClearPage\1/'
	'/^TESTSETFLAG([[:space:]]*\([[:alnum:]_]*\).*/TestSetPage\1/'
	'/^TESTPAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/Page\1/'
	'/^SETPAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/SetPage\1/'
	'/\<__SETPAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/__SetPage\1/'
	'/\<TESTCLEARFLAG([[:space:]]*\([[:alnum:]_]*\).*/TestClearPage\1/'
	'/\<__TESTCLEARFLAG([[:space:]]*\([[:alnum:]_]*\).*/TestClearPage\1/'
	'/\<CLEARPAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/ClearPage\1/'
	'/\<__CLEARPAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/__ClearPage\1/'
	'/^__PAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/__SetPage\1/'
	'/^__PAGEFLAG([[:space:]]*\([[:alnum:]_]*\).*/__ClearPage\1/'
	'/^PAGEFLAG_FALSE([[:space:]]*\([[:alnum:]_]*\).*/Page\1/'
	'/\<TESTSCFLAG([[:space:]]*\([[:alnum:]_]*\).*/TestSetPage\1/'
	'/\<TESTSCFLAG([[:space:]]*\([[:alnum:]_]*\).*/TestClearPage\1/'
	'/\<SETPAGEFLAG_NOOP([[:space:]]*\([[:alnum:]_]*\).*/SetPage\1/'
	'/\<CLEARPAGEFLAG_NOOP([[:space:]]*\([[:alnum:]_]*\).*/ClearPage\1/'
	'/\<__CLEARPAGEFLAG_NOOP([[:space:]]*\([[:alnum:]_]*\).*/__ClearPage\1/'
	'/\<TESTCLEARFLAG_FALSE([[:space:]]*\([[:alnum:]_]*\).*/TestClearPage\1/'
	'/^PAGE_TYPE_OPS([[:space:]]*\([[:alnum:]_]*\).*/Page\1/'
	'/^PAGE_TYPE_OPS([[:space:]]*\([[:alnum:]_]*\).*/__SetPage\1/'
	'/^PAGE_TYPE_OPS([[:space:]]*\([[:alnum:]_]*\).*/__ClearPage\1/'
	'/^TASK_PFA_TEST([^,]*,[[:space:]]*\([[:alnum:]_]*\))/task_\1/'
	'/^TASK_PFA_SET([^,]*,[[:space:]]*\([[:alnum:]_]*\))/task_set_\1/'
	'/^TASK_PFA_CLEAR([^,]*,[[:space:]]*\([[:alnum:]_]*\))/task_clear_\1/'
	'/^DEF_MMIO_\(IN\|OUT\)_[XD]([[:space:]]*\([[:alnum:]_]*\),[^)]*)/\2/'
	'/^DEBUGGER_BOILERPLATE([[:space:]]*\([[:alnum:]_]*\))/\1/'
	'/^DEF_PCI_AC_\(\|NO\)RET([[:space:]]*\([[:alnum:]_]*\).*/\2/'
	'/^PCI_OP_READ([[:space:]]*\(\w*\).*[1-4])/pci_bus_read_config_\1/'
	'/^PCI_OP_WRITE([[:space:]]*\(\w*\).*[1-4])/pci_bus_write_config_\1/'
	'/\<DEFINE_\(RT_MUTEX\|MUTEX\|SEMAPHORE\|SPINLOCK\)([[:space:]]*\([[:alnum:]_]*\)/\2/v/'
	'/\<DEFINE_\(RAW_SPINLOCK\|RWLOCK\|SEQLOCK\)([[:space:]]*\([[:alnum:]_]*\)/\2/v/'
	'/\<DEFINE_TIMER(\([^,)]*\),/\1/'
	'/\<DECLARE_\(RWSEM\|COMPLETION\)([[:space:]]*\([[:alnum:]_]\+\)/\2/v/'
	'/\<DECLARE_BITMAP([[:space:]]*\([[:alnum:]_]\+\)/\1/v/'
	'/\(^\|\s\)\(\|L\|H\)LIST_HEAD([[:space:]]*\([[:alnum:]_]*\)/\3/v/'
	'/\(^\|\s\)RADIX_TREE([[:space:]]*\([[:alnum:]_]*\)/\2/v/'
	'/\<DEFINE_PER_CPU([^,]*,[[:space:]]*\([[:alnum:]_]*\)/\1/v/'
	'/\<DEFINE_PER_CPU_SHARED_ALIGNED([^,]*,[[:space:]]*\([[:alnum:]_]*\)/\1/v/'
	'/\<DECLARE_WAIT_QUEUE_HEAD([[:space:]]*\([[:alnum:]_]*\)/\1/v/'
	'/\<DECLARE_\(TASKLET\|WORK\|DELAYED_WORK\)([[:space:]]*\([[:alnum:]_]*\)/\2/v/'
	'/\(^\s\)OFFSET([[:space:]]*\([[:alnum:]_]*\)/\2/v/'
	'/\(^\s\)DEFINE([[:space:]]*\([[:alnum:]_]*\)/\2/v/'
	'/\<\(DEFINE\|DECLARE\)_HASHTABLE([[:space:]]*\([[:alnum:]_]*\)/\2/v/'
	'/\<DEFINE_ID\(R\|A\)([[:space:]]*\([[:alnum:]_]\+\)/\2/'
	'/\<DEFINE_WD_CLASS([[:space:]]*\([[:alnum:]_]\+\)/\1/'
	'/\<ATOMIC_NOTIFIER_HEAD([[:space:]]*\([[:alnum:]_]\+\)/\1/'
	'/\<RAW_NOTIFIER_HEAD([[:space:]]*\([[:alnum:]_]\+\)/\1/'
	'/\<DECLARE_FAULT_ATTR([[:space:]]*\([[:alnum:]_]\+\)/\1/'
	'/\<BLOCKING_NOTIFIER_HEAD([[:space:]]*\([[:alnum:]_]\+\)/\1/'
	'/\<DEVICE_ATTR_\(RW\|RO\|WO\)([[:space:]]*\([[:alnum:]_]\+\)/dev_attr_\2/'
	'/\<DRIVER_ATTR_\(RW\|RO\|WO\)([[:space:]]*\([[:alnum:]_]\+\)/driver_attr_\2/'
	'/\<\(DEFINE\|DECLARE\)_STATIC_KEY_\(TRUE\|FALSE\)\(\|_RO\)([[:space:]]*\([[:alnum:]_]\+\)/\4/'
	'/^SEQCOUNT_LOCKTYPE(\([^,]*\),[[:space:]]*\([^,]*\),[^)]*)/seqcount_\2_t/'
	'/^SEQCOUNT_LOCKTYPE(\([^,]*\),[[:space:]]*\([^,]*\),[^)]*)/seqcount_\2_init/'
	'/^\<DECLARE_IDTENTRY[[:alnum:]_]*([^,)]*,[[:space:]]*\([[:alnum:]_]\+\)/\1/'
	'/^\<DEFINE_IDTENTRY[[:alnum:]_]*([[:space:]]*\([[:alnum:]_]\+\)/\1/'
	'/^\<DEFINE_FREE(\([[:alnum:]_]\+\)/cleanup_\1/'
	'/^\<DEFINE_CLASS(\([[:alnum:]_]\+\)/class_\1/'
	'/^\<EXTEND_CLASS(\([[:alnum:]_]\+\),[[:space:]]*\([[:alnum:]_]\+\)/class_\1\2/'
	'/^\<DEFINE_GUARD(\([[:alnum:]_]\+\)/class_\1/'
	'/^\<DEFINE_GUARD_COND(\([[:alnum:]_]\+\),[[:space:]]*\([[:alnum:]_]\+\)/class_\1\2/'
	'/^\<DEFINE_LOCK_GUARD_[[:digit:]](\([[:alnum:]_]\+\)/class_\1/'
	'/^\<DEFINE_LOCK_GUARD_[[:digit:]]_COND(\([[:alnum:]_]\+\),[[:space:]]*\([[:alnum:]_]\+\)/class_\1\2/'
	'/^context_lock_struct(\([^,)]*\)[^)]*)/struct \1/'
)
regex_kconfig=(
	'/^[[:blank:]]*\(menu\|\)config[[:blank:]]\+\([[:alnum:]_]\+\)/\2/'
	'/^[[:blank:]]*\(menu\|\)config[[:blank:]]\+\([[:alnum:]_]\+\)/CONFIG_\2/'
)
setup_regex()
{
	local mode=$1 lang tmp=() r
	shift

	regex=()
	for lang; do
		case "$lang" in
		asm)       tmp=("${regex_asm[@]}") ;;
		c)         tmp=("${regex_c[@]}") ;;
		kconfig)   tmp=("${regex_kconfig[@]}") ;;
		esac
		for r in "${tmp[@]}"; do
			if test "$mode" = "exuberant"; then
				regex[${#regex[@]}]="--regex-$lang=${r}b"
			else
				# Remove ctags /kind-spec/
				case "$r" in
				/*/*/?/)
					r=${r%?/}
				esac
				# Prepend ^[^#] unless already anchored
				case "$r" in
				/^*) ;;
				*)
					r="/^[^#]*${r#/}"
				esac
				regex[${#regex[@]}]="--regex=$r"
			fi
		done
	done
}

setup_ctags()
{
	CTAGS_EXTRA="extra"
	if $1 --version 2>&1 | grep -iq universal; then
	    CTAGS_EXTRA="extras"
	fi
	CTAGS_KCONFIG=n
	if $1 --list-languages | grep -iq kconfig; then
		CTAGS_KCONFIG=y
	fi
	export CTAGS_EXTRA CTAGS_KCONFIG
}

exuberant()
{
	if [ -z "$CTAGS_EXTRA" ]; then
		setup_ctags "$1"
	fi
	setup_regex exuberant asm c
	# identifiers to ignore by ctags
	local ign=(
		ACPI_EXPORT_SYMBOL
		DECLARE_BITMAP
		DEFINE_{TRACE,MUTEX,TIMER}
		EXPORT_SYMBOL EXPORT_SYMBOL_GPL
		EXPORT_TRACEPOINT_SYMBOL EXPORT_TRACEPOINT_SYMBOL_GPL
		____cacheline_aligned ____cacheline_aligned_in_smp
		____cacheline_internodealigned_in_smp
		__acquires __aligned __always_inline __always_unused
		__attribute
		__cacheline_aligned __cacheline_aligned_in_smp
		__deprecated
		__exitdata
		__initconst __initdata __initdata_memblock
		__maybe_unused __must_check __must_hold
		__packed __packed2__
		__read_mostly __refdata __releases __ro_after_init
		__used
		const
		static
	)
	all_target_sources | \
	xargs -r $1 -a "${tags_flags[@]}" -I "$(IFS=','; echo "${ign[*]}")" \
	--$CTAGS_EXTRA=+fq --c-kinds=+px --fields=+iaS --langmap=c:+.h \
	"${regex[@]}" || return

	KCONFIG_ARGS=()
	if [ "$CTAGS_KCONFIG" != y ]; then
		setup_regex exuberant kconfig
		KCONFIG_ARGS=(--langdef=kconfig --language-force=kconfig "${regex[@]}")
	fi
	all_kconfigs | xargs -r $1 -a "${tags_flags[@]}" "${KCONFIG_ARGS[@]}"
}

# Call in a subshell so error handling and cleanup stay local to the operation.
setup_tags_tmp()
{
	set -eo pipefail
	tmp=$(mktemp -d .tmp_tags.XXXXXX)
	trap 'rm -rf "$tmp"' EXIT
	trap 'exit 1' HUP INT TERM
}

# Let recursive make schedule directory shards using the caller's jobserver.
# The scan is unconditional so additions and deletions are also noticed.
parallel_tags()
(
	local tmp file
	# Older zstd versions mishandle hidden paths with --output-dir-mirror.
	if ! ${ZSTD:-zstd} --version | awk '
		match($0, /v[0-9]+\.[0-9]+\.[0-9]+/) {
			split(substr($0, RSTART + 1, RLENGTH - 1), v, ".")
			ok = (v[1] * 10000 + v[2] * 100 + v[3] >= 10506)
		}
		END { exit !ok }'; then
		echo "make tags requires zstd 1.5.6 or later" >&2
		exit 1
	fi
	setup_tags_tmp
	# Workers inherit these capabilities instead of probing for each directory.
	setup_ctags ${CTAGS:-ctags}

	{
		# Invalidate caches written before shards were compressed.
		printf '%s\n' 'shard-format=distributed-zstd-v1'
		printf '%s\n' "${CTAGS:-ctags}"
		${CTAGS:-ctags} --version
	} > "$tmp/settings"

	{
		# Include the generator so changes to the cache format rebuild lists.
		cksum "$0"
		all_target_sources | sed 's/^/s /'
		all_kconfigs | sed 's/^/k /'
	} | LC_ALL=C sort -u > "$tmp/inputs"

	# A matching inventory is not sufficient if a cached input list was lost.
	if [ -f .tags.shards ]; then
		while IFS= read -r file; do
			if [ ! -f "${file%.zst}.files" ]; then
				rm -f .tags.inputs
				break
			fi
		done < .tags.shards
	fi

	if ! cmp -s "$tmp/inputs" .tags.inputs ||
	   [ ! -f .tags.shards ] || [ ! -f .tags.dependencies ]; then
		# Publish inputs last: an interrupted update must regenerate the lists.
		rm -f .tags.inputs
		# Resolve source paths once so out-of-tree caches stay in the build tree.
		sed -n 's/^[sk] //p' "$tmp/inputs" | tr '\n' '\0' |
			xargs -0 -r realpath -e -- > "$tmp/paths"
		awk -v tmp="$tmp" -v source_root="$(realpath "${srctree:-.}")/" \
		    -v build_root="$(pwd -P)/" '
		BEGIN {
			printf "" > (tmp "/dependencies")
			printf "" > (tmp "/lists")
		}
		/^[sk] / {
			file = substr($0, 3)
			getline path < (tmp "/paths")
			# Prefer the more specific root when one tree contains the other.
			if (index(path, source_root) == 1 &&
			    (length(source_root) >= length(build_root) ||
			     index(path, build_root) != 1))
				path = substr(path, length(source_root) + 1)
			else if (index(path, build_root) == 1)
				path = substr(path, length(build_root) + 1)
			else
				path = ".tags.external" path
			dir = path
			if (!sub(/\/[^\/]*$/, "", dir))
				dir = "."
			base = (dir == "." ? "" : dir "/") ".tags"
			if (!(dir in ids)) {
				ids[dir] = ++n
				print base ".zst"
				print n, base ".files" > (tmp "/lists")
			}
			out = tmp "/" ids[dir] ".files"
			if (out != previous) {
				if (previous != "")
					close(previous)
				previous = out
			}
			print >> out
			print base ".zst: " file > (tmp "/dependencies")
		}' "$tmp/inputs" | LC_ALL=C sort > "$tmp/shards"
	fi

	# Record all locations before publishing lists so interrupted builds can
	# still prune files left by earlier source selections.
	if [ -f "$tmp/shards" ] || [ ! -f .tags.locations ]; then
		{
			for file in .tags.locations .tags.shards "$tmp/shards"; do
				if [ -f "$file" ]; then
					cat "$file"
				fi
			done
		} | LC_ALL=C sort -u > "$tmp/locations"
		mv "$tmp/locations" .tags.locations
	fi

	if [ -f "$tmp/lists" ]; then
		local id dir
		while read -r id file; do
			dir=${file%/*}
			if [ "$dir" != "$file" ] && [ ! -d "$dir" ]; then
				mkdir -p "$dir"
			fi
			if ! cmp -s "$tmp/$id.files" "$file"; then
				mv "$tmp/$id.files" "$file"
			fi
		done < "$tmp/lists"
	fi
	for file in settings shards dependencies; do
		if [ -f "$tmp/$file" ] && ! cmp -s "$tmp/$file" ".tags.$file"; then
			mv "$tmp/$file" ".tags.$file"
		fi
	done
	if [ ! -f .tags.inputs ]; then
		mv "$tmp/inputs" .tags.inputs
	fi
	${MAKE:-make} -f "${tree}scripts/Makefile.tags"
	if ! cmp -s .tags.locations .tags.shards; then
		LC_ALL=C comm -23 .tags.locations .tags.shards | remove_tag_shards
		cp .tags.shards "$tmp/locations"
		mv "$tmp/locations" .tags.locations
	fi
)

merge_tags()
(
	local tmp
	setup_tags_tmp

	# Use only current shards, excluding directories removed since last run.
	# Expand into the temporary directory; cleanup also covers decode failures.
	xargs -r ${ZSTD:-zstd} -q -d --output-dir-mirror="$tmp" -- < .tags.shards
	# Read filenames from stdin to avoid command-line length limits.
	{
		sed "s|^|$tmp/|; s/\.zst$//" .tags.shards | tr '\n' '\0'
		# sort requires at least one input, even when there are no shards.
		printf '/dev/null\0'
	} | LC_ALL=C sort -m -u --files0-from=- | cut -f2- > "$tmp/merged"
	mv "$tmp/merged" tags
)

# Remove both files belonging to each listed shard, preserving path boundaries.
remove_tag_shards()
{
	while IFS= read -r file; do
		printf '%s\0' "$file" "${file%.zst}.files"
	done | xargs -0 -r rm -f --
}

remove_struct_forward_declarations()
{
	LC_ALL=C sed -e '/^\([a-zA-Z_][a-zA-Z0-9_]*\)\t.*\t\/\^struct \1;.*\$\/;"\tx$/d' "$@"
}

emacs()
{
	setup_regex emacs asm c
	all_target_sources | xargs $1 -a "${regex[@]}"

	setup_regex emacs kconfig
	all_kconfigs | xargs $1 -a "${regex[@]}"
}

xtags()
{
	if $1 --version 2>&1 | grep -iq exuberant; then
		exuberant $1
	elif $1 --version 2>&1 | grep -iq emacs; then
		emacs $1
	else
		all_target_sources | xargs $1 -a
	fi
}

# Support um (which uses SUBARCH)
if [ "${ARCH}" = "um" ]; then
	if [ "$SUBARCH" = "i386" ]; then
		archinclude=x86
	elif [ "$SUBARCH" = "x86_64" ]; then
		archinclude=x86
	else
		archinclude=${SUBARCH}
	fi
fi

remove_structs=
case "$1" in
	"cscope")
		docscope
		;;

	"gtags")
		dogtags
		;;

	"tags")
		# Recursive recipes also run in dry-run, touch and question modes.
		# Only normal invocations may generate intermediate files.
		case ${MAKEFLAGS%% *} in
		*n*) exit 0 ;;
		*t*) touch tags; exit $? ;;
		*q*) exit 1 ;;
		esac
		if ${CTAGS:-ctags} --version 2>&1 | grep -Eiq 'exuberant|universal'; then
			parallel_tags
			exit $?
		else
			rm -f tags
			xtags ${CTAGS:-ctags}
		fi
		remove_structs=y
		;;

	"tags-worker")
		tags_input=${2%.zst}
		# Never expose an incomplete shard after an error or interruption.
		setup_tags_tmp
		tags_flags=(-f "$tmp/tags" --sort=no --tag-relative=no)
		exuberant ${CTAGS:-ctags}
		# Sort once per changed directory. Prefix records so the final merge
		# keeps pseudo-tags ahead of all regular tag names.
		remove_struct_forward_declarations "$tmp/tags" | awk '
			{
				sub(/^!_TAG_FILE_SORTED\t0\t/, "!_TAG_FILE_SORTED\t1\t")
				print (/^!_TAG_/ ? "0\t" : "1\t") $0
			}' | LC_ALL=C sort --parallel=1 -u |
			${ZSTD:-zstd} -q -1 --single-thread -c > "$tmp/sorted.zst"
		mv "$tmp/sorted.zst" "$2"
		exit 0
		;;

	"tags-merge")
		merge_tags
		exit $?
		;;

	"tags-clean")
		set -eo pipefail
		for file in .tags.locations .tags.shards; do
			if [ -f "$file" ]; then
				remove_tag_shards < "$file"
			fi
		done
		rm -f .tags.{inputs,settings,shards,dependencies,locations}
		exit 0
		;;

	"TAGS")
		rm -f TAGS
		xtags etags
		remove_structs=y
		;;
esac

# Remove structure forward declarations.
if [ -n "$remove_structs" ]; then
	remove_struct_forward_declarations -i "$1"
fi
