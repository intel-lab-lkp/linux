/* SPDX-License-Identifier: GPL-2.0-or-later */
/* memdump.h - Live Dump's memory dumping management
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

#ifndef _LIVEDUMP_MEMDUMP_H
#define _LIVEDUMP_MEMDUMP_H

#include <linux/fs.h>

extern int livedump_memdump_init(const char *bdevpath);

extern void livedump_memdump_uninit(void);

extern void livedump_memdump_handle_page(unsigned long pfn, unsigned long addr, int for_sweep);

extern void livedump_memdump_sm_init(void);

extern void livedump_memdump_write_elf_hdr(void);

#endif /* _LIVEDUMP_MEMDUMP_H */
