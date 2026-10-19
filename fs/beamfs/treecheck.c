// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs tree checker -- catch a lost pointer where it is lost.
 *
 * generic/464 reports the damage through fsck, twenty seconds after the
 * fact, as a count of blocks the bitmap calls used and no inode
 * reaches. Working back from that count means reading a million trace
 * lines and guessing which of them mattered.
 *
 * The way out of chasing an intermittent defect is to stop chasing it:
 * check the invariant at every write, and the first violation arrives
 * as a stack trace naming the task, the block and the slot, instead of
 * a number in a log.
 *
 * The invariant: a slot that holds a pointer keeps it until the block
 * it names is freed. Every store into an indirect block is recorded
 * here; every subsequent store to the same slot is checked against the
 * record. A store that finds zero where a live pointer was recorded is
 * the defect, and it is reported at that instant.
 *
 * Cost: one hash lookup per store, and one struct beamfs_tc_entry --
 * 72 bytes on a 64-bit kernel -- per slot recorded. Off unless
 * CONFIG_BEAMFS_DEBUG_TREE, and the calls compile away entirely when it
 * is off.
 */

#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "beamfs.h"

#ifdef CONFIG_BEAMFS_DEBUG_TREE

#define BEAMFS_TC_BITS 14

struct beamfs_tc_entry {
	struct hlist_node node;
	struct hlist_node cnode;   /* on s_tc_child while child != 0 */
	u64 parent;
	u32 slot;
	u64 child;
	unsigned long ino;
	const char *who;
};

/*
 * Keyed on (parent, slot) rather than on the child: the question asked
 * at every store is "what did this slot hold", and a child that has
 * been freed and handed out again would collide under the other key.
 */
static inline u32 beamfs_tc_key(u64 parent, u32 slot)
{
	return hash_64(parent * BEAMFS_INDIRECT_PTRS + slot, BEAMFS_TC_BITS);
}

static inline u32 beamfs_tc_ckey(u64 child)
{
	return hash_64(child, BEAMFS_TC_BITS);
}

/* Under s_tc_lock. An entry is on the child index while it names a block. */
static void beamfs_tc_index_child(struct beamfs_sb_info *sbi,
				  struct beamfs_tc_entry *e)
{
	if (e->child)
		hash_add(sbi->s_tc_child, &e->cnode, beamfs_tc_ckey(e->child));
}

static void beamfs_tc_unindex_child(struct beamfs_tc_entry *e)
{
	if (!hlist_unhashed(&e->cnode))
		hash_del(&e->cnode);
}

/* Under s_tc_lock. */
static void beamfs_tc_drop(struct beamfs_sb_info *sbi, struct beamfs_tc_entry *e)
{
	beamfs_tc_unindex_child(e);
	hash_del(&e->node);
	sbi->s_tc_entries--;
	kfree(e);
}

void beamfs_tc_init(struct beamfs_sb_info *sbi)
{
	hash_init(sbi->s_tc);
	hash_init(sbi->s_tc_child);
	spin_lock_init(&sbi->s_tc_lock);
	sbi->s_tc_entries = 0;
	sbi->s_tc_violations = 0;
}

void beamfs_tc_exit(struct beamfs_sb_info *sbi)
{
	struct beamfs_tc_entry *e;
	struct hlist_node *tmp;
	unsigned int bkt;

	spin_lock(&sbi->s_tc_lock);
	hash_for_each_safe(sbi->s_tc, bkt, tmp, e, node) {
		beamfs_tc_unindex_child(e);
		hash_del(&e->node);
		kfree(e);
	}
	spin_unlock(&sbi->s_tc_lock);

	if (sbi->s_tc_violations)
		pr_err("beamfs/treecheck: %u lost pointer(s) over the life of this mount\n",
		       sbi->s_tc_violations);
	else
		pr_info("beamfs/treecheck: no lost pointer seen\n");
}

/*
 * Record a store, and check what the slot held before it.
 *
 * @old is what the caller read out of the slot. If a live pointer was
 * recorded for this slot and @old is zero, that pointer vanished from
 * the buffer between the two stores with nothing freeing it -- which is
 * the defect, reported here with the task and the stack that found it.
 */
void beamfs_tc_store(struct super_block *sb, unsigned long ino, u64 parent,
		     u32 slot, u64 old, u64 child, const char *who)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e, *found = NULL;
	u32 key = beamfs_tc_key(parent, slot);

	spin_lock(&sbi->s_tc_lock);

	hash_for_each_possible(sbi->s_tc, e, node, key) {
		if (e->parent == parent && e->slot == slot) {
			found = e;
			break;
		}
	}

	if (found && found->child && old == 0) {
		sbi->s_tc_violations++;
		spin_unlock(&sbi->s_tc_lock);
		pr_err("beamfs/treecheck: LOST POINTER parent=%llu slot=%u held %llu (ino=%lu, put there by %s), read as 0, now %llu (ino=%lu, by %s)\n",
		       (unsigned long long)parent, slot,
		       (unsigned long long)found->child, found->ino,
		       found->who ? found->who : "?",
		       (unsigned long long)child, ino, who);
		WARN_ONCE(1, "beamfs: indirect slot lost its pointer\n");
		spin_lock(&sbi->s_tc_lock);
		beamfs_tc_unindex_child(found);
		found->child = child;
		found->ino = ino;
		found->who = who;
		beamfs_tc_index_child(sbi, found);
		spin_unlock(&sbi->s_tc_lock);
		return;
	}

	if (found) {
		beamfs_tc_unindex_child(found);
		found->child = child;
		found->ino = ino;
		found->who = who;
		beamfs_tc_index_child(sbi, found);
		spin_unlock(&sbi->s_tc_lock);
		return;
	}

	spin_unlock(&sbi->s_tc_lock);

	/* GFP_NOFS: this runs inside writeback. */
	e = kmalloc_obj(*e, GFP_NOFS);
	if (!e)
		return;   /* a checker that fails allocation checks less, not wrongly */
	e->parent = parent;
	e->slot = slot;
	e->child = child;
	e->ino = ino;
	e->who = who;
	INIT_HLIST_NODE(&e->cnode);

	spin_lock(&sbi->s_tc_lock);
	hash_add(sbi->s_tc, &e->node, key);
	beamfs_tc_index_child(sbi, e);
	sbi->s_tc_entries++;
	spin_unlock(&sbi->s_tc_lock);
}

/*
 * A freed block's pointer is allowed to disappear.
 *
 * Called from beamfs_free_block. Every slot naming this block is
 * forgotten, so a later store into that slot is not a violation. Without
 * this the checker would report every ordinary truncate.
 */
void beamfs_tc_forget_child(struct super_block *sb, u64 child)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e;
	struct hlist_node *tmp;
	u32 ckey = beamfs_tc_ckey(child);

	spin_lock(&sbi->s_tc_lock);
	hash_for_each_possible_safe(sbi->s_tc_child, e, tmp, cnode, ckey) {
		if (e->child == child)
			beamfs_tc_drop(sbi, e);
	}
	spin_unlock(&sbi->s_tc_lock);
}

/*
 * A freed parent takes its slots with it.
 *
 * The block is about to be handed to somebody else, who will zero it
 * and store into the same slots. Those stores are not violations.
 */
void beamfs_tc_forget_parent(struct super_block *sb, u64 parent)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e;
	struct hlist_node *tmp;
	u32 slot;

	/*
	 * The 512 slots this block could have, each one probe, rather
	 * than the whole table for every freed block.
	 */
	spin_lock(&sbi->s_tc_lock);
	for (slot = 0; slot < BEAMFS_INDIRECT_PTRS; slot++) {
		u32 key = beamfs_tc_key(parent, slot);

		hash_for_each_possible_safe(sbi->s_tc, e, tmp, node, key) {
			if (e->parent == parent && e->slot == slot) {
				beamfs_tc_drop(sbi, e);
				break;
			}
		}
	}
	spin_unlock(&sbi->s_tc_lock);
}

/*
 * A slot deliberately set to zero.
 *
 * Truncate zeroes slots it has freed the children of. Without telling
 * the checker, the next store into that slot reads as a lost pointer.
 */
void beamfs_tc_clear(struct super_block *sb, u64 parent, u32 slot)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e;
	u32 key = beamfs_tc_key(parent, slot);

	spin_lock(&sbi->s_tc_lock);
	hash_for_each_possible(sbi->s_tc, e, node, key) {
		if (e->parent == parent && e->slot == slot) {
			beamfs_tc_unindex_child(e);
			e->child = 0;
			break;
		}
	}
	spin_unlock(&sbi->s_tc_lock);
}

/*
 * A block about to be memset as a fresh indirect block.
 *
 * Every memset in the allocator follows sb_getblk on a block the
 * allocator has just handed out, so the block should hold nothing. If
 * the checker still has live pointers recorded for it, the allocator
 * gave out a block that is in service, and the memset is about to erase
 * a subtree.
 *
 * generic/464 writes with pwrite -b 65536 on a truncated file, so each
 * write rebuilds the whole tree; the lost slots come out at a fixed
 * stride of 17, which is 65536 / 3824 rounded down: the logical blocks
 * one write covers. Slots vanishing in groups at a fixed stride is what a
 * whole block being zeroed looks like, not what a race on one slot
 * looks like.
 */
void beamfs_tc_zeroed(struct super_block *sb, unsigned long ino, u64 parent,
		      const char *who)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e;
	struct hlist_node *tmp;
	unsigned int live = 0;
	u64 first = 0;
	u32 first_slot = 0;
	u32 slot;

	spin_lock(&sbi->s_tc_lock);
	for (slot = 0; slot < BEAMFS_INDIRECT_PTRS; slot++) {
		u32 key = beamfs_tc_key(parent, slot);

		hash_for_each_possible_safe(sbi->s_tc, e, tmp, node, key) {
			if (e->parent != parent || e->slot != slot)
				continue;
			if (e->child && !live) {
				first = e->child;
				first_slot = e->slot;
			}
			if (e->child)
				live++;
			beamfs_tc_drop(sbi, e);
			break;
		}
	}
	spin_unlock(&sbi->s_tc_lock);

	if (live) {
		sbi->s_tc_violations++;
		pr_err("beamfs/treecheck: ZEROED IN SERVICE block=%llu had %u live pointer(s) (slot %u held %llu), zeroed by %s for ino=%lu\n",
		       (unsigned long long)parent, live, first_slot,
		       (unsigned long long)first, who, ino);
		WARN_ONCE(1, "beamfs: an indirect block in service was zeroed\n");
	}
}

#endif /* CONFIG_BEAMFS_DEBUG_TREE */
