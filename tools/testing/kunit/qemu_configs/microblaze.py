# SPDX-License-Identifier: GPL-2.0-only
from ..qemu_config import QemuArchParams

QEMU_ARCH = QemuArchParams(linux_arch='microblaze',
						   kconfig='''
CONFIG_CPU_BIG_ENDIAN=y
CONFIG_SERIAL_UARTLITE=y
CONFIG_SERIAL_UARTLITE_CONSOLE=y
CONFIG_MB_POWER_OFF_THROUGH_UNALIGNED_PC=y
''',
						   qemu_arch='microblaze',
						   kernel_path='arch/microblaze/boot/linux.bin',
						   kernel_command_line='kunit_shutdown=poweroff',
						   extra_qemu_params=[
							'-M', 'petalogix-s3adsp1800',
						   ],
)
