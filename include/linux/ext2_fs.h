/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/include/linux/ext2_fs.h
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#ifndef _LINUX_EXT2_FS_H
#define _LINUX_EXT2_FS_H

#include <linux/types.h>
#include <linux/magic.h>

#define EXT2_NAME_LEN 255

/*
 * Maximal count of links to a file
 */
#define EXT2_LINK_MAX		32000

#define EXT2_SB_MAGIC_OFFSET	0x38
#define EXT2_SB_BLOCKS_OFFSET	0x04
#define EXT2_SB_BSIZE_OFFSET	0x18

#endif	/* _LINUX_EXT2_FS_H */
