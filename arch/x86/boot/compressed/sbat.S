/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Embed SBAT data in the kernel.
 */
	.pushsection ".sbat", "a", @progbits
	.incbin "arch/x86/boot/compressed/sbat"
	.balign	0x1000
	.popsection
