#!/bin/bash

set -e

filter=example
llvm=0

crosstool_triple() {
	case "$1" in
	arm) echo arm-linux-gnueabi;;
	arm64) echo aarch64-linux;;
	powerpc*) echo powerpc64-linux;;
	riscv) echo riscv64-linux;;
	loongarch) echo loongarch64-linux;;
	sh) echo sh4-linux;;
	*) echo "$1-linux";;
	esac
}

TEMP=$(getopt -o 'l' -n "$0" -- "$@")

eval set -- "$TEMP"
unset TEMP

while true; do
	case "$1" in
		'-l')
			llvm=1
			shift; continue ;;
		'--')
			shift; break ;;
		*)
			echo 'Internal error!' >&2; exit 1 ;;
	esac
done

non_llvm_architectures="alpha m68k powerpc powerpc32 powerpcle sh sparc sparc64"
architectures="$*"
if [ -z "$architectures" ]; then
	architectures="$(tools/testing/kunit/kunit.py run --arch help)"
	if [[ "$llvm" = 1 ]]; then
		for nla in $non_llvm_architectures; do
			architectures="${architectures/${nla}}"
		done
	fi
fi

options="--kunitconfig lib/kunit"

for arch in $architectures; do
	ct_triple=$(crosstool_triple $arch)
	build_dir=".kunit_${arch}"
	opts="$options"

	if [[ "$llvm" = 1 ]]; then
		build_dir="${build_dir}_llvm"
		compile_opts="--make_options LLVM=1"
	elif [[ "$arch" != "um" ]]; then
		compile_opts="--cross_compile $HOME/.cache/crosstools/gcc-13.2.0-nolibc/$ct_triple/bin/$ct_triple-"
	fi

	./tools/testing/kunit/kunit.py run --build_dir $build_dir --arch $arch $compile_opts $options $filter
done
