/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2002, 2004, 2005 Oracle.  All rights reserved.
 */

#ifndef OCFS2_AOPS_H
#define OCFS2_AOPS_H

#include <linux/fs.h>
#include <linux/iomap.h>

extern const struct iomap_ops ocfs2_iomap_ops;
extern const struct iomap_dio_ops ocfs2_iomap_dio_ops_r_pr;
extern const struct iomap_dio_ops ocfs2_iomap_dio_ops_w_pr;
extern const struct iomap_dio_ops ocfs2_iomap_dio_ops_w_ex;

int ocfs2_map_folio_blocks(struct folio *folio, u64 *p_blkno,
			  struct inode *inode, unsigned int from,
			  unsigned int to, int new);

void ocfs2_unlock_and_free_folios(struct folio **folios, int num_folios);

int walk_page_buffers(	handle_t *handle,
			struct buffer_head *head,
			unsigned from,
			unsigned to,
			int *partial,
			int (*fn)(	handle_t *handle,
					struct buffer_head *bh));

int ocfs2_write_end_nolock(struct address_space *mapping,
			   loff_t pos, unsigned len, unsigned copied, void *fsdata);

typedef enum {
	OCFS2_WRITE_BUFFER = 0,
	OCFS2_WRITE_DIRECT,
	OCFS2_WRITE_MMAP,
} ocfs2_write_type_t;

int ocfs2_write_begin_nolock(struct address_space *mapping,
		loff_t pos, unsigned len, ocfs2_write_type_t type,
		struct folio **foliop, void **fsdata,
		struct buffer_head *di_bh, struct folio *mmap_folio);

int ocfs2_read_inline_data(struct inode *inode, struct folio *folio,
			   struct buffer_head *di_bh);
int ocfs2_size_fits_inline_data(struct buffer_head *di_bh, u64 new_size);

int ocfs2_get_block(struct inode *inode, sector_t iblock,
		    struct buffer_head *bh_result, int create);
int ocfs2_map_blocks(struct inode *inode, struct ocfs2_map_block *map,
		    int flags);

#endif /* OCFS2_FILE_H */
