/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wrprortect.h - Kernel space write protection support
 * Copyright (C) 2012 Hitachi, Ltd.
 * Copyright (C) 2023 SUSE
 * Author: YOSHIDA Masanori <masanori.yoshida.tv@hitachi.com>
 * Author: Lukas Hruska <lhruska@suse.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _WRPROTECT_H
#define _WRPROTECT_H

#include <linux/mm.h>		/* PAGE_SIZE */

typedef void (*fn_handle_page_t)(unsigned long pfn, unsigned long addr, int for_sweep);
typedef void (*fn_sm_init_t)(void);

extern int wrprotect_init(
		fn_handle_page_t fn_handle_page,
		fn_sm_init_t fn_sm_init);
extern void wrprotect_uninit(void);
extern int wrprotect_start(void);
extern int wrprotect_sweep(void);
extern void wrprotect_unselect_pages(
		unsigned long start,
		unsigned long len);
extern int wrprotect_page_fault_handler(unsigned long error_code);

extern int wrprotect_is_on;

#endif /* _WRPROTECT_H */
