#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Script to check commits for UAPI backwards compatibility

set -o errexit
set -o pipefail

print_usage() {
	name=$(basename "$0")
	cat << EOF
$name - check for module parameters stability across git commits.

By default, the script will check to make sure the latest commit (or current
dirty changes) did not introduce changes when compared to HEAD^1. You can
check against additional commit ranges with the -b and -p options.

Usage: $name [-b BASE_REF] [-p PAST_REF] [-j N] [-l ERROR_LOG] [-q] [-v]

Options:
    -b BASE_REF    Base git reference to use for comparison. If unspecified or empty,
                   will use any dirty changes in tree to UAPI files. If there are no
                   dirty changes, HEAD will be used.
    -p PAST_REF    Compare BASE_REF to PAST_REF (e.g. -p v6.1). If unspecified or empty,
                   will use BASE_REF^1. Must be an ancestor of BASE_REF. Only headers
                   that exist on PAST_REF will be checked for compatibility.
    -j JOBS        Number of checks to run in parallel (default: number of CPU cores).
    -l ERROR_LOG   Write error log to file (default: no error log is generated).
    -q             Quiet operation (suppress stdout, still print stderr).
    -v             Verbose operation (print more information about each header being checked).

Exit codes:
    $SUCCESS) Success
    $FAILURE) Module param differences detected
EOF
}

readonly SUCCESS=0
readonly FAILURE=1

# Print to stderr
eprintf() {
	# shellcheck disable=SC2059
	printf "$@" >&2
}

# Check if git tree is dirty
tree_is_dirty() {
	! git diff --quiet
}

file_module_params_unmodified() {
	local file="$1"
	local base_ref="$2"
	local past_ref="$3"
	local base_params_file="${TMP_DIR}/${file}.base"
	local past_params_file="${TMP_DIR}/${file}.past"
	local error_log="${TMP_DIR}/${file}.error"

	local -r awk_cmd='/^ *module_param.*\(/,/.*\);/'

	mkdir -p "$(dirname "$error_log")"
	git show "${past_ref}:${file}" 2> /dev/null \
		| awk "$awk_cmd" > "$past_params_file" || true

	# Ignore files that don't exist at the past ref or don't have module params
	if [ ! -s "$past_params_file" ]; then
		return 255 # Special return code for "no-op"
	fi

	if [ -z "$base_ref" ]; then
		awk "$awk_cmd" "${KERNEL_SRC}/${file}" \
			> "$base_params_file" 2> /dev/null || true
	else
		git show "${base_ref}:${file}" 2> /dev/null \
			| awk "$awk_cmd" > "$base_params_file" || true
	fi

	# Process the param data to come up with an associative array of param names to param data
	# For example:
	#   module_param_call(foo, set_result, get_result, NULL, 0600);
	#
	# is processed into:
	#   pre_change_params[foo]="set_result,get_result,NULL,0600"
	local -A pre_change_params
	local param_name
	local param_params
	while read -r mod_param_args; do
		param_name="$(echo "$mod_param_args" | cut -d ',' -f 1)"
		param_params="$(echo "$mod_param_args" | cut -d ',' -f 2-)"

		pre_change_params[$param_name]=$param_params
	done < <(tr -d '\t\n ' < "$past_params_file" | tr ';' '\n' | grep -o '(.*)' | tr -d '()')

	local -A post_change_params
	while read -r mod_param_args; do
		param_name="$(echo "$mod_param_args" | cut -d ',' -f 1)"
		param_params="$(echo "$mod_param_args" | cut -d ',' -f 2-)"

		post_change_params[$param_name]=$param_params
	done < <(tr -d '\t\n ' < "$base_params_file" | tr ';' '\n' | grep -o '(.*)' | tr -d '()')

	# Flag any module param changes that:
	#  - Remove/rename a parameter
	#  - Change the arguments of the parameter
	local incompat_param_changes=0
	local pre
	local post
	for param_name in "${!pre_change_params[@]}"; do
		pre="${pre_change_params[$param_name]}"
		if [ ! "${post_change_params[$param_name]+set}" ]; then
			{
				printf "Module parameter \"%s\" in %s removed!\n" "$param_name" "$file"
				printf "  Original args: %s\n" "$pre"
			} > "$error_log"
			incompat_param_changes=$((incompat_param_changes + 1))
			continue
		fi

		post="${post_change_params[$param_name]}"
		if [ "$pre" != "$post" ]; then
			{
				printf "Module parameter \"%s\" in %s changed!\n" "$param_name" "$file"
				printf "  Original args: %s\n" "$pre"
				printf "       New args: %s\n" "$post"
			} > "$error_log"
			incompat_param_changes=$((incompat_param_changes + 1))
			continue
		fi
	done

	if [ "$incompat_param_changes" -gt 0 ]; then
		return 1
	fi
}

run() {
	local base_ref="$1"
	local past_ref="$2"
	local abi_error_log="$3"

	diff_args=("$past_ref")
	if [ -n "$base_ref" ]; then
		diff_args+=("$base_ref")
	fi

	local -a threads=()
	local passed=0
	local failed=0
	printf "Checking files between %s and %s for module parameter compatibility...\n" \
		"$past_ref" "$base_ref"
	while read -r modified_file; do
		if [ "${#threads[@]}" -ge "$MAX_THREADS" ]; then
			wait "${threads[0]}" && ret="$?" || ret="$?"
			if [ "$ret" -eq 0 ]; then
				passed=$((passed + 1))
			elif [ "$ret" -eq 1 ]; then
				failed=$((failed + 1))
			fi
			threads=("${threads[@]:1}")
		fi

		file_module_params_unmodified "$modified_file" "$base_ref" "$past_ref" &
		threads+=("$!")
	done < <(git diff --diff-filter=MCD --name-only "${diff_args[@]}" -- '*.c' '*.h')

	for t in "${threads[@]}"; do
		wait "$t" && ret="$?" || ret="$?"
		if [ "$ret" -eq 0 ]; then
			passed=$((passed + 1))
		elif [ "$ret" -eq 1 ]; then
			failed=$((failed + 1))
		fi
	done

	total=$((passed + failed))
	if [ "$total" -eq 0 ]; then
		printf "No files with module parameters modified between %s and %s\n" \
			"$past_ref" "${base_ref:-dirty tree}"
		exit 0
	fi

	if [ -n "$abi_error_log" ]; then
		printf 'Generated by "%s %s" from git ref %s\n\n' \
			"$0" "$*" "$(git rev-parse HEAD)" > "$abi_error_log"
	fi

	while read -r error_file; do
		{
			cat "$error_file"
			printf "\n\n"
		} | tee -a "${abi_error_log:-/dev/null}" >&2
	done < <(find "$TMP_DIR" -type f -name '*.error')

	if [ "$failed" -gt 0 ]; then
		eprintf "error - %d/%d files with modules parameters appear _not_ to be backwards compatible\n" \
			"$failed" "$total"
		if [ -n "$abi_error_log" ]; then
			eprintf "Failure summary saved to %s\n" "$abi_error_log"
		fi
	else
		printf "All %d files with module_parameters checked appear to be backwards compatible\n" \
			"$total" "$ARCH"
	fi

	exit "$failed"
}

# Make sure the git refs we have make sense
check_refs() {
	if ! git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
		eprintf "error - this script requires the kernel tree to be initialized with Git\n"
		return 1
	fi

	if ! git rev-parse --verify "$past_ref" > /dev/null 2>&1; then
		printf 'error - invalid git reference "%s"\n' "$past_ref"
		return 1
	fi

	if [ -n "$base_ref" ]; then
		if ! git merge-base --is-ancestor "$past_ref" "$base_ref" > /dev/null 2>&1; then
			printf 'error - "%s" is not an ancestor of base ref "%s"\n' "$past_ref" "$base_ref"
			return 1
		fi
		if [ "$(git rev-parse "$base_ref")" = "$(git rev-parse "$past_ref")" ]; then
			printf 'error - "%s" and "%s" are the same reference\n' "$past_ref" "$base_ref"
			return 1
		fi
	fi
}

main() {
	MAX_THREADS=$(nproc)
	quiet="false"
	local base_ref=""
	while getopts "hb:p:j:l:q" opt; do
		case $opt in
		h)
			print_usage
			exit "$SUCCESS"
			;;
		b)
			base_ref="$OPTARG"
			;;
		p)
			past_ref="$OPTARG"
			;;
		j)
			MAX_THREADS="$OPTARG"
			;;
		l)
			abi_error_log="$OPTARG"
			;;
		q)
			quiet="true"
			;;
		*)
			exit "$FAIL_PREREQ"
		esac
	done

	if [ "$quiet" = "true" ]; then
		exec > /dev/null 2>&1
	fi

	if [ -z "$KERNEL_SRC" ]; then
		KERNEL_SRC="$(realpath "$(dirname "$0")"/..)"
	fi

	cd "$KERNEL_SRC"

	if [ -z "$base_ref" ] && ! tree_is_dirty; then
		base_ref=HEAD
	fi

	if [ -z "$past_ref" ]; then
		if [ -n "$base_ref" ]; then
			past_ref="${base_ref}^1"
		else
			past_ref=HEAD
		fi
	fi

	if ! check_refs; then
		exit "$FAIL_PREREQ"
	fi

	TMP_DIR=$(mktemp -d)
	readonly TMP_DIR
	trap 'rm -rf "$TMP_DIR"' EXIT

	run "$base_ref" "$past_ref" "$abi_error_log"
}

main "$@"
