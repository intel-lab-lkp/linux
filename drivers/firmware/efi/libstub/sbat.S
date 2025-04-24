/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Embed SBAT data in the kernel.
 */
	.pushsection ".sbat","a",@progbits
	.incbin "drivers/firmware/efi/libstub/sbat.bin"
	.popsection
