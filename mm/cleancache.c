// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cleancache frontend
 *
 */

#include <linux/cleancache.h>
#include <linux/debugfs.h>
#include <linux/exportfs.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

/*
 * Possible lock nesting:
 * inode->pages.xa_lock
 *	free_folios_lock
 *
 * inode->pages.xa_lock
 *	fs->hash_lock
 *
 * Notes: should keep free_folios_lock and fs->hash_lock HARDIRQ-irq-safe
 * since inode->pages.xa_lock is HARDIRQ-irq-safe and we take these locks
 * while holding inode->pages.xa_lock. This means whenever we take these
 * locks while not holding inode->pages.xa_lock, we should disable irqs.
 */

/* Counters available via /sys/kernel/debug/cleancache */
static u64 cleancache_hits;
static u64 cleancache_misses;
static u64 cleancache_stores;
static u64 cleancache_failed_stores;
static u64 cleancache_invalidates;

/*
 * @cleancache_inode represents each inode in @cleancache_fs
 *
 * The cleancache_inode will be freed by RCU when the last page from xarray
 * is freed, except for invalidate_inode() case.
 */
struct cleancache_inode {
	struct cleancache_filekey key;
	struct hlist_node hash;
	refcount_t ref_count;

	struct xarray pages;
	struct rcu_head rcu;
	struct cleancache_fs *fs;
};

static struct kmem_cache *slab_inode;

#define INODE_HASH_BITS		10

/* represents each file system instance hosted by the cleancache */
struct cleancache_fs {
	spinlock_t hash_lock;
	DECLARE_HASHTABLE(inode_hash, INODE_HASH_BITS);
	refcount_t ref_count;
};

static DEFINE_IDR(fs_idr);
static DEFINE_SPINLOCK(fs_lock);

/* Cleancache backend memory pool */
struct cleancache_pool {
	struct list_head free_folios;
	spinlock_t free_folios_lock;
};

#define CLEANCACHE_MAX_POOLS	64

static struct cleancache_pool pools[CLEANCACHE_MAX_POOLS];
static atomic_t nr_pools = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(pools_lock);

/*
 * If the filesystem uses exportable filehandles, use the filehandle as
 * the key, else use the inode number.
 */
struct cleancache_filekey *cleancache_get_key(struct inode *inode,
					      struct cleancache_filekey *key)
{
	int (*fhfn)(struct inode *inode, __u32 *fh, int *max_len, struct inode *parent);
	int len = 0, maxlen = CLEANCACHE_KEY_MAX;
	struct super_block *sb = inode->i_sb;

	key->u.ino = inode->i_ino;
	if (sb->s_export_op != NULL) {
		fhfn = sb->s_export_op->encode_fh;
		if  (fhfn) {
			len = (*fhfn)(inode, &key->u.fh[0], &maxlen, NULL);
			if (len <= FILEID_ROOT || len == FILEID_INVALID)
				return NULL;
			if (maxlen > CLEANCACHE_KEY_MAX)
				return NULL;
		}
	}
	return key;
}

/* page attribute helpers */
static inline void set_page_pool_id(struct page *page, int id)
{
	page->page_type = id;
}

static inline int page_pool_id(struct page *page)
{
	return page->page_type;
}

static inline struct cleancache_pool *page_pool(struct page *page)
{
	return &pools[page_pool_id(page)];
}

/* Can be used only when page is isolated */
static inline void __SetPageCCacheFree(struct page *page)
{
	SetPagePrivate(page);
}

static inline void SetPageCCacheFree(struct page *page)
{
	lockdep_assert_held(&(page_pool(page)->free_folios_lock));
	__SetPageCCacheFree(page);
}

static inline void ClearPageCCacheFree(struct page *page)
{
	lockdep_assert_held(&(page_pool(page)->free_folios_lock));
	ClearPagePrivate(page);
}

static inline int PageCCacheFree(struct page *page)
{
	lockdep_assert_held(&(page_pool(page)->free_folios_lock));
	return PagePrivate(page);
}

/* Can be used only when page is isolated */
static void __set_page_inode_offs(struct page *page,
				  struct cleancache_inode *inode,
				  unsigned long index)
{
	page->mapping = (struct address_space *)inode;
	page->index = index;
}

static void set_page_inode_offs(struct page *page, struct cleancache_inode *inode,
				unsigned long index)
{
	lockdep_assert_held(&(page_pool(page)->free_folios_lock));

	__set_page_inode_offs(page, inode, index);
}

static void page_inode_offs(struct page *page, struct cleancache_inode **inode,
			    unsigned long *index)
{
	lockdep_assert_held(&(page_pool(page)->free_folios_lock));

	*inode = (struct cleancache_inode *)page->mapping;
	*index = page->index;
}

/* page pool helpers */
static void add_page_to_pool(struct page *page, struct cleancache_pool *pool)
{
	unsigned long flags;

	VM_BUG_ON(!list_empty(&page->lru));

	spin_lock_irqsave(&pool->free_folios_lock, flags);

	set_page_inode_offs(page, NULL, 0);
	SetPageCCacheFree(page);
	list_add(&page_folio(page)->lru, &pool->free_folios);

	spin_unlock_irqrestore(&pool->free_folios_lock, flags);
}

static struct page *remove_page_from_pool(struct page *page, struct cleancache_pool *pool)
{
	lockdep_assert_held(&pool->free_folios_lock);
	VM_BUG_ON(page_pool(page) != pool);

	if (!PageCCacheFree(page))
		return NULL;

	list_del_init(&page->lru);
	ClearPageCCacheFree(page);

	return page;
}

static struct page *pick_page_from_pool(void)
{
	struct cleancache_pool *pool;
	struct page *page = NULL;
	unsigned long flags;
	int count;

	count = atomic_read_acquire(&nr_pools);
	for (int i = 0; i < count; i++) {
		pool = &pools[i];
		spin_lock_irqsave(&pool->free_folios_lock, flags);
		if (!list_empty(&pool->free_folios)) {
			struct folio *folio;

			folio = list_last_entry(&pool->free_folios,
						struct folio, lru);
			page = &folio->page;
			WARN_ON(!remove_page_from_pool(page, pool));
			spin_unlock_irqrestore(&pool->free_folios_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&pool->free_folios_lock, flags);
	}

	return page;
}

/* FS helpers */
static struct cleancache_fs *get_fs(int fs_id)
{
	struct cleancache_fs *fs;

	rcu_read_lock();
	fs = idr_find(&fs_idr, fs_id);
	if (fs && !refcount_inc_not_zero(&fs->ref_count))
		fs = NULL;
	rcu_read_unlock();

	return fs;
}

static void put_fs(struct cleancache_fs *fs)
{
	if (refcount_dec_and_test(&fs->ref_count))
		kfree(fs);
}

/* inode helpers */
static struct cleancache_inode *alloc_inode(struct cleancache_fs *fs,
					    struct cleancache_filekey *key)
{
	struct cleancache_inode *inode;

	inode = kmem_cache_alloc(slab_inode, GFP_ATOMIC|__GFP_NOWARN);
	if (inode) {
		memcpy(&inode->key, key, sizeof(*key));
		xa_init_flags(&inode->pages, XA_FLAGS_LOCK_IRQ);
		INIT_HLIST_NODE(&inode->hash);
		inode->fs = fs;
		refcount_set(&inode->ref_count, 1);
	}

	return inode;
}

static int erase_pages_from_inode(struct cleancache_inode *inode,
				  bool remove_inode);

static void inode_free_rcu(struct rcu_head *rcu)
{
	struct cleancache_inode *inode;

	inode = container_of(rcu, struct cleancache_inode, rcu);
	erase_pages_from_inode(inode, false);
	kmem_cache_free(slab_inode, inode);
}

static bool get_inode(struct cleancache_inode *inode)
{
	return refcount_inc_not_zero(&inode->ref_count);
}

static bool put_inode(struct cleancache_inode *inode)
{
	if (!refcount_dec_and_test(&inode->ref_count))
		return false;

	call_rcu(&inode->rcu, inode_free_rcu);
	return true;
}

static void remove_inode_if_empty(struct cleancache_inode *inode)
{
	struct cleancache_fs *fs = inode->fs;

	lockdep_assert_held(&inode->pages.xa_lock);

	if (!xa_empty(&inode->pages))
		return;

	spin_lock(&fs->hash_lock);
	if (!WARN_ON(hlist_unhashed(&inode->hash)))
		hlist_del_init_rcu(&inode->hash);
	spin_unlock(&fs->hash_lock);
	/* Caller should have taken an extra refcount to keep inode valid */
	WARN_ON(put_inode(inode));
}

static int store_page_in_inode(struct cleancache_inode *inode,
			       unsigned long index, struct page *page)
{
	struct cleancache_pool *pool = page_pool(page);
	unsigned long flags;
	int err;

	lockdep_assert_held(&inode->pages.xa_lock);
	VM_BUG_ON(!list_empty(&page->lru));

	spin_lock_irqsave(&pool->free_folios_lock, flags);

	err = xa_err(__xa_store(&inode->pages, index, page,
				GFP_ATOMIC|__GFP_NOWARN));
	if (!err) {
		set_page_inode_offs(page, inode, index);
		VM_BUG_ON_PAGE(PageCCacheFree(page), page);
	}

	spin_unlock_irqrestore(&pool->free_folios_lock, flags);

	return err;
}

static void erase_page_from_inode(struct cleancache_inode *inode,
				  unsigned long index, struct page *page)
{
	bool removed;

	lockdep_assert_held(&inode->pages.xa_lock);

	removed = __xa_erase(&inode->pages, index);
	VM_BUG_ON(!removed || !list_empty(&page->lru));

	remove_inode_if_empty(inode);
}

static int erase_pages_from_inode(struct cleancache_inode *inode, bool remove_inode)
{
	XA_STATE(xas, &inode->pages, 0);
	unsigned long flags;
	struct page *page;
	unsigned int ret = 0;

	xas_lock_irqsave(&xas, flags);

	if (!xa_empty(&inode->pages)) {
		xas_for_each(&xas, page, ULONG_MAX) {
			__xa_erase(&inode->pages, xas.xa_index);
			add_page_to_pool(page, page_pool(page));
			ret++;
		}
	}
	if (remove_inode)
		remove_inode_if_empty(inode);

	xas_unlock_irqrestore(&xas, flags);

	return ret;
}

static struct cleancache_inode *find_and_get_inode(struct cleancache_fs *fs,
						   struct cleancache_filekey *key)
{
	struct cleancache_inode *tmp, *inode = NULL;

	rcu_read_lock();
	hash_for_each_possible_rcu(fs->inode_hash, tmp, hash, key->u.ino) {
		if (memcmp(&tmp->key, key, sizeof(*key)))
			continue;

		/* TODO: should we stop if get fails? */
		if (get_inode(tmp)) {
			inode = tmp;
			break;
		}
	}
	rcu_read_unlock();

	return inode;
}

static struct cleancache_inode *add_and_get_inode(struct cleancache_fs *fs,
						  struct cleancache_filekey *key)
{
	struct cleancache_inode *inode, *tmp;
	unsigned long flags;

	inode = alloc_inode(fs, key);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	spin_lock_irqsave(&fs->hash_lock, flags);
	tmp = find_and_get_inode(fs, key);
	if (tmp) {
		spin_unlock_irqrestore(&fs->hash_lock, flags);
		/* someone already added it */
		put_inode(inode);
		put_inode(tmp);
		return ERR_PTR(-EEXIST);
	}

	hash_add_rcu(fs->inode_hash, &inode->hash, key->u.ino);
	get_inode(inode);
	spin_unlock_irqrestore(&fs->hash_lock, flags);

	return inode;
}

/*
 * We want to store only workingset pages in the cleancache to increase hit
 * ratio so there are four cases:
 *
 * @page is workingset but cleancache doesn't have it: use new cleancache page
 * @page is workingset and cleancache has it: overwrite the stale data
 * @page is !workingset and cleancache doesn't have it: just bail out
 * @page is !workingset and cleancache has it: remove the stale @page
 */
static bool store_into_inode(struct cleancache_fs *fs,
			     struct cleancache_filekey *key,
			     pgoff_t offset, struct page *page)
{
	bool workingset = PageWorkingset(page);
	struct cleancache_inode *inode;
	struct page *stored_page;
	void *src, *dst;
	bool ret = false;

find_inode:
	inode = find_and_get_inode(fs, key);
	if (!inode) {
		if (!workingset)
			return false;

		inode = add_and_get_inode(fs, key);
		if (IS_ERR_OR_NULL(inode)) {
			/*
			 * Retry if someone just added new inode from under us.
			 */
			if (PTR_ERR(inode) == -EEXIST)
				goto find_inode;

			return false;
		}
	}

	xa_lock(&inode->pages);

	stored_page = xa_load(&inode->pages, offset);
	if (stored_page) {
		if (!workingset) {
			erase_page_from_inode(inode, offset, stored_page);
			add_page_to_pool(stored_page, page_pool(stored_page));
			goto out_unlock;
		}
	} else {
		if (!workingset)
			goto out_unlock;

		stored_page = pick_page_from_pool();
		if (!stored_page)
			goto out_unlock;

		if (store_page_in_inode(inode, offset, stored_page)) {
			add_page_to_pool(stored_page, page_pool(stored_page));
			goto out_unlock;
		}
	}

	/* Copy the content of the page */
	src = kmap_local_page(page);
	dst = kmap_local_page(stored_page);
	memcpy(dst, src, PAGE_SIZE);
	kunmap_local(dst);
	kunmap_local(src);

	ret = true;
out_unlock:
	/*
	 * Remove the inode if it was just created but we failed to add a page.
	 */
	remove_inode_if_empty(inode);
	xa_unlock(&inode->pages);
	put_inode(inode);

	return ret;
}

static bool load_from_inode(struct cleancache_fs *fs,
			    struct cleancache_filekey *key,
			    pgoff_t offset, struct page *page)
{
	struct cleancache_inode *inode;
	struct page *stored_page;
	void *src, *dst;
	bool ret = false;

	inode = find_and_get_inode(fs, key);
	if (!inode)
		return false;

	xa_lock(&inode->pages);

	stored_page = xa_load(&inode->pages, offset);
	if (stored_page) {
		src = kmap_local_page(stored_page);
		dst = kmap_local_page(page);
		memcpy(dst, src, PAGE_SIZE);
		kunmap_local(dst);
		kunmap_local(src);
		ret = true;
	}

	xa_unlock(&inode->pages);
	put_inode(inode);

	return ret;
}

static bool invalidate_page(struct cleancache_fs *fs,
			    struct cleancache_filekey *key, pgoff_t offset)
{
	struct cleancache_inode *inode;
	struct page *page;

	inode = find_and_get_inode(fs, key);
	if (!inode)
		return false;

	xa_lock(&inode->pages);
	page = xa_load(&inode->pages, offset);
	if (page) {
		erase_page_from_inode(inode, offset, page);
		add_page_to_pool(page, page_pool(page));
	}
	xa_unlock(&inode->pages);
	put_inode(inode);

	return page != NULL;
}

static unsigned int invalidate_inode(struct cleancache_fs *fs,
				     struct cleancache_filekey *key)
{
	struct cleancache_inode *inode;
	unsigned int ret;

	inode = find_and_get_inode(fs, key);
	if (!inode)
		return 0;

	ret = erase_pages_from_inode(inode, true);
	put_inode(inode);

	return ret;
}

/* Hooks into MM and FS */
void cleancache_add_fs(struct super_block *sb)
{
	int fs_id;
	struct cleancache_fs *fs;

	fs = kzalloc(sizeof(struct cleancache_fs), GFP_KERNEL);
	if (!fs)
		goto err;

	spin_lock_init(&fs->hash_lock);
	hash_init(fs->inode_hash);
	refcount_set(&fs->ref_count, 1);

	idr_preload(GFP_KERNEL);
	spin_lock(&fs_lock);
	fs_id = idr_alloc(&fs_idr, fs, 0, 0, GFP_NOWAIT);
	spin_unlock(&fs_lock);
	idr_preload_end();

	if (fs_id < 0) {
		pr_warn("too many file systems\n");
		goto err_free;
	}

	sb->cleancache_id = fs_id;
	return;

err_free:
	kfree(fs);
err:
	sb->cleancache_id = CLEANCACHE_ID_INVALID;
}

void cleancache_remove_fs(struct super_block *sb)
{
	int fs_id = sb->cleancache_id;
	struct cleancache_inode *inode;
	struct cleancache_fs *fs;
	struct hlist_node *tmp;
	int cursor;

	sb->cleancache_id = CLEANCACHE_ID_INVALID;
	fs = get_fs(fs_id);
	if (!fs)
		return;

	/*
	 * No need to hold any lock here since this function is called when
	 * fs is unmounted. IOW, inode insert/delete race cannot happen.
	 */
	hash_for_each_safe(fs->inode_hash, cursor, tmp, inode, hash)
		cleancache_invalidates += invalidate_inode(fs, &inode->key);
	synchronize_rcu();

#ifdef CONFIG_DEBUG_VM
	for (int i = 0; i < HASH_SIZE(fs->inode_hash); i++)
		VM_BUG_ON(!hlist_empty(&fs->inode_hash[i]));
#endif
	spin_lock(&fs_lock);
	idr_remove(&fs_idr, fs_id);
	spin_unlock(&fs_lock);
	put_fs(fs);
	pr_info("removed file system %d\n", fs_id);

	/* free the object */
	put_fs(fs);
}

/*
 * WARNING: This cleancache function might be called with disabled irqs
 */
void cleancache_store_folio(struct folio *folio,
			    struct cleancache_filekey *key)
{
	struct cleancache_fs *fs;
	int fs_id;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (!key)
		return;

	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return;

	fs_id = folio->mapping->host->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return;

	fs = get_fs(fs_id);
	if (!fs)
		return;

	if (store_into_inode(fs, key, folio->index, &folio->page))
		cleancache_stores++;
	else
		cleancache_failed_stores++;
	put_fs(fs);
}

bool cleancache_restore_folio(struct folio *folio,
			      struct cleancache_filekey *key)
{
	struct cleancache_fs *fs;
	int fs_id;
	bool ret;

	if (!key)
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

	ret = load_from_inode(fs, key, folio->index, &folio->page);
	if (ret)
		cleancache_hits++;
	else
		cleancache_misses++;
	put_fs(fs);

	return ret;
}

/*
 * WARNING: This cleancache function might be called with disabled irqs
 */
void cleancache_invalidate_folio(struct address_space *mapping,
				 struct folio *folio,
				 struct cleancache_filekey *key)
{
	struct cleancache_fs *fs;
	int fs_id;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (!key)
		return;

	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return;

	/* Careful, folio->mapping can be NULL */
	fs_id = mapping->host->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return;

	fs = get_fs(fs_id);
	if (!fs)
		return;

	if (invalidate_page(fs, key, folio->index))
		cleancache_invalidates++;
	put_fs(fs);
}

void cleancache_invalidate_inode(struct address_space *mapping,
				 struct cleancache_filekey *key)
{
	struct cleancache_fs *fs;
	int fs_id;

	if (!key)
		return;

	fs_id = mapping->host->i_sb->cleancache_id;
	if (fs_id == CLEANCACHE_ID_INVALID)
		return;

	fs = get_fs(fs_id);
	if (!fs)
		return;

	cleancache_invalidates += invalidate_inode(fs, key);
	put_fs(fs);
}

/* Backend API */
/*
 * Register a new backend and add its pages for cleancache to use.
 * Returns pool id on success or a negative error code on failure.
 */
int cleancache_register_backend(const char *name, struct list_head *folios)
{
	struct cleancache_pool *pool;
	unsigned long pool_size = 0;
	unsigned long flags;
	struct folio *folio;
	int pool_id;

	/* pools_lock prevents concurrent registrations */
	spin_lock(&pools_lock);

	pool_id = atomic_read(&nr_pools);
	if (pool_id >= CLEANCACHE_MAX_POOLS) {
		spin_unlock(&pools_lock);
		return -ENOMEM;
	}

	pool = &pools[pool_id];
	INIT_LIST_HEAD(&pool->free_folios);
	spin_lock_init(&pool->free_folios_lock);
	/* Ensure above stores complete before we increase the count */
	atomic_set_release(&nr_pools, pool_id + 1);

	spin_unlock(&pools_lock);

	list_for_each_entry(folio, folios, lru) {
		struct page *page;

		/* Do not support large folios yet */
		VM_BUG_ON_FOLIO(folio_test_large(folio), folio);
		VM_BUG_ON_FOLIO(folio_ref_count(folio) != 1, folio);
		page = &folio->page;
		set_page_pool_id(page, pool_id);
		__set_page_inode_offs(page, NULL, 0);
		__SetPageCCacheFree(page);
		pool_size++;
	}

	spin_lock_irqsave(&pool->free_folios_lock, flags);
	list_splice_init(folios, &pool->free_folios);
	spin_unlock_irqrestore(&pool->free_folios_lock, flags);

	pr_info("Registered \'%s\' cleancache backend, pool id %d, size %lu pages\n",
		name ? : "none", pool_id, pool_size);

	return pool_id;
}
EXPORT_SYMBOL(cleancache_register_backend);

int cleancache_backend_get_folio(int pool_id, struct folio *folio)
{
	struct cleancache_inode *inode;
	struct cleancache_pool *pool;
	unsigned long flags;
	unsigned long index;
	struct page *page;


	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return -EOPNOTSUPP;

	page = &folio->page;
	/* Does the page belong to the requesting backend */
	if (page_pool_id(page) != pool_id)
		return -EINVAL;

	pool = &pools[pool_id];
again:
	spin_lock_irqsave(&pool->free_folios_lock, flags);

	/* If page is free inside the pool, return it */
	if (remove_page_from_pool(page, pool)) {
		spin_unlock_irqrestore(&pool->free_folios_lock, flags);
		return 0;
	}

	/*
	 * The page is not free, therefore it has to belong to a valid inode.
	 * Operations on CCacheFree and page->mapping are done under
	 * free_folios_lock which we are currently holding and CCacheFree
	 * always gets cleared before page->mapping is set.
	 */
	page_inode_offs(page, &inode, &index);
	if (WARN_ON(!inode || !get_inode(inode))) {
		spin_unlock_irqrestore(&pool->free_folios_lock, flags);
		return -EINVAL;
	}

	spin_unlock_irqrestore(&pool->free_folios_lock, flags);

	xa_lock_irqsave(&inode->pages, flags);
	/*
	 * Retry if the page got erased from the inode but was not added into
	 * the pool yet. erase_page_from_inode() and add_page_to_pool() happens
	 * under inode->pages.xa_lock which we are holding, therefore by now
	 * both operations should have completed. Let's retry.
	 */
	if (xa_load(&inode->pages, index) != page) {
		xa_unlock_irqrestore(&inode->pages, flags);
		put_inode(inode);
		goto again;
	}

	erase_page_from_inode(inode, index, page);

	spin_lock(&pool->free_folios_lock);
	set_page_inode_offs(page, NULL, 0);
	spin_unlock(&pool->free_folios_lock);

	xa_unlock_irqrestore(&inode->pages, flags);

	put_inode(inode);

	return 0;
}
EXPORT_SYMBOL(cleancache_backend_get_folio);

int cleancache_backend_put_folio(int pool_id, struct folio *folio)
{
	struct cleancache_pool *pool = &pools[pool_id];
	struct page *page;

	/* Do not support large folios yet */
	if (folio_test_large(folio))
		return -EOPNOTSUPP;

	page = &folio->page;
	VM_BUG_ON_PAGE(page_ref_count(page) != 1, page);
	VM_BUG_ON(!list_empty(&page->lru));
	/* Reset struct page fields */
	set_page_pool_id(page, pool_id);
	INIT_LIST_HEAD(&page->lru);
	add_page_to_pool(page, pool);

	return 0;
}
EXPORT_SYMBOL(cleancache_backend_put_folio);

static int __init init_cleancache(void)
{
	slab_inode = KMEM_CACHE(cleancache_inode, 0);
	if (!slab_inode)
		return -ENOMEM;

	return 0;
}
core_initcall(init_cleancache);

#ifdef CONFIG_DEBUG_FS
static int __init cleancache_debugfs_init(void)
{
	struct dentry *root;

	root = debugfs_create_dir("cleancache", NULL);
	debugfs_create_u64("hits", 0444, root, &cleancache_hits);
	debugfs_create_u64("misses", 0444, root, &cleancache_misses);
	debugfs_create_u64("stores", 0444, root, &cleancache_stores);
	debugfs_create_u64("failed_stores", 0444, root, &cleancache_failed_stores);
	debugfs_create_u64("invalidates", 0444, root, &cleancache_invalidates);

	return 0;
}
late_initcall(cleancache_debugfs_init);
#endif
