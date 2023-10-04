/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Inline kernel configuration for debuginfo files
 *
 * Copyright (c) 2023, Oracle and/or its affiliates.
 */

/*
 * By using the same "IKCFG_ST" and "IKCFG_ED" markers found in configs.c, we
 * can ensure that the resulting debuginfo files can be read by
 * scripts/extract-ikconfig. Unfortunately, this means that the contents of the
 * section cannot be directly extracted and used. Since debuggers should be able
 * to trim these markers off trivially, this is a good tradeoff.
 */
	.section .debug_linux_ikconfig
	.ascii "IKCFG_ST"
	.incbin "kernel/config_data.gz"
	.ascii "IKCFG_ED"
