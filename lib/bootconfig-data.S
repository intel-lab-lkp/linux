/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Embed default bootconfig in the kernel.
 */
	.section .init.data, "aw"
	.global embedded_bootconfig_data
embedded_bootconfig_data:
	.incbin "lib/default.bconf"
	.byte 0
	.global embedded_bootconfig_data_end
embedded_bootconfig_data_end:
