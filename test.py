import pathlib
import subprocess

from elftools.elf.elffile import ELFFile
import pytest


__here__ = pathlib.Path(__file__).parent


def _run_test(name, arch, defconfig, cross_compile, elf_info=None):
	build_dir = __here__ / 'out' / name
	config = build_dir / '.config'

	args = [
		'make', '-j10', f'O={build_dir}',
		*([f'CROSS_COMPILE={cross_compile}'] if cross_compile else []),
		f'ARCH={arch}',
		defconfig, 'init/', 'usr/include/',
	]

	subprocess.check_call(args)

	if elf_info is None:
		assert 'CONFIG_CC_CAN_LINK' not in config.read_text(), 'invalid CONFIG_CC_CAN_LINK'
	else:
		assert 'CONFIG_CC_CAN_LINK=y' in config.read_text(), 'invalid CONFIG_CC_CAN_LINK'

		with open(build_dir / 'init' / 'userprogs-test', 'rb') as f:
			elf = ELFFile(f)

			assert elf.header.e_machine == elf_info


system_toolchain_testcases = [
	('x86_64', 'x86', 'x86_64_defconfig', None, 'EM_X86_64'),
	('i386', 'x86', 'i386_defconfig', None, 'EM_386'),
	('arm', 'arm', 'defconfig', 'arm-none-eabi-'),
	('arm64', 'arm64', 'defconfig', 'aarch64-linux-gnu-', 'EM_AARCH64'),
	('riscv32', 'riscv', 'rv32_defconfig', 'riscv64-linux-gnu-'),
	('riscv64', 'riscv', 'defconfig', 'riscv64-linux-gnu-', 'EM_RISCV'),
]


@pytest.mark.parametrize('testcase',
			 system_toolchain_testcases,
			 ids=lambda tc: tc[0])
def test_system_toolchain(testcase):
	_run_test(*testcase)


korg_toolchain_testcases = [
	('aarch64-linux', 'arm64', 'defconfig'),
	('alpha-linux', 'alpha', 'defconfig'),
	('arm-linux-gnueabi', 'arm', 'defconfig'),
	('csky-linux', 'csky', 'defconfig'),
#	('hppa-linux', 'parisc', 'defconfig'),
	('i386-linux', 'x86', 'defconfig'),
	('loongarch64-linux', 'loongarch', 'defconfig'),
	('m68k-linux', 'm68k', 'defconfig'),
	('mips-linux', 'mips', 'defconfig'),
	('mips64-linux', 'mips', 'defconfig'),
	('powerpc-linux', 'powerpc', 'pmac32_defconfig'),
	('powerpc64-linux', 'powerpc', 'defconfig'),
	('riscv32-linux', 'riscv', 'defconfig'),
	('riscv64-linux', 'riscv', 'defconfig'),
	('s390-linux', 's390', 'defconfig'),
	('sh4-linux', 'sh', 'defconfig'),
	('sparc-linux', 'sparc', 'defconfig'),
	('sparc64-linux', 'sparc', 'defconfig'),
	('x86_64-linux', 'x86', 'defconfig'),
]


@pytest.mark.parametrize('testcase',
			 korg_toolchain_testcases,
			 ids=lambda tc: tc[0])
def test_korg_toolchain(testcase):
	name, arch, defconfig = testcase

	_run_test(f'korg-{name}', arch, defconfig,
		  f'/home/t-8ch/.cache/crosstools/gcc-13.2.0-nolibc/{name}/bin/{name}-')


bootlin_toolchain_testcases = [
	('arm', 'armv5-eabi--glibc--stable-2025.08-1', 'arm-linux', 'arm', 'defconfig', 'EM_ARM'),
	('mips32el', 'mips32el--glibc--stable-2025.08-1', 'mipsel-linux', 'mips', 'defconfig', None),
	('mips32r6el', 'mips32r6el--glibc--stable-2025.08-1', 'mipsel-linux', 'mips', 'defconfig', None),
	('mips64r6el-n32', 'mips64r6el-n32--glibc--stable-2025.08-1', 'mips64el-linux', 'mips', 'defconfig', None),
	('powerpc64le-power8', 'powerpc64le-power8--glibc--stable-2025.08-1', 'powerpc64le-linux', 'powerpc', 'defconfig', 'EM_PPC64'),
	('sparc32', 'sparcv8--uclibc--stable-2025.08-1', 'sparc-linux', 'sparc', 'sparc32_defconfig', 'EM_SPARC'),
	('sparc64', 'sparc64--glibc--stable-2024.05-1', 'sparc64-linux', 'sparc', 'sparc64_defconfig', 'EM_SPARCV9'),
	('sparc32on64', 'sparc64--glibc--stable-2024.05-1', 'sparc64-linux', 'sparc', 'sparc32_defconfig', None),
	('m68k', 'm68k-68xxx--glibc--stable-2025.08-1', 'm68k-linux', 'm68k', 'defconfig', 'EM_68K'),
	('riscv32', 'riscv32-ilp32d--glibc--stable-2025.08-1', 'riscv32-linux', 'riscv', 'rv32_defconfig', 'EM_RISCV'),
	('riscv32on64', 'riscv64-lp64d--glibc--stable-2025.08-1', 'riscv64-linux', 'riscv', 'rv32_defconfig', None),
	('riscv64', 'riscv64-lp64d--glibc--stable-2025.08-1', 'riscv64-linux', 'riscv', 'defconfig', 'EM_RISCV'),
	('s390', 's390x-z13--glibc--stable-2025.08-1', 's390x-linux', 's390', 'defconfig', 'EM_S390'),
]


@pytest.mark.parametrize('testcase',
			 bootlin_toolchain_testcases,
			 ids=lambda tc: tc[0])
def test_bootlin_toolchain(testcase):
	name, dist_name, triple, arch, defconfig, elf_info = testcase

	_run_test(f'bootlin-{name}', arch, defconfig,
		  f'/home/t-8ch/.cache/crosstools/{dist_name}/bin/{triple}-',
		  elf_info)
