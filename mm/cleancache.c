// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cleancache.h>
#include <linux/exportfs.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "cleancache_sysfs.h"

/*
 * Lock nesting:
 *	ccinode->folios.xa_lock
 *		fs->hash_lock
 *
 *	ccinode->folios.xa_lock
 *		pool->lock
 *
 *	ccinode->folios.xa_lock
 *		lru_lock
 *
 *	ccinode->folios.xa_lock
 *		lru_lock
 *			pool->lock
 */

#define INODE_HASH_BITS		6

/* represents each file system instance hosted by the cleancache */
struct cleancache_fs {
	refcount_t ref_count;
	DECLARE_HASHTABLE(inode_hash, INODE_HASH_BITS);
	spinlock_t hash_lock; /* protects inode_hash */
	struct rcu_head rcu;
};

/*
 * @cleancache_inode represents each ccinode in @cleancache_fs
 *
 * The cleancache_inode will be freed by RCU when the last folio from xarray
 * is freed, except for invalidate_inode() case.
 */
struct cleancache_inode {
	struct inode *inode;
	struct hlist_node hash;
	refcount_t ref_count;
	struct xarray folios;
	struct cleancache_fs *fs;
	struct rcu_head rcu;
};

/* Cleancache backend memory pool */
struct cleancache_pool {
	struct list_head folio_list;
	spinlock_t lock; /* protects folio_list */
	char *name;
	struct cleancache_pool_stats *stats;
};

#define CLEANCACHE_MAX_POOLS	64

static DEFINE_XARRAY_ALLOC(fs_xa);
static struct kmem_cache *slab_inode; /* cleancache_inode slab */
static struct cleancache_pool pools[CLEANCACHE_MAX_POOLS];
static atomic_t nr_pools = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(pools_lock); /* protects pools */
static LIST_HEAD(cleancache_lru);
static DEFINE_SPINLOCK(lru_lock); /* protects cleancache_lru */

static inline void init_cleancache_folio(struct folio *folio, int pool_id)
{
	/* Folio is being donated and has no refs. No locking is needed. */
	VM_BUG_ON(folio_ref_count(folio) != 0);

	folio->cc_pool_id = pool_id;
	folio->cc_inode = NULL;
	folio->cc_index = 0;
}

static inline void clear_cleancache_folio(struct folio *folio)
{
	/* Folio must be detached and not in the pool. No locking is needed. */
	VM_BUG_ON(folio->cc_inode);
	VM_BUG_ON(!list_empty(&folio->lru));

	folio->cc_pool_id = -1;
}

static inline int folio_pool_id(struct folio *folio)
{
	return folio->cc_pool_id;
}

static inline struct cleancache_pool *folio_pool(struct folio *folio)
{
	return &pools[folio_pool_id(folio)];
}

static void attach_folio(struct folio *folio, struct cleancache_inode *ccinode,
			 pgoff_t offset)
{
	lockdep_assert_held(&(folio_pool(folio)->lock));

	folio->cc_inode = ccinode;
	folio->cc_index = offset;
	cleancache_pool_stat_inc(folio_pool(folio)->stats, POOL_CACHED);
}

static void detach_folio(struct folio *folio)
{
	lockdep_assert_held(&(folio_pool(folio)->lock));

	folio->cc_inode = NULL;
	folio->cc_index = 0;
	cleancache_pool_stat_dec(folio_pool(folio)->stats, POOL_CACHED);
}

static void folio_attachment(struct folio *folio,
			     struct cleancache_inode **ccinode, pgoff_t *offset)
{
	lockdep_assert_held(&(folio_pool(folio)->lock));

	*ccinode = folio->cc_inode;
	*offset = folio->cc_index;
}

static inline bool is_folio_attached(struct folio *folio)
{
	lockdep_assert_held(&(folio_pool(folio)->lock));

	return folio->cc_inode != NULL;
}

/*
 * Folio pool helpers.
 *	Only detached folios are stored in the pool->folio_list.
 *	Once a folio gets attached, it's placed on the cleancache LRU list.
 *
 * Locking:
 *	pool->folio_list is accessed under pool->lock.
 */
static void add_folio_to_pool(struct folio *folio, struct cleancache_pool *pool)
{
	lockdep_assert_held(&pool->lock);
	VM_BUG_ON(folio_pool(folio) != pool);
	VM_BUG_ON(!list_empty(&folio->lru));
	VM_BUG_ON(is_folio_attached(folio));

	list_add(&folio->lru, &pool->folio_list);
}

static struct folio *remove_folio_from_pool(struct folio *folio, struct cleancache_pool *pool)
{
	lockdep_assert_held(&pool->lock);
	VM_BUG_ON(folio_pool(folio) != pool);

	if (is_folio_attached(folio))
		return NULL;

	list_del_init(&folio->lru);

	return folio;
}

static struct folio *pick_folio_from_any_pool(void)
{
	struct cleancache_pool *pool;
	struct folio *folio = NULL;
	int count;

	/* nr_pools can only increase, so the following loop is safe */
	count = atomic_read_acquire(&nr_pools);
	for (int i = 0; i < count; i++) {
		pool = &pools[i];
		spin_lock(&pool->lock);
		if (!list_empty(&pool->folio_list)) {
			folio = list_last_entry(&pool->folio_list,
						struct folio, lru);
			WARN_ON(!remove_folio_from_pool(folio, pool));
			spin_unlock(&pool->lock);
			break;
		}
		spin_unlock(&pool->lock);
	}

	return folio;
}

/* Folio LRU helpers. Only attached folios are stored in the cleancache_lru. */
static void add_folio_to_lru(struct folio *folio)
{
	VM_BUG_ON(!list_empty(&folio->lru));

	spin_lock(&lru_lock);
	list_add(&folio->lru, &cleancache_lru);
	spin_unlock(&lru_lock);
}

static void rotate_lru_folio(struct folio *folio)
{
	spin_lock(&lru_lock);
	if (!list_empty(&folio->lru))
		list_move(&folio->lru, &cleancache_lru);
	spin_unlock(&lru_lock);
}

static void delete_folio_from_lru(struct folio *folio)
{
	spin_lock(&lru_lock);
	if (!list_empty(&folio->lru))
		list_del_init(&folio->lru);
	spin_unlock(&lru_lock);
}

/* FS helpers */
static struct cleancache_fs *get_fs(int fs_id)
{
	struct cleancache_fs *fs;

	rcu_read_lock();
	fs = xa_load(&fs_xa, fs_id);
	if (fs && !refcount_inc_not_zero(&fs->ref_count))
		fs = NULL;
	rcu_read_unlock();

	return fs;
}

static unsigned int invalidate_inode(struct cleancache_fs *fs,
				     struct inode *inode);

static void put_fs(struct cleancache_fs *fs)
{
	if (refcount_dec_and_test(&fs->ref_count)) {
		struct cleancache_inode *ccinode;
		struct hlist_node *tmp;
		int cursor;

		/*
		 * There are no concurrent RCU walkers because they
		 * would have taken fs reference.
		 * We don't need to hold fs->hash_lock because there
		 * are no other users and no way to reach fs.
		 */
		hash_for_each_safe(fs->inode_hash, cursor, tmp, ccinode, hash)
			invalidate_inode(fs, ccinode->inode);
		/*
		 * Don't need to synchronize_rcu() and wait for all inodes to be
		 * freed because RCU read walkers can't take fs refcount anymore
		 * to start their walk.
		 */
		kfree_rcu(fs, rcu);
	}
}

/* cleancache_inode helpers. */
static struct cleancache_inode *alloc_cleancache_inode(struct cleancache_fs *fs,
						       struct inode *inode)
{
	struct cleancache_inode *ccinode;

	ccinode = kmem_cache_alloc(slab_inode, GFP_ATOMIC|__GFP_NOWARN);
	if (ccinode) {
		ccinode->inode = inode;
		xa_init_flags(&ccinode->folios, XA_FLAGS_LOCK_IRQ);
		INIT_HLIST_NODE(&ccinode->hash);
		ccinode->fs = fs;
		refcount_set(&ccinode->ref_count, 1);
	}

	return ccinode;
}

static void inode_free_rcu(struct rcu_head *rcu)
{
	struct cleancache_inode *ccinode;

	ccinode = container_of(rcu, struct cleancache_inode, rcu);
	VM_BUG_ON(!xa_empty(&ccinode->folios));
	kmem_cache_free(slab_inode, ccinode);
}

static inline bool get_inode(struct cleancache_inode *ccinode)
{
	return refcount_inc_not_zero(&ccinode->ref_count);
}

static void put_inode(struct cleancache_inode *ccinode)
{
	VM_BUG_ON(refcount_read(&ccinode->ref_count) == 0);
	if (!refcount_dec_and_test(&ccinode->ref_count))
		return;

	lockdep_assert_not_held(&ccinode->folios.xa_lock);
	VM_BUG_ON(!xa_empty(&ccinode->folios));
	call_rcu(&ccinode->rcu, inode_free_rcu);
}

static void remove_inode_if_empty(struct cleancache_inode *ccinode)
{
	struct cleancache_fs *fs = ccinode->fs;

	lockdep_assert_held(&ccinode->folios.xa_lock);

	if (!xa_empty(&ccinode->folios))
		return;

	spin_lock(&fs->hash_lock);
	hlist_del_init_rcu(&ccinode->hash);
	spin_unlock(&fs->hash_lock);
	/*
	 * Drop the refcount set in alloc_cleancache_inode(). Caller should
	 * have taken an extra refcount to keep ccinode valid, so ccinode
	 * will be freed once the caller releases it.
	 */
	put_inode(ccinode);
}

static bool store_folio_in_inode(struct cleancache_inode *ccinode,
				 pgoff_t offset, struct folio *folio)
{
	struct cleancache_pool *pool = folio_pool(folio);
	int err;

	lockdep_assert_held(&ccinode->folios.xa_lock);
	VM_BUG_ON(!list_empty(&folio->lru));

	spin_lock(&pool->lock);
	err = xa_err(__xa_store(&ccinode->folios, offset, folio,
				GFP_ATOMIC|__GFP_NOWARN));
	if (!err)
		attach_folio(folio, ccinode, offset);
	spin_unlock(&pool->lock);

	return err == 0;
}

static void erase_folio_from_inode(struct cleancache_inode *ccinode,
				   unsigned long offset, struct folio *folio)
{
	bool removed;

	lockdep_assert_held(&ccinode->folios.xa_lock);

	removed = __xa_erase(&ccinode->folios, offset);
	VM_BUG_ON(!removed);
	delete_folio_from_lru(folio);
	remove_inode_if_empty(ccinode);
}

static void move_folio_from_inode_to_pool(struct cleancache_inode *ccinode,
					  unsigned long offset, struct folio *folio)
{
	struct cleancache_pool *pool = folio_pool(folio);

	erase_folio_from_inode(ccinode, offset, folio);
	spin_lock(&pool->lock);
	detach_folio(folio);
	add_folio_to_pool(folio, pool);
	spin_unlock(&pool->lock);
}

static bool isolate_folio_from_inode(struct cleancache_inode *ccinode,
				     unsigned long offset, struct folio *folio)
{
	bool isolated = false;

	xa_lock(&ccinode->folios);
	if (xa_load(&ccinode->folios, offset) == folio) {
		struct cleancache_pool *pool = folio_pool(folio);

		erase_folio_from_inode(ccinode, offset, folio);
		spin_lock(&pool->lock);
		detach_folio(folio);
		spin_unlock(&pool->lock);
		isolated = true;
	}
	xa_unlock(&ccinode->folios);

	return isolated;
}

static unsigned int erase_folios_from_inode(struct cleancache_inode *ccinode,
					    struct xa_state *xas)
{
	unsigned int ret = 0;
	struct folio *folio;

	lockdep_assert_held(&ccinode->folios.xa_lock);

	xas_for_each(xas, folio, ULONG_MAX) {
		move_folio_from_inode_to_pool(ccinode, xas->xa_index, folio);
		ret++;
	}

	return ret;
}

static struct cleancache_inode *find_and_get_inode(struct cleancache_fs *fs,
						   struct inode *inode)
{
	struct cleancache_inode *ccinode = NULL;
	struct cleancache_inode *tmp;

	rcu_read_lock();
	hash_for_each_possible_rcu(fs->inode_hash, tmp, hash, inode->i_ino) {
		if (tmp->inode != inode)
			continue;

		if (get_inode(tmp)) {
			ccinode = tmp;
			break;
		}
	}
	rcu_read_unlock();

	return ccinode;
}

static struct cleancache_inode *add_and_get_inode(struct cleancache_fs *fs,
						  struct inode *inode)
{
	struct cleancache_inode *ccinode, *tmp;

	ccinode = alloc_cleancache_inode(fs, inode);
	if (!ccinode)
		return ERR_PTR(-ENOMEM);

	spin_lock(&fs->hash_lock);
	tmp = find_and_get_inode(fs, inode);
	if (tmp) {
		spin_unlock(&fs->hash_lock);
		/* someone already added it */
		put_inode(ccinode);
		put_inode(tmp);
		return ERR_PTR(-EEXIST);
	}
	hash_add_rcu(fs->inode_hash, &ccinode->hash, inode->i_ino);
	get_inode(ccinode);
	spin_unlock(&fs->hash_lock);

	return ccinode;
}

static struct folio *reclaim_folio_from_lru(void)
{
	struct cleancache_inode *ccinode;
	struct folio *folio;
	pgoff_t offset;

again:
	spin_lock(&lru_lock);
	if (list_empty(&cleancache_lru)) {
		spin_unlock(&lru_lock);
		return NULL;
	}
	ccinode = NULL;
	/* Get the ccinode of the folio at the LRU tail */
	list_for_each_entry_reverse(folio, &cleancache_lru, lru) {
		struct cleancache_pool *pool = folio_pool(folio);

		/* Find and get ccinode */
		spin_lock(&pool->lock);
		folio_attachment(folio, &ccinode, &offset);
		if (ccinode && !get_inode(ccinode))
			ccinode = NULL;
		spin_unlock(&pool->lock);
		if (ccinode)
			break;
	}
	spin_unlock(&lru_lock);

	if (!ccinode)
		return NULL; /* No ccinode to reclaim */

	if (!isolate_folio_from_inode(ccinode, offset, folio)) {
		/* Retry if the folio got erased from the ccinode */
		put_inode(ccinode);
		goto again;
	}

	put_inode(ccinode);

	return folio;
}

static void copy_folio_content(struct folio *from, struct folio *to)
{
	void *src = kmap_local_folio(from, 0);
	void *dst = kmap_local_folio(to, 0);

	memcpy(dst, src, PAGE_SIZE);
	kunmap_local(dst);
	kunmap_local(src);
}

/*
 * We want to store only workingset folios in the cleancache to increase hit
 * ratio so there are four cases:
 *
 * @folio is workingset but cleancache doesn't have it: use new cleancache folio
 * @folio is workingset and cleancache has it: overwrite the stale data
 * @folio is !workingset and cleancache doesn't have it: just bail out
 * @folio is !workingset and cleancache has it: remove the stale @folio
 */
static bool store_into_inode(struct cleancache_fs *fs,
			     struct inode *inode,
			     pgoff_t offset, struct folio *folio)
{
	bool workingset = folio_test_workingset(folio);
	struct cleancache_inode *ccinode;
	struct folio *stored_folio;
	bool new_inode = false;
	bool ret = false;

find_inode:
	ccinode = find_and_get_inode(fs, inode);
	if (!ccinode) {
		if (!workingset)
			goto out;

		ccinode = add_and_get_inode(fs, inode);
		if (IS_ERR_OR_NULL(ccinode)) {
			/*
			 * Retry if someone just added new ccinode from under us.
			 */
			if (PTR_ERR(ccinode) == -EEXIST)
				goto find_inode;

			return false;
		}
		new_inode = true;
	}

	xa_lock(&ccinode->folios);
	stored_folio = xa_load(&ccinode->folios, offset);
	if (stored_folio) {
		if (!workingset) {
			move_folio_from_inode_to_pool(ccinode, offset, stored_folio);
			cleancache_stat_inc(RECLAIMED);
			goto out_unlock;
		}
		rotate_lru_folio(stored_folio);
	} else {
		if (!workingset)
			goto out_unlock;

		stored_folio = pick_folio_from_any_pool();
		if (!stored_folio) {
			/* No free folios, try reclaiming */
			xa_unlock(&ccinode->folios);
			stored_folio = reclaim_folio_from_lru();
			xa_lock(&ccinode->folios);
			if (!stored_folio)
				goto out_unlock;

			cleancache_stat_inc(RECLAIMED);
		}

		if (!store_folio_in_inode(ccinode, offset, stored_folio)) {
			struct cleancache_pool *pool = folio_pool(stored_folio);

			/* Return stored_folio back into pool */
			spin_lock(&pool->lock);
			add_folio_to_pool(stored_folio, pool);
			spin_unlock(&pool->lock);
			goto out_unlock;
		}
		cleancache_stat_inc(STORED);
		add_folio_to_lru(stored_folio);
	}
	copy_folio_content(folio, stored_folio);

	ret = true;
out_unlock:
	/* Free ccinode if it was created but no folio was stored in it. */
	if (new_inode)
		remove_inode_if_empty(ccinode);
	xa_unlock(&ccinode->folios);
	put_inode(ccinode);
out:
	cleancache_stat_inc(SKIPPED);

	return ret;
}

static bool load_from_inode(struct cleancache_fs *fs,
			    struct inode *inode,
			    pgoff_t offset, struct folio *folio)
{
	struct cleancache_inode *ccinode;
	struct folio *stored_folio;

	ccinode = find_and_get_inode(fs, inode);
	if (!ccinode) {
		cleancache_stat_inc(MISSED);
		return false;
	}

	xa_lock(&ccinode->folios);
	stored_folio = xa_load(&ccinode->folios, offset);
	if (stored_folio) {
		rotate_lru_folio(stored_folio);
		copy_folio_content(stored_folio, folio);
		cleancache_stat_inc(RESTORED);
	} else {
		cleancache_stat_inc(MISSED);
	}
	xa_unlock(&ccinode->folios);
	put_inode(ccinode);

	return !!stored_folio;
}

static bool invalidate_folio(struct cleancache_fs *fs,
			     struct inode *inode, pgoff_t offset)
{
	struct cleancache_inode *ccinode;
	struct folio *folio;

	ccinode = find_and_get_inode(fs, inode);
	if (!ccinode)
		return false;

	xa_lock(&ccinode->folios);
	folio = xa_load(&ccinode->folios, offset);
	if (folio) {
		move_folio_from_inode_to_pool(ccinode, offset, folio);
		cleancache_stat_inc(INVALIDATED);
	}
	xa_unlock(&ccinode->folios);
	put_inode(ccinode);

	return folio != NULL;
}

static unsigned int invalidate_inode(struct cleancache_fs *fs,
				     struct inode *inode)
{
	struct cleancache_inode *ccinode;
	unsigned int ret;

	ccinode = find_and_get_inode(fs, inode);
	if (ccinode) {
		XA_STATE(xas, &ccinode->folios, 0);

		xas_lock(&xas);
		ret = erase_folios_from_inode(ccinode, &xas);
		xas_unlock(&xas);
		put_inode(ccinode);
		cleancache_stat_add(INVALIDATED, ret);

		return ret;
	}

	return 0;
}

/* Sysfs helpers */
#ifdef CONFIG_CLEANCACHE_SYSFS

static struct kobject *kobj_sysfs_root;

static void __init cleancache_sysfs_init(void)
{
	struct cleancache_pool *pool;
	int pool_id, pool_count;
	struct kobject *kobj;

	kobj = cleancache_sysfs_create_root();
	if (IS_ERR(kobj)) {
		pr_warn("Failed to create cleancache sysfs root\n");
		return;
	}

	kobj_sysfs_root = kobj;
	if (!kobj_sysfs_root)
		return;

	pool_count = atomic_read(&nr_pools);
	pool = &pools[0];
	for (pool_id = 0; pool_id < pool_count; pool_id++, pool++)
		if (cleancache_sysfs_create_pool(kobj_sysfs_root, pool->stats, pool->name))
			pr_warn("Failed to create sysfs nodes for \'%s\' cleancache backend\n",
				pool->name);
}

static void cleancache_sysfs_pool_init(struct cleancache_pool_stats *pool_stats,
				       const char *name)
{
	/* Skip if sysfs was not initialized yet. */
	if (!kobj_sysfs_root)
		return;

	if (cleancache_sysfs_create_pool(kobj_sysfs_root, pool_stats, name))
		pr_warn("Failed to create sysfs nodes for \'%s\' cleancache backend\n",
			name);
}

#else /* CONFIG_CLEANCACHE_SYSFS */
static inline void cleancache_sysfs_init(void) {}
static inline void cleancache_sysfs_pool_init(struct cleancache_pool_stats *pool_stats,
					      const char *name) {}
#endif /* CONFIG_CLEANCACHE_SYSFS */

/* Hooks into MM and FS */
int cleancache_add_fs(struct super_block *sb)
{
	struct cleancache_fs *fs;
	int fs_id;
	int ret;

	fs = kzalloc(sizeof(struct cleancache_fs), GFP_KERNEL);
	if (!fs) {
		sb->cleancache_id = CLEANCACHE_ID_INVALID;
		return -ENOMEM;
	}

	spin_lock_init(&fs->hash_lock);
	hash_init(fs->inode_hash);
	refcount_set(&fs->ref_count, 1);
	ret = xa_alloc(&fs_xa, &fs_id, fs, xa_limit_32b, GFP_KERNEL);
	if (ret) {
		if (ret == -EBUSY)
			pr_warn("too many file systems\n");

		sb->cleancache_id = CLEANCACHE_ID_INVALID;
		kfree(fs);
	} else {
		sb->cleancache_id = fs_id;
	}

	return ret;
}

void cleancache_remove_fs(struct super_block *sb)
{
	int fs_id = sb->cleancache_id;
	struct cleancache_fs *fs;

	sb->cleancache_id = CLEANCACHE_ID_INVALID;
	fs = get_fs(fs_id);
	if (!fs)
		return;

	xa_erase(&fs_xa, fs_id);
	put_fs(fs);

	/* free the object */
	put_fs(fs);
}

bool cleancache_store_folio(struct inode *inode, struct folio *folio)
{
	struct cleancache_fs *fs;
	int fs_id;
	bool ret;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (!inode)
		return false;

	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return false;

	fs_id = folio->mapping->host->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return false;

	fs = get_fs(fs_id);
	if (!fs)
		return false;

	ret = store_into_inode(fs, inode, folio->index, folio);
	put_fs(fs);

	return ret;
}

bool cleancache_restore_folio(struct inode *inode, struct folio *folio)
{
	struct cleancache_fs *fs;
	int fs_id;
	bool ret;

	if (!inode)
		return false;

	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return false;

	fs_id = folio->mapping->host->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return false;

	fs = get_fs(fs_id);
	if (!fs)
		return false;

	ret = load_from_inode(fs, inode, folio->index, folio);
	put_fs(fs);

	return ret;
}

bool cleancache_invalidate_folio(struct inode *inode, struct folio *folio)
{
	struct cleancache_fs *fs;
	int fs_id;
	bool ret;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (!inode)
		return false;

	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return false;

	/* Careful, folio->mapping can be NULL */
	fs_id = inode->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return false;

	fs = get_fs(fs_id);
	if (!fs)
		return false;

	ret = invalidate_folio(fs, inode, folio->index);
	put_fs(fs);

	return ret;
}

bool cleancache_invalidate_inode(struct inode *inode)
{
	struct cleancache_fs *fs;
	unsigned int count;
	int fs_id;

	if (!inode)
		return false;

	fs_id = inode->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return false;

	fs = get_fs(fs_id);
	if (!fs)
		return false;

	count = invalidate_inode(fs, inode);
	put_fs(fs);

	return count > 0;
}

struct cleancache_inode *
cleancache_start_inode_walk(struct inode *inode, unsigned long count)
{
	struct cleancache_inode *ccinode;
	struct cleancache_fs *fs;
	int fs_id;

	if (!inode)
		return ERR_PTR(-EINVAL);

	fs_id = inode->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return ERR_PTR(-EINVAL);

	fs = get_fs(fs_id);
	if (!fs)
		return NULL;

	ccinode = find_and_get_inode(fs, inode);
	if (!ccinode) {
		put_fs(fs);
		cleancache_stat_add(MISSED, count);
		return NULL;
	}

	return ccinode;
}

void cleancache_end_inode_walk(struct cleancache_inode *ccinode)
{
	struct cleancache_fs *fs = ccinode->fs;

	put_inode(ccinode);
	put_fs(fs);
}

bool cleancache_restore_from_inode(struct cleancache_inode *ccinode,
				   struct folio *folio)
{
	struct folio *stored_folio;
	void *src, *dst;
	bool ret = false;

	xa_lock(&ccinode->folios);
	stored_folio = xa_load(&ccinode->folios, folio->index);
	if (stored_folio) {
		rotate_lru_folio(stored_folio);
		src = kmap_local_folio(stored_folio, 0);
		dst = kmap_local_folio(folio, 0);
		memcpy(dst, src, PAGE_SIZE);
		kunmap_local(dst);
		kunmap_local(src);
		cleancache_stat_inc(RESTORED);
		ret = true;
	} else {
		cleancache_stat_inc(MISSED);
	}
	xa_unlock(&ccinode->folios);

	return ret;
}

/* Backend API */
/*
 * Register a new backend and add its folios for cleancache to use.
 * Returns pool id on success or a negative error code on failure.
 */
int cleancache_backend_register_pool(const char *name)
{
	struct cleancache_pool_stats *pool_stats;
	struct cleancache_pool *pool;
	char *pool_name;
	int pool_id;

	if (!name)
		return -EINVAL;

	pool_name = kstrdup(name, GFP_KERNEL);
	if (!pool_name)
		return -ENOMEM;

	/* pools_lock prevents concurrent registrations */
	spin_lock(&pools_lock);
	pool_id = atomic_read(&nr_pools);
	if (pool_id >= CLEANCACHE_MAX_POOLS) {
		spin_unlock(&pools_lock);
		return -ENOMEM;
	}

	pool = &pools[pool_id];
	INIT_LIST_HEAD(&pool->folio_list);
	spin_lock_init(&pool->lock);
	pool->name = pool_name;
	/* Ensure above stores complete before we increase the count */
	atomic_set_release(&nr_pools, pool_id + 1);
	spin_unlock(&pools_lock);

	pool_stats = cleancache_create_pool_stats(pool_id);
	if (!IS_ERR(pool_stats)) {
		pool->stats = pool_stats;
		cleancache_sysfs_pool_init(pool_stats, pool->name);
	} else {
		pr_warn("Failed to create pool stats for \'%s\' cleancache backend\n",
			pool->name);
	}

	pr_info("Registered \'%s\' cleancache backend, pool id %d\n",
		name, pool_id);

	return pool_id;
}
EXPORT_SYMBOL(cleancache_backend_register_pool);

int cleancache_backend_get_folio(int pool_id, struct folio *folio)
{
	struct cleancache_inode *ccinode;
	struct cleancache_pool *pool;
	pgoff_t offset;

	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return -EOPNOTSUPP;

	/* Does the folio belong to the requesting backend */
	if (folio_pool_id(folio) != pool_id)
		return -EINVAL;

	pool = &pools[pool_id];
again:
	spin_lock(&pool->lock);

	/* If folio is free in the pool, return it */
	if (remove_folio_from_pool(folio, pool)) {
		spin_unlock(&pool->lock);
		goto out;
	}
	/*
	 * The folio is not free, therefore it has to belong
	 * to a valid ccinode.
	 */
	folio_attachment(folio, &ccinode, &offset);
	if (WARN_ON(!ccinode || !get_inode(ccinode))) {
		spin_unlock(&pool->lock);
		return -EINVAL;
	}

	spin_unlock(&pool->lock);

	/* Retry if the folio got erased from the ccinode */
	if (!isolate_folio_from_inode(ccinode, offset, folio)) {
		put_inode(ccinode);
		goto again;
	}

	cleancache_stat_inc(RECALLED);
	cleancache_pool_stat_inc(folio_pool(folio)->stats, POOL_RECALLED);
	put_inode(ccinode);
out:
	VM_BUG_ON_FOLIO(folio_ref_count(folio) != 0, (folio));
	clear_cleancache_folio(folio);
	cleancache_pool_stat_dec(pool->stats, POOL_SIZE);

	return 0;
}
EXPORT_SYMBOL(cleancache_backend_get_folio);

int cleancache_backend_put_folio(int pool_id, struct folio *folio)
{
	struct cleancache_pool *pool = &pools[pool_id];

	/* Do not support large folios yet */
	VM_BUG_ON_FOLIO(folio_test_large(folio), folio);

	/* Can't put a still used folio into cleancache */
	if (folio_ref_count(folio) != 0)
		return -EINVAL;

	/* Reset struct folio fields */
	init_cleancache_folio(folio, pool_id);
	INIT_LIST_HEAD(&folio->lru);
	spin_lock(&pool->lock);
	add_folio_to_pool(folio, pool);
	cleancache_pool_stat_inc(pool->stats, POOL_SIZE);
	spin_unlock(&pool->lock);

	return 0;
}
EXPORT_SYMBOL(cleancache_backend_put_folio);

int cleancache_backend_put_folios(int pool_id, struct list_head *folios)
{
	struct cleancache_pool *pool = &pools[pool_id];
	LIST_HEAD(unused_folios);
	struct folio *folio;
	struct folio *tmp;
	int count = 0;

	list_for_each_entry_safe(folio, tmp, folios, lru) {
		/* Do not support large folios yet */
		VM_BUG_ON_FOLIO(folio_test_large(folio), folio);
		if (folio_ref_count(folio) != 0)
			continue;

		init_cleancache_folio(folio, pool_id);
		list_move(&folio->lru, &unused_folios);
		count++;
	}

	spin_lock(&pool->lock);
	list_splice_init(&unused_folios, &pool->folio_list);
	cleancache_pool_stat_add(pool->stats, POOL_SIZE, count);
	spin_unlock(&pool->lock);

	return list_empty(folios) ? 0 : -EINVAL;
}
EXPORT_SYMBOL(cleancache_backend_put_folios);

static int __init init_cleancache(void)
{
	slab_inode = KMEM_CACHE(cleancache_inode, 0);
	if (!slab_inode)
		return -ENOMEM;

	cleancache_sysfs_init();

	return 0;
}
subsys_initcall(init_cleancache);
