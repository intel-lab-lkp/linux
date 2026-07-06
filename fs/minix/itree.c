// SPDX-License-Identifier: GPL-2.0

/*
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Copyright (C) 1996  Gertjan van Wingerde
 *	Minix V2 fs support.
 *
 *  Modified for 680x0 by Andreas Schwab
 *  Updated to filesystem version 3 by Daniel Aragones
 *
 *  Disparate itree_v1.c, itree_v2.c, itree_common.c unified by Jeremy Bingham.
 */

#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "minix.h"

#define DIRCOUNT 7
#define INDIRCOUNT(sb) (1 << ((sb)->s_blocksize_bits - 2))
#define MINIX_V1_BLK 512
#define MINIX_V1_BLK_SHIFT 9

/* The different versions of the Minix filesystem also have different block
 * sizes. In order to unify the itree functions and not have the split that's
 * been in place for decades, we're going to have two separate types for v1 and
 * v2/v3 block sizes. This does require having explicit version checks and
 * casting block pointers to v1_block_t and a few cases where there's a special
 * v1 version of a function that gets called where it's necessary to do it that
 * way.
 *
 * To maintain type safety with this unified approach to the minix itree
 * functions, both the direct and indirect block pointer data and structures
 * need to use accessor functions. This way, the number of places that require
 * conditionally casting block pointers can be kept confined to a few places
 * while the general filesystem logic can be kept in common.
 *
 * NB: Formerly, this module had the itree functions split into three files:
 *     - itree_v1.c, with defines, some inline helper functions, and wrapper
 *       functions tailored to version 1 of the Minix filesystem.
 *     - itree_v2.c, as above but for versions 2 and 3 of the Minix filesystem.
 *     - itree_common.c, an aptly named file that held functions that were
 *       common to both v1 and v2/v3 of the Minix filesystem.
 *
 * The odd part of this was that itree_v1.c and itree_v2.c each had their own
 * definitions of DEPTH - 3 for v1, 4 for v2/v3, and their own special block_t
 * typedefs. Version 1 only has doubly indirect blocks and 16 bit block
 * pointers, while versions 2 and 3 have trebly indirect blocks and 32 bit
 * block pointers. Both itree_v1.c and itree_v2.c then included itree_common.c
 * and provided version specific wrappers for itree_common.c's functions. Code
 * in other files would then call one wrapper or the other depending on the
 * filesystem's version number. All of this was in lieu of just defining the
 * number of direct blocks and levels of indirect blocks in the superblock info
 * struct.
 *
 * While there are more explicit pointer casts here now, this code should
 * hopefully be clearer than having two very similar but subtly different code
 * paths where it is not always obvious what's different.
 *
 * Should anyone need to refer to the itree_v1.c, itree_v2.c, or itree_common.c
 * files, the last master commit before they were merged and altered was
 * 87320be9f0d24fce67631b7eef919f0b79c3e45c.
 */

typedef u32 block_t;
typedef u16 v1_block_t;

typedef struct {
	void	*p;
	block_t	key;
	struct buffer_head *bh;
	u8 is_v1;
} Indirect;

/* Accessor functions for Indirect structs to provide a unified interface for
 * both v1 and v2/v3 filesystems. This way we can keep the number of places we
 * need to dance around the different block sizes to a relative minimum.
 */
static inline block_t indir_get(Indirect *ip)
{
	if (ip->is_v1)
		return *(v1_block_t *)ip->p;
	return *(block_t *)ip->p;
}

static inline void indir_set(Indirect *ip, block_t n)
{
	if (ip->is_v1)
		*(v1_block_t *)ip->p = (v1_block_t)n;
	else
		*(block_t *)ip->p = n;
}

static inline void indir_set_ptr(Indirect *ip, void *base, int offset)
{
	if (ip->is_v1)
		ip->p = (v1_block_t *)base + offset;
	else
		ip->p = (block_t *)base + offset;
}

/* Pointer arithmetic where the poor block pointer is violently coerced into
 * different forms depending on what block pointer size we're working with,
 * which depends on the filesystem version.
 */
static inline block_t *blk_ptr_offset(void *base, int offset, u8 is_v1)
{
	block_t *p;

	if (is_v1)
		p = (block_t *)((v1_block_t *)base + offset);
	else
		p = (block_t *)base + offset;

	return p;
}

static DEFINE_RWLOCK(pointers_lock);

static inline v1_block_t block_to_v1_block(block_t n)
{
	return n;
}

static inline block_t v1_block_to_block(v1_block_t n)
{
	return n;
}

static inline unsigned long block_to_cpu(block_t n)
{
	return n;
}

static inline block_t cpu_to_block(unsigned long n)
{
	return n;
}

/* Use accessor functions for the i_data arrays in the inode info union instead
 * of trying to forcibly coerce the u16 version into a u32 array. It might be
 * a bit clunky, but it's not as fraught with peril as the headache of dealing
 * with the allocated coerced u32 array floating around and potentially leaking
 * accidentally.
 */
static inline block_t *i_data_get(struct inode *inode, int i)
{
	if (INODE_VERSION(inode) == MINIX_V1)
		return (block_t *)(v1_block_t *)&minix_i(inode)->u.i1_data[i];
	return (block_t *)&minix_i(inode)->u.i2_data[i];
}

static inline void i_data_set(struct inode *inode, int i, block_t n)
{
	if (INODE_VERSION(inode) == MINIX_V1)
		minix_i(inode)->u.i1_data[i] = (v1_block_t)n;
	else
		minix_i(inode)->u.i2_data[i] = n;
}

static inline block_t i_data_read(struct inode *inode, int i)
{
	if (INODE_VERSION(inode) == MINIX_V1)
		return minix_i(inode)->u.i1_data[i];
	return minix_i(inode)->u.i2_data[i];
}

static inline int v1_block_to_path(struct inode *inode, long block, int *offsets)
{
	int n = 0;
	struct super_block *sb = inode->i_sb;

	if ((u64)block * BLOCK_SIZE >= sb->s_maxbytes)
		return 0;

	if (block < DIRCOUNT) {
		offsets[n++] = block;
	} else if ((block - DIRCOUNT) < MINIX_V1_BLK) {
		block -= DIRCOUNT;
		offsets[n++] = DIRCOUNT;
		offsets[n++] = block;
	} else {
		block -= MINIX_V1_BLK;
		offsets[n++] = DIRCOUNT + 1;
		offsets[n++] = block>>MINIX_V1_BLK_SHIFT;
		offsets[n++] = block & (MINIX_V1_BLK - 1);
	}
	return n;
}

static inline int v2_block_to_path(struct inode *inode, long block, int *offsets)
{
	int n = 0;
	struct super_block *sb = inode->i_sb;

	if ((u64)block * (u64)sb->s_blocksize >= sb->s_maxbytes)
		return 0;

	if (block < DIRCOUNT) {
		offsets[n++] = block;
	} else if ((block - DIRCOUNT) < INDIRCOUNT(sb)) {
		block -= DIRCOUNT;
		offsets[n++] = DIRCOUNT;
		offsets[n++] = block;
	} else if ((block - INDIRCOUNT(sb)) < INDIRCOUNT(sb) * INDIRCOUNT(sb)) {
		block -= INDIRCOUNT(sb);
		offsets[n++] = DIRCOUNT + 1;
		offsets[n++] = block / INDIRCOUNT(sb);
		offsets[n++] = block % INDIRCOUNT(sb);
	} else {
		block -= INDIRCOUNT(sb) * INDIRCOUNT(sb);
		offsets[n++] = DIRCOUNT + 2;
		offsets[n++] = (block / INDIRCOUNT(sb)) / INDIRCOUNT(sb);
		offsets[n++] = (block / INDIRCOUNT(sb)) % INDIRCOUNT(sb);
		offsets[n++] = block % INDIRCOUNT(sb);
	}
	return n;
}

/* *offsets has DEPTH elements. The exact value of DEPTH depends on whether or
 * not this is a v1 minix fs or v2/v3.
 *
 * This function also unavoidably wraps version specific functions, because
 * they're different enough that completely reconciling the two versions is
 * pointless.
 */
static int block_to_path(struct inode *inode, long block, int *offsets)
{
	int ret = 0;

	if (block < 0) {
		pr_warn("MINIX-fs: %s: block %ld < 0 on dev %pg\n", __func__,
			block, inode->i_sb->s_bdev);
		return ret;
	}

	if (INODE_VERSION(inode) == MINIX_V1)
		ret = v1_block_to_path(inode, block, offsets);
	else
		ret = v2_block_to_path(inode, block, offsets);

	return ret;
}

static inline void add_chain(Indirect *p, struct buffer_head *bh, block_t *v, u8 is_v1)
{
	p->key = (is_v1) ? *(v1_block_t *)(p->p = v) : *(block_t *)(p->p = v);
	p->bh = bh;
	p->is_v1 = is_v1;
}

static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == indir_get(from))
		from++;
	return (from > to);
}

static inline block_t *block_end(struct buffer_head *bh)
{
	return (block_t *)((char *)bh->b_data + bh->b_size);
}

/* chain has DEPTH elements as described elsewhere. */
static inline Indirect *get_branch(struct inode *inode,
					int depth,
					int *offsets,
					Indirect *chain,
					int *err)
{
	struct super_block *sb = inode->i_sb;
	Indirect *p = chain;
	struct buffer_head *bh;
	u8 is_v1 = INODE_VERSION(inode) == MINIX_V1;

	*err = 0;

	add_chain(chain, NULL, i_data_get(inode, *offsets), is_v1);
	if (!p->key)
		goto no_block;
	while (--depth) {
		bh = sb_bread(sb, block_to_cpu(p->key));
		if (!bh)
			goto failure;
		read_lock(&pointers_lock);
		if (!verify_chain(chain, p))
			goto changed;
		add_chain(++p, bh, blk_ptr_offset(bh->b_data, *++offsets, is_v1), is_v1);
		read_unlock(&pointers_lock);
		if (!p->key)
			goto no_block;
	}
	return NULL;

changed:
	read_unlock(&pointers_lock);
	brelse(bh);
	*err = -EAGAIN;
	goto no_block;
failure:
	*err = -EIO;
no_block:
	return p;
}

static int alloc_branch(struct inode *inode,
			     int num,
			     int *offsets,
			     Indirect *branch)
{
	int n = 0;
	int i;
	int parent = minix_new_block(inode);
	int err = -ENOSPC;
	u8 is_v1 = INODE_VERSION(inode) == MINIX_V1;

	/* set is_v1 to be extra safe. Belt, suspenders, etc. */
	branch[0].is_v1 = is_v1;
	branch[0].key = cpu_to_block(parent);
	if (parent)
		for (n = 1; n < num; n++) {
			struct buffer_head *bh;
			/* Allocate the next block */
			int nr = minix_new_block(inode);

			if (!nr)
				break;

			branch[n].is_v1 = is_v1;
			branch[n].key = cpu_to_block(nr);
			bh = sb_getblk(inode->i_sb, parent);

			if (!bh) {
				minix_free_block(inode, nr);
				err = -ENOMEM;
				break;
			}

			lock_buffer(bh);
			memset(bh->b_data, 0, bh->b_size);
			branch[n].bh = bh;
			indir_set_ptr(&branch[n], bh->b_data, offsets[n]);
			indir_set(&branch[n], branch[n].key);
			set_buffer_uptodate(bh);
			unlock_buffer(bh);
			mmb_mark_buffer_dirty(bh,
				&minix_i(inode)->i_metadata_bhs);
			parent = nr;
		}
	if (n == num)
		return 0;

	/* Allocation failed, free what we already allocated */
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < n; i++)
		minix_free_block(inode, block_to_cpu(branch[i].key));
	return err;
}

/* same business with chain as before */
static inline int splice_branch(struct inode *inode,
				     Indirect *chain,
				     Indirect *where,
				     int num)
{
	int i;

	write_lock(&pointers_lock);

	/* Verify that place we are splicing to is still there and vacant */
	if (!verify_chain(chain, where-1) || indir_get(where))
		goto changed;

	indir_set(where, where->key);

	write_unlock(&pointers_lock);

	/* We are done with atomic stuff, now do the rest of housekeeping */

	inode_set_ctime_current(inode);

	/* had we spliced it onto indirect block? */
	if (where->bh)
		mmb_mark_buffer_dirty(where->bh,
				      &minix_i(inode)->i_metadata_bhs);

	mark_inode_dirty(inode);
	return 0;

changed:
	write_unlock(&pointers_lock);
	for (i = 1; i < num; i++)
		bforget(where[i].bh);
	for (i = 0; i < num; i++)
		minix_free_block(inode, block_to_cpu(where[i].key));
	return -EAGAIN;
}

/* offsets and chain have DEPTH elements. Since DEPTH varies between versions of
 * the minix filesystems, instead of having two different definitions of DEPTH
 * and two different versions of this function just allocate the offsets and
 * chain arrays dynamically. This does require remembering to free them.
 */
int minix_get_block(struct inode *inode, sector_t block,
			struct buffer_head *bh, int create)
{
	struct super_block *sb = inode->i_sb;
	struct minix_sb_info *sbi = minix_sb(sb);
	u8 s_depth = sbi->s_depth;
	int err = -EIO;
	int *offsets = kmalloc_array(s_depth, sizeof(int), GFP_KERNEL);
	Indirect *chain = kmalloc_array(s_depth, sizeof(Indirect), GFP_KERNEL);

	if (offsets == NULL || chain == NULL) {
		err = -ENOMEM;
		if (offsets != NULL)
			kfree(offsets);
		if (chain != NULL)
			kfree(chain);
		goto out;
	}

	Indirect *partial;
	int left;
	int depth = block_to_path(inode, block, offsets);

	if (depth == 0)
		goto out;

reread:
	partial = get_branch(inode, depth, offsets, chain, &err);

	/* Simplest case - block found, no allocation needed */
	if (!partial) {
got_it:
		map_bh(bh, inode->i_sb, block_to_cpu(chain[depth-1].key));
		/* Clean up and exit */
		partial = chain+depth-1; /* the whole chain */
		goto cleanup;
	}

	/* Next simple case - plain lookup or failed read of indirect block */
	if (!create || err == -EIO) {
cleanup:
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}

		kfree(offsets);
		kfree(chain);
out:
		return err;
	}

	/*
	 * Indirect block might be removed by truncate while we were
	 * reading it. Handling of that case (forget what we've got and
	 * reread) is taken out of the main path.
	 */
	if (err == -EAGAIN)
		goto changed;

	left = (chain + depth) - partial;
	err = alloc_branch(inode, left, offsets+(partial-chain), partial);
	if (err)
		goto cleanup;

	if (splice_branch(inode, chain, partial, left) < 0)
		goto changed;

	set_buffer_new(bh);
	goto got_it;

changed:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	goto reread;
}

static inline int all_zeroes(Indirect *b, block_t *p, block_t *q)
{
	v1_block_t *pv1;
	v1_block_t *qv1;

	if (b->is_v1) {
		pv1 = (v1_block_t *)p;
		qv1 = (v1_block_t *)q;
		while (pv1 < qv1)
			if (*pv1++)
				return 0;
	} else {
		while (p < q)
			if (*p++)
				return 0;
	}

	return 1;
}

/* offset and chains, etc. etc. */
static Indirect *find_shared(struct inode *inode,
				int depth,
				int *offsets,
				Indirect *chain,
				block_t *top)
{
	Indirect *partial, *p;
	int k, err;

	*top = 0;
	for (k = depth; k > 1 && !offsets[k-1]; k--)
		;
	partial = get_branch(inode, k, offsets, chain, &err);

	write_lock(&pointers_lock);
	if (!partial)
		partial = chain + k-1;
	if (!partial->key && indir_get(partial)) {
		write_unlock(&pointers_lock);
		goto no_top;
	}
	for (p = partial; p > chain && all_zeroes(p, (block_t *)p->bh->b_data,
			(block_t *)p->p); p--)
		;
	if (p == chain + k - 1 && p > chain) {
		indir_set_ptr(p, p->p, -1);
	} else {
		*top = indir_get(p);
		indir_set(p, 0);
	}
	write_unlock(&pointers_lock);

	while (partial > p) {
		brelse(partial->bh);
		partial--;
	}
no_top:
	return partial;
}

static inline void v1_free_data(struct inode *inode, v1_block_t *p,
		v1_block_t *q)
{
	unsigned long nr;

	for ( ; p < q; p++) {
		nr = block_to_cpu(v1_block_to_block(*p));
		if (nr) {
			*p = 0;
			minix_free_block(inode, nr);
		}
	}
}
static inline void free_data(struct inode *inode, block_t *p, block_t *q)
{
	unsigned long nr;

	/* Take the easy way out if we're minix v1. */
	if (INODE_VERSION(inode) == MINIX_V1) {
		v1_free_data(inode, (v1_block_t *)p, (v1_block_t *)q);
		return;
	}

	for ( ; p < q ; p++) {
		nr = block_to_cpu(*p);
		if (nr) {
			*p = 0;
			minix_free_block(inode, nr);
		}
	}
}

/* Once again, the easy way out may well be the best. */
static void v1_free_branches(struct inode *inode, v1_block_t *p,
	v1_block_t *q, int depth)
{
	struct buffer_head *bh;
	unsigned long nr;

	/* At least in this function we can go straight to the v1 functions. */
	if (depth--) {
		for ( ; p < q ; p++) {
			nr = block_to_cpu(v1_block_to_block(*p));
			if (!nr)
				continue;
			*p = 0;
			bh = sb_bread(inode->i_sb, nr);
			if (!bh)
				continue;
			v1_free_branches(inode, (v1_block_t *)bh->b_data,
				      (v1_block_t *)block_end(bh), depth);
			bforget(bh);
			minix_free_block(inode, nr);
			mark_inode_dirty(inode);
		}
	} else
		v1_free_data(inode, p, q);
}

static void free_branches(struct inode *inode, block_t *p, block_t *q,
	int depth)
{
	struct buffer_head *bh;
	unsigned long nr;

	if (INODE_VERSION(inode) == MINIX_V1) {
		v1_free_branches(inode, (v1_block_t *)p, (v1_block_t *)q,
				depth);
		return;
	}

	if (depth--) {
		for ( ; p < q ; p++) {
			nr = block_to_cpu(*p);
			if (!nr)
				continue;
			*p = 0;
			bh = sb_bread(inode->i_sb, nr);
			if (!bh)
				continue;
			free_branches(inode, (block_t *)bh->b_data,
				      block_end(bh), depth);
			bforget(bh);
			minix_free_block(inode, nr);
			mark_inode_dirty(inode);
		}
	} else
		free_data(inode, p, q);
}

/* The function called for file truncation. *offsets and *chain have DEPTH
 * elements; the value of DEPTH depends on the version of the minix filesystem
 * being worked with. Dynamically allocate and free offsets and chain with the
 * appropriate DEPTH value for the filesystem in question.
 */
void minix_truncate(struct inode *inode)
{
	/* Instead of leaving this as an internal function wrapped by
	 * minix_truncate in inode.c, just do the inode type check here before
	 * we define and set any variables.
	 */
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
		return;

	struct super_block *sb = inode->i_sb;
	struct minix_sb_info *sbi = minix_sb(sb);
	u8 direct = sbi->s_direct;
	u8 depth = sbi->s_depth;
	int *offsets = kmalloc_array(depth, sizeof(int), GFP_KERNEL);
	Indirect *chain = kmalloc_array(depth, sizeof(Indirect), GFP_KERNEL);
	Indirect *partial;
	block_t nr = 0;
	int n;
	int first_whole;
	long iblock;

	/* check for nullness, bail if anything failed. */
	if (offsets == NULL || chain == NULL)
		goto do_free;

	iblock = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	block_truncate_page(inode->i_mapping, inode->i_size, minix_get_block);

	n = block_to_path(inode, iblock, offsets);
	if (!n)
		goto do_free;

	if (n == 1) {
		free_data(inode, i_data_get(inode, offsets[0]),
			i_data_get(inode, direct));
		first_whole = 0;
		goto do_indirects;
	}

	first_whole = offsets[0] + 1 - direct;
	partial = find_shared(inode, n, offsets, chain, &nr);
	if (nr) {
		if (partial == chain)
			mark_inode_dirty(inode);
		else
			mmb_mark_buffer_dirty(partial->bh,
					      &minix_i(inode)->i_metadata_bhs);
		free_branches(inode, &nr, &nr+1, (chain+n-1) - partial);
	}
	/* Clear the ends of indirect blocks on the shared branch */
	while (partial > chain) {
		free_branches(inode, blk_ptr_offset(partial->p, 1,
			partial->is_v1), block_end(partial->bh),
				(chain+n-1) - partial);
		mmb_mark_buffer_dirty(partial->bh,
				      &minix_i(inode)->i_metadata_bhs);
		brelse(partial->bh);
		partial--;
	}
do_indirects:
	/* Kill the remaining (whole) subtrees */
	while (first_whole < depth-1) {
		nr = i_data_read(inode, direct+first_whole);
		if (nr) {
			i_data_set(inode, direct+first_whole, 0);
			mark_inode_dirty(inode);
			free_branches(inode, &nr, &nr+1, first_whole+1);
		}
		first_whole++;
	}
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

do_free:
	if (offsets != NULL)
		kfree(offsets);
	if (chain != NULL)
		kfree(chain);
}

unsigned int minix_blocks(loff_t size, struct super_block *sb)
{
	struct minix_sb_info *sbi = minix_sb(sb);
	int k = sb->s_blocksize_bits - 10;
	unsigned int blocks, res;
	unsigned int direct = sbi->s_direct;
	unsigned int i = sbi->s_depth;

	blocks = (size + sb->s_blocksize - 1) >> (BLOCK_SIZE_BITS + k);
	res = blocks;
	size_t bsize = (sbi->s_version == MINIX_V1) ? sizeof(v1_block_t) :
			sizeof(block_t);

	while (--i && blocks > direct) {
		blocks -= direct;
		blocks += sb->s_blocksize/bsize - 1;
		blocks /= sb->s_blocksize/bsize;
		res += blocks;
		direct = 1;
	}

	return res;
}
