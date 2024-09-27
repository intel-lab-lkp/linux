#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e

crosstool_version=13.2.0
hostarch=x86_64
nproc=$(( $(nproc) + 2))
cache_dir="${XDG_CACHE_HOME:-"$HOME"/.cache}"
download_location="${cache_dir}/crosstools/"
build_location="$(realpath "${cache_dir}"/nolibc-tests/)"
perform_download=0
test_mode=system
CFLAGS_EXTRA="-Werror"
archs="i386 x86_64 arm arm-nommu arm64 arm64-p64k ppc ppc64 m68k mips mips64 riscv riscv-nommu riscv32 riscv32-nommu s390 loongarch um um32 sparc32 sparc64"

if [[ -n "$*" ]]; then
	archs="$*"
fi

karch() {
	case "$1" in
	x86_64) echo "x86";;
	ppc*) echo "powerpc";;
	mips*) echo "mips";;
	riscv*) echo "riscv";;
	um*) echo x86;;
	arm64*) echo arm64;;
	arm*) echo arm;;
	sparc*) echo sparc;;
	*) echo "$1";;
	esac
}

crosstool_arch() {
	case "$1" in
	arm64*) echo aarch64;;
	arm*) echo arm;;
	ppc) echo powerpc;;
	ppc64) echo powerpc64;;
	ppc64le) echo powerpc64;;
	riscv32) echo riscv32;;
	riscv*) echo riscv64;;
	loongarch) echo loongarch64;;
	mips*) echo mips;;
	um*) echo x86_64;;
	sparc*) echo sparc64;;
	*) echo "$1";;
	esac
}

crosstool_abi() {
	case "$1" in
	arm64*) echo linux;;
	arm*) echo linux-gnueabi;;
	*) echo linux;;
	esac
}

download_crosstool() {
	arch="$(crosstool_arch "$1")"
	abi="$(crosstool_abi "$1")"

	archive_name="${hostarch}-gcc-${crosstool_version}-nolibc-${arch}-${abi}.tar.gz"
	url="https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/${hostarch}/${crosstool_version}/${archive_name}"
	archive="${download_location}${archive_name}"
	stamp="${archive}.stamp"

	[ -f "${stamp}" ] && return

	echo "Downloading crosstools ${arch} ${crosstool_version}"
	mkdir -p "${download_location}"
	curl -o "${archive}" --fail --continue-at - "${url}"
	tar -C "${download_location}" -xf "${archive}"
	touch "${stamp}"
}

build_arch() {
	arch=$1
	karch="$(karch "$arch")"
	ct_arch=$(crosstool_arch "$arch")
	ct_abi=$(crosstool_abi "$1")
	cross_compile=$(realpath "${download_location}gcc-${crosstool_version}-nolibc/${ct_arch}-${ct_abi}/bin/${ct_arch}-${ct_abi}-")
	build_dir="out/${arch}"
	MAKE=(make -j"${nproc}" ARCH="${karch}" CROSS_COMPILE="${cross_compile}" O="${build_dir}")

	if [[ $arch == arm64* ]]; then
		MAKE+=(CROSS_COMPILE_COMPAT="${download_location}gcc-${crosstool_version}-nolibc/arm-linux-gnueabi/bin/arm-linux-gnueabi-")
	fi

	if [ "$arch" = "um" ]; then
		MAKE+=(CROSS_COMPILE="")
	fi

	mkdir -p "$build_dir"
	KCONFIG_ALLCONFIG="configs/$arch.config" "${MAKE[@]}" allnoconfig
	./scripts/diffconfig -v "configs/$arch.config" out/$arch/.config

	echo ${MAKE[@]}
	"${MAKE[@]}" -k vmlinux
}

for arch in $archs; do
	download_crosstool "$arch"
done

for arch in $archs; do
	build_arch "$arch"
done
