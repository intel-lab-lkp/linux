// SPDX-License-Identifier: GPL-2.0-only
/*
 * Device Memory Buffer support for virtio devices.
 *
 * A device that negotiates VIRTIO_F_DMB owns one shared memory region, the
 * Device Memory Buffer, that holds its virtqueues and the buffers they
 * reference.  The data DMA of such a device is routed through a per-device
 * transparent IOMMU whose bus address space maps 1:1 onto that region, so
 * every address the driver publishes to the device is an address in that
 * space.
 *
 * This file carves the region up with a gen_pool and implements the
 * virtio_map_ops that turn allocations into those addresses.  A mapping handle
 * belonging to a DMB device is an address in that space and nothing else: no
 * code outside these operations may treat it as a DMA address.
 *
 * The region is shared with the device, which may read or write any of it at
 * any time.  Nothing this file reads back from the region is used to compute a
 * kernel address, a length or an index.
 */

#include <linux/align.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/genalloc.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#include "virtio_dmb.h"

/* No source recorded: this allocation holds no bounced mapping. */
#define DMB_SRC_NONE		((phys_addr_t)-1)

/**
 * struct virtio_dmb_alloc - what the driver remembers about one allocation
 * @len: length in bytes, which is what bounds a copy
 * @src: physical address the allocation bounces, DMB_SRC_NONE for an area
 *
 * gen_pool owns which pages are allocated; this records what an allocation is.
 * gen_pool cannot hold either field: its only opaque value is per chunk rather
 * than per allocation (lib/genalloc.c:493), and it derives a freed extent from
 * the length its caller passes (lib/genalloc.c:501) rather than from anything
 * it remembers.
 *
 * A zero @len is what marks a record as holding no live allocation, which is
 * the state the records are allocated in.  No live allocation can have that
 * length, because both callers of virtio_dmb_claim() reject a zero size before
 * reaching it.
 *
 * @len is a byte count, not a page count.  A page-granular bound would admit a
 * length ending anywhere inside the allocation's last page, up to PAGE_SIZE - 1
 * bytes past what was mapped, and virtio_dmb_copy() would then touch the page
 * after the source run.
 *
 * @src doubles as the mark that tells a bounced mapping from a virtqueue area.
 * Without it, unmap_page() on an area would copy from (phys_addr_t)-1.
 */
struct virtio_dmb_alloc {
	size_t		len;
	phys_addr_t	src;
};

/**
 * struct virtio_dmb - driver-side state for one Device Memory Buffer
 * @vdev: the device that owns the region, for message context
 * @map_va: what memremap() returned, for memunmap()
 * @map_phys: physical base of the region, for release_mem_region()
 * @map_len: length of the claimed and mapped part of the region
 * @map_claimed: whether request_mem_region() succeeded for that range
 * @prev_map: map operations the transport had installed, restored on teardown
 * @prev_vmap: mapping token that went with @prev_map
 * @pool: allocator over the region, addressed by kernel address
 * @allocs: one record per pool page, indexed by the page
 * @base_va: kernel address the pool starts at, inside the mapping
 * @base_off: region address the pool starts at
 * @nslots: pool size in PAGE_SIZE pages
 * @shm_id: shared memory id the device reported for the region
 */
struct virtio_dmb {
	struct virtio_device	*vdev;
	void			*map_va;
	phys_addr_t		 map_phys;
	size_t			 map_len;
	bool			 map_claimed;
	const struct virtio_map_ops *prev_map;
	union virtio_map	 prev_vmap;
	struct gen_pool		*pool;
	struct virtio_dmb_alloc *allocs;
	void			*base_va;
	u64			 base_off;
	unsigned int		 nslots;
	u16			 shm_id;
};

static unsigned int virtio_dmb_slots(size_t size)
{
	return DIV_ROUND_UP(size, PAGE_SIZE);
}

static size_t virtio_dmb_pool_size(const struct virtio_dmb *dmb)
{
	return (size_t)dmb->nslots << PAGE_SHIFT;
}

/* Handle the driver publishes for the allocation starting at pool page @slot. */
static dma_addr_t virtio_dmb_handle(const struct virtio_dmb *dmb,
				    unsigned int slot)
{
	return dmb->base_off + ((u64)slot << PAGE_SHIFT);
}

/* Pool page of @va, which every caller has already obtained from the pool. */
static unsigned int virtio_dmb_slot_of(const struct virtio_dmb *dmb, void *va)
{
	return (unsigned int)(((char *)va - (char *)dmb->base_va) >> PAGE_SHIFT);
}

/*
 * The largest buffer mapping this pool will serve: an eighth of it, and never
 * less than one page, which is the floor a region at the four-page minimum
 * sits on.  The eighth is a policy choice.  It bounds the capacity one mapping
 * can deny the rest of the device, so that a device with several virtqueues
 * still makes progress while one large mapping is outstanding.
 *
 * The cap applies to map_page() only.  An alloc() is a virtqueue area, which
 * lives as long as the queue and which no back-pressure can defer, so capping
 * it could only shrink a queue or refuse one outright.  Sizing the region for
 * the areas as well as the buffers is the device's obligation.
 */
static size_t virtio_dmb_max_mapping(const struct virtio_dmb *dmb)
{
	size_t eighth = ALIGN_DOWN(virtio_dmb_pool_size(dmb) / 8, PAGE_SIZE);

	return max_t(size_t, eighth, PAGE_SIZE);
}

/*
 * Claim @len bytes and remember what the allocation is, or return NULL.  @slot
 * receives the first pool page, which is what names the allocation.
 *
 * Exhaustion is routine, because the region's length bounds how much virtqueue
 * data can be in flight, and it is not always back-pressure: a network receive
 * fill has nothing to push back on and repolls instead.  Reporting it at a
 * level a working device would print would be a log flood, so it goes to
 * dynamic debug.
 */
static void *virtio_dmb_claim(struct virtio_dmb *dmb, size_t len,
			      phys_addr_t src, unsigned int *slot,
			      struct virtio_dmb_alloc **out)
{
	struct virtio_dmb_alloc *rec;
	unsigned long va;

	va = gen_pool_alloc(dmb->pool, len);
	if (!va) {
		dev_dbg_ratelimited(&dmb->vdev->dev,
				    "device memory buffer has no run of %u of %u pages\n",
				    virtio_dmb_slots(len), dmb->nslots);
		return NULL;
	}

	*slot = virtio_dmb_slot_of(dmb, (void *)va);

	/*
	 * gen_pool has just published these pages exclusively to this caller,
	 * so nothing else can be writing this record.
	 */
	rec = &dmb->allocs[*slot];
	rec->len = len;
	rec->src = src;

	*out = rec;
	return (void *)va;
}

/*
 * Release the allocation @rec starting at pool page @slot.  The record goes
 * first, so that a page reachable from the pool never carries an extent
 * virtio_dmb_resolve() would trust.
 */
static void virtio_dmb_release(struct virtio_dmb *dmb, unsigned int slot,
			       struct virtio_dmb_alloc *rec)
{
	size_t len = rec->len;

	memset(rec, 0, sizeof(*rec));

	gen_pool_free(dmb->pool,
		      (unsigned long)dmb->base_va + ((size_t)slot << PAGE_SHIFT),
		      len);
}

/*
 * Resolve a handle to the allocation it names and the pool page it starts at,
 * rejecting anything that is not a live allocation of at least @size bytes.
 * @bounced additionally requires an allocation this file bounced rather than a
 * virtqueue area.
 *
 * Every caller runs this before touching the pool, which is what keeps a stale
 * or malformed handle away from gen_pool_free().  That function derives the
 * extent it frees from the length it is given (lib/genalloc.c:501), so an
 * over-long length clears a neighbouring allocation's bits with no diagnostic
 * at all, and reaches BUG_ON(remain) (lib/genalloc.c:508) only once the range
 * runs into free space.  gen_pool_has_addr() answers a different question,
 * testing chunk containment alone (lib/genalloc.c:553).
 */
static bool virtio_dmb_resolve(struct virtio_dmb *dmb, dma_addr_t handle,
			       size_t size, bool bounced, unsigned int *slot,
			       struct virtio_dmb_alloc **out)
{
	struct virtio_dmb_alloc *rec;
	u64 off;

	if (!size || (u64)handle < dmb->base_off)
		goto bad_handle;

	off = (u64)handle - dmb->base_off;
	if (!IS_ALIGNED(off, PAGE_SIZE) ||
	    (off >> PAGE_SHIFT) >= dmb->nslots)
		goto bad_handle;

	rec = &dmb->allocs[off >> PAGE_SHIFT];
	if (dev_WARN_ONCE(&dmb->vdev->dev, !rec->len,
			  "device memory buffer handle %pad holds no allocation\n",
			  &handle))
		return false;

	if (dev_WARN_ONCE(&dmb->vdev->dev, size > rec->len,
			  "device memory buffer handle %pad length %zu leaves its allocation\n",
			  &handle, size))
		return false;

	if (bounced &&
	    dev_WARN_ONCE(&dmb->vdev->dev, rec->src == DMB_SRC_NONE,
			  "device memory buffer handle %pad holds no mapping\n",
			  &handle))
		return false;

	*slot = off >> PAGE_SHIFT;
	*out = rec;
	return true;

bad_handle:
	dev_WARN_ONCE(&dmb->vdev->dev, 1,
		      "device memory buffer handle %pad length %zu out of range\n",
		      &handle, size);
	return false;
}

/*
 * Copy @size bytes between the region and the pages the allocation at pool
 * page @slot bounces.  Walk the source a page at a time through
 * kmap_local_page(): it may be highmem, which is why the record is a physical
 * address.  The region side is one contiguous mapping and needs no such split.
 */
static void virtio_dmb_copy(struct virtio_dmb *dmb, unsigned int slot,
			    const struct virtio_dmb_alloc *rec, size_t size,
			    bool to_region)
{
	void *region = dmb->base_va + ((size_t)slot << PAGE_SHIFT);
	size_t done = 0;

	while (done < size) {
		phys_addr_t src = rec->src + done;
		unsigned int in_src = offset_in_page(src);
		size_t n = min(size - done, (size_t)PAGE_SIZE - in_src);
		void *va;

		va = kmap_local_page(pfn_to_page(PHYS_PFN(src)));
		if (to_region)
			memcpy(region + done, va + in_src, n);
		else
			memcpy(va + in_src, region + done, n);
		kunmap_local(va);

		done += n;
	}
}

static void *virtio_dmb_op_alloc(union virtio_map map, size_t size,
				 dma_addr_t *map_handle, gfp_t gfp)
{
	struct virtio_dmb *dmb = map.dmb;
	struct virtio_dmb_alloc *rec;
	unsigned int slot;
	void *va;

	/*
	 * The result is zeroed because this stands in for
	 * dma_alloc_coherent(), whose callers rely on that.
	 */
	if (!size || size > virtio_dmb_pool_size(dmb))
		goto no_room;

	va = virtio_dmb_claim(dmb, size, DMB_SRC_NONE, &slot, &rec);
	if (!va)
		goto no_room;

	memset(va, 0, size);
	*map_handle = virtio_dmb_handle(dmb, slot);

	return va;

no_room:
	/*
	 * A buffer that does not fit is back-pressure and stays quiet, but a
	 * virtqueue area cannot be deferred, and a region sized for the buffers
	 * but not for the areas otherwise fails queue setup with nothing to
	 * tell it apart from every other reason find_vqs() can fail.
	 *
	 * Which is why __GFP_NOWARN is honoured rather than ignored.
	 * vring_alloc_queue_split() walks the queue size down from the size the
	 * device asked for and marks every attempt but the last with the flag,
	 * so warning regardless would print a line per attempt for a probe that
	 * then succeeds.  Dynamic debug still carries the message, which is
	 * what a packed ring relies on: none of its three areas can be made
	 * smaller and all three set the flag.
	 */
	if (gfp & __GFP_NOWARN)
		dev_dbg(&dmb->vdev->dev,
			"no room for a %zu-byte virtqueue area in %u pages\n",
			size, dmb->nslots);
	else
		dev_warn(&dmb->vdev->dev,
			 "no room for a %zu-byte virtqueue area in %u pages\n",
			 size, dmb->nslots);
	return NULL;
}

/*
 * A caller reaching this with a bounced mapping has called free() on something
 * it got from map_page(), and loses the copy-out that unmap_page() does; the
 * bytes it loses are its own, and refusing would trade that for a leak of the
 * pages, which is worse.  So this asks for an allocation and not for an area.
 */
static void virtio_dmb_op_free(union virtio_map map, size_t size, void *vaddr,
			       dma_addr_t map_handle, unsigned long attrs)
{
	struct virtio_dmb *dmb = map.dmb;
	struct virtio_dmb_alloc *rec;
	unsigned int slot;

	if (!virtio_dmb_resolve(dmb, map_handle, size, false, &slot, &rec))
		return;

	virtio_dmb_release(dmb, slot, rec);
}

static dma_addr_t virtio_dmb_op_map_page(union virtio_map map,
					 struct page *page,
					 unsigned long offset, size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	struct virtio_dmb *dmb = map.dmb;
	phys_addr_t src = page_to_phys(page) + offset;
	struct virtio_dmb_alloc *rec;
	unsigned int slot;
	void *va;

	if (!size || size > virtio_dmb_max_mapping(dmb))
		return DMA_MAPPING_ERROR;

	va = virtio_dmb_claim(dmb, size, src, &slot, &rec);
	if (!va)
		return DMA_MAPPING_ERROR;

	/*
	 * Copy in whatever the direction is, and without honouring
	 * DMA_ATTR_SKIP_CPU_SYNC.  swiotlb_tbl_map_single() bounces
	 * unconditionally for the same two reasons: a device that writes less
	 * than the whole buffer must leave the rest of the caller's bytes
	 * intact, and the mapped bytes must not reach the device as whatever
	 * the pages held before.
	 *
	 * Those bytes and no others.  The rest of the allocation's last page
	 * keeps what it held before, which the device may already have put
	 * there itself, and the descriptor carries a length.
	 */
	virtio_dmb_copy(dmb, slot, rec, size, true);

	return virtio_dmb_handle(dmb, slot);
}

static void virtio_dmb_op_unmap_page(union virtio_map map,
				     dma_addr_t map_handle, size_t size,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	struct virtio_dmb *dmb = map.dmb;
	struct virtio_dmb_alloc *rec;
	unsigned int slot;

	if (!virtio_dmb_resolve(dmb, map_handle, size, true, &slot, &rec))
		return;

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) &&
	    (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL))
		virtio_dmb_copy(dmb, slot, rec, size, false);

	/*
	 * The extent freed comes from the record rather than from the caller's
	 * size, so a mismatched size cannot release a different number of pages
	 * than were claimed.
	 */
	virtio_dmb_release(dmb, slot, rec);
}

static int virtio_dmb_op_mapping_error(union virtio_map map,
				       dma_addr_t map_handle)
{
	/*
	 * DMA_MAPPING_ERROR is the value virtio_ring reserves.  Address 0 is
	 * reserved too, both by the feature and by every device that reads a
	 * queue address of zero as a queue never programmed, so this driver may
	 * not publish it either.  Nothing allocated here yields it, but a
	 * premapped buffer carries an address its caller obtained and
	 * vring_map_one_sg() asks this operation to judge that one.  Whether
	 * such an address came from this map cannot be answered here; zero can
	 * be.
	 */
	if (map_handle == DMA_MAPPING_ERROR || !map_handle)
		return -ENOMEM;

	return 0;
}

static size_t virtio_dmb_op_max_mapping_size(union virtio_map map)
{
	return virtio_dmb_max_mapping(map.dmb);
}

static const struct virtio_map_ops virtio_dmb_map_ops = {
	.map_page		= virtio_dmb_op_map_page,
	.unmap_page		= virtio_dmb_op_unmap_page,
	.alloc			= virtio_dmb_op_alloc,
	.free			= virtio_dmb_op_free,
	.mapping_error		= virtio_dmb_op_mapping_error,
	.max_mapping_size	= virtio_dmb_op_max_mapping_size,
};

/*
 * Whether the device still has virtqueues.  No caller here can race an adder,
 * because every path that reaches this runs under the device lock, but the
 * invariant is worth enforcing rather than inheriting from callers this file
 * does not control.
 */
static bool virtio_dmb_vqs_live(struct virtio_device *vdev)
{
	bool live;

	spin_lock(&vdev->vqs_list_lock);
	live = !list_empty(&vdev->vqs);
	spin_unlock(&vdev->vqs_list_lock);

	return live;
}

/**
 * virtio_dmb_destroy - release the Device Memory Buffer state of a device
 * @vdev: the device
 *
 * Does nothing unless @vdev is currently using a Device Memory Buffer, and
 * refuses if the device still has virtqueues.  The mapping and the region
 * claim then stay behind until something deletes those virtqueues and calls
 * again, which unbinding the driver does: virtio_dev_remove() calls the
 * driver's remove() before this.
 */
void virtio_dmb_destroy(struct virtio_device *vdev)
{
	struct virtio_dmb *dmb;

	/* vdev->map identifies which member of vdev->vmap is live. */
	if (vdev->map != &virtio_dmb_map_ops)
		return;

	/*
	 * A virtqueue keeps the mapping token it was created with, while
	 * vdev->map is consulted afresh on every dispatch.  Clearing vdev->map
	 * therefore does not disarm a live virtqueue, it redirects that
	 * virtqueue's copy of the token into the DMA API, where the pointer
	 * this frees would be used as a struct device.  Refuse instead and leak
	 * the mapping, which is better than a use-after-free.
	 *
	 * Reported rather than warned about: a device that fails to report its
	 * region on the way back from a suspend takes virtio_device_restore()
	 * to its error path, which calls this, and a driver with no freeze
	 * callback still has its virtqueues at that point, correctly.  A
	 * condition a correct driver can satisfy must not taint the kernel.
	 */
	if (virtio_dmb_vqs_live(vdev)) {
		dev_warn(&vdev->dev,
			 "device memory buffer not released, virtqueues are still live\n");
		return;
	}

	dmb = vdev->vmap.dmb;

	/* Put back exactly what the transport had installed. */
	vdev->map = dmb->prev_map;
	vdev->vmap = dmb->prev_vmap;

	/*
	 * gen_pool_destroy() ends at a BUG_ON() for a pool with an allocation
	 * outstanding (lib/genalloc.c:255).  Every allocation belongs to a
	 * virtqueue, and the refusal above establishes that none is left, which
	 * is also why the records are empty by now.
	 */
	gen_pool_destroy(dmb->pool);
	kvfree(dmb->allocs);
	memunmap(dmb->map_va);
	if (dmb->map_claimed)
		release_mem_region(dmb->map_phys, dmb->map_len);
	kfree(dmb);
}
EXPORT_SYMBOL_GPL(virtio_dmb_destroy);

/**
 * virtio_dmb_init - make a device's Device Memory Buffer state current
 * @vdev: the device, with feature negotiation complete
 *
 * Reads the shared memory id the device reports, locates the region, builds an
 * allocator over it and routes every mapping of the device through it.  When
 * the feature is not negotiated, releases any state a previous negotiation
 * left behind.
 *
 * Reached again from resume and from reset completion.  A device that reports
 * the region it reported last time keeps the state it already has, so handles
 * held by a virtqueue that outlived the transition stay valid.
 *
 * A caller that gets an error must set the FAILED device status bit, and must
 * not touch the device for anything else before it does.  The device has
 * already confirmed the feature by the time this runs, so it is entitled to
 * assume the driver will address it through the region; the bit is what tells
 * it the driver gave up instead.
 *
 * Return: 0 on success, or a negative errno.
 */
int virtio_dmb_init(struct virtio_device *vdev)
{
	struct virtio_shm_region region;
	struct virtio_dmb *dmb;
	unsigned int nslots, skew;
	u64 base_off, slots = 0;
	size_t map_len;
	u16 shm_id;
	int err;

	if (!virtio_has_feature(vdev, VIRTIO_F_DMB)) {
		/*
		 * The feature may have been withdrawn across re-negotiation.
		 * virtio_dmb_destroy() refuses under live virtqueues, and
		 * returning 0 after a refusal would leave these map operations
		 * installed for a device that has not negotiated the feature,
		 * so every later mapping would resolve a handle against a
		 * region the device no longer agrees it has.
		 */
		virtio_dmb_destroy(vdev);
		if (vdev->map == &virtio_dmb_map_ops)
			return -EBUSY;
		return 0;
	}

	/*
	 * Without both of these the region cannot be located at all, which is
	 * what keeps a transport that does not implement them from offering the
	 * feature in the first place.
	 */
	if (!vdev->config->get_dmb_shm_id || !vdev->config->get_shm_region) {
		dev_warn(&vdev->dev,
			 "transport cannot locate a device memory buffer\n");
		return -EINVAL;
	}

	err = vdev->config->get_dmb_shm_id(vdev, &shm_id);
	if (err)
		return err;

	/* A region is looked up by a u8 id. */
	if (shm_id > U8_MAX) {
		dev_warn(&vdev->dev,
			 "device memory buffer id %u out of range\n", shm_id);
		return -EINVAL;
	}

	if (!virtio_get_shm_region(vdev, &region, shm_id)) {
		dev_warn(&vdev->dev,
			 "cannot locate device memory buffer region %u\n",
			 shm_id);
		return -ENODEV;
	}

	/*
	 * A device reports a region whose base is aligned to at least the
	 * largest alignment any virtqueue layout requires of an area.  The
	 * driver holds the pool to PAGE_SIZE, which is stronger, and checks
	 * below that memremap() preserved the base's page offset.  Start the
	 * pool at a PAGE_SIZE-aligned address and record the skew, so that
	 * page-granular allocation makes every absolute address aligned.
	 *
	 * PAGE_SIZE - skew is the distance to the first aligned address
	 * strictly after the region start, so the pool never begins at the
	 * region's first byte and no handle is ever 0.  Address 0 is reserved,
	 * a device that predates the reservation reads a queue address of 0 as
	 * a queue never programmed, and gen_pool_alloc() returns 0 for failure,
	 * so the value is unusable three times over.
	 */
	skew = offset_in_page(region.addr);
	base_off = PAGE_SIZE - skew;

	if (region.len > base_off)
		slots = (region.len - base_off) >> PAGE_SHIFT;

	/*
	 * The least a region could hold: one minimally-sized virtqueue plus one
	 * buffer in flight against it.  A packed queue costs three allocations
	 * and a split queue up to two when the transport aligns its areas to
	 * PAGE_SIZE, so four pages is the floor for either layout.  It is a
	 * floor and not a sufficiency check: how much a device needs depends on
	 * how many virtqueues its driver creates and how deep they are, neither
	 * of which is known before find_vqs().
	 *
	 * The upper bounds are what a page index, a mapping length and a
	 * published handle can each represent.  The length bound is exclusive
	 * because the mapping is base_off longer than the pool, and at the last
	 * representable page count that sum would wrap on a 32-bit size_t.
	 */
	if (slots < 4 || slots > UINT_MAX ||
	    slots >= (u64)(SIZE_MAX >> PAGE_SHIFT) ||
	    base_off + (slots << PAGE_SHIFT) - 1 >
			DMA_BIT_MASK(BITS_PER_TYPE(dma_addr_t))) {
		dev_warn(&vdev->dev,
			 "device memory buffer region holds %llu usable pages\n",
			 slots);
		return -EINVAL;
	}
	nslots = slots;

	/* Nothing outside the pool and the bytes ahead of it is used. */
	map_len = base_off + ((size_t)nslots << PAGE_SHIFT);

	/*
	 * The transport reports the region in 64 bits while a resource is
	 * addressed in resource_size_t.  Refuse a region that does not fit
	 * rather than claim and map a truncated one.
	 */
	if (region.addr > (u64)(resource_size_t)-1 - map_len) {
		dev_warn(&vdev->dev,
			 "device memory buffer region at 0x%llx is not addressable\n",
			 region.addr);
		return -EINVAL;
	}

	/*
	 * Everything that identifies the region is known now and nothing has
	 * been touched, so an unchanged region is adopted instead of being torn
	 * down and rebuilt identically.  That is what lets a virtqueue which
	 * outlived a suspend or a reset keep handles that are still valid.  A
	 * region that moved can be neither adopted nor replaced under live
	 * virtqueues, so refuse without a warning: a device that moves its
	 * region while its driver still has virtqueues is misbehaving, which is
	 * not evidence of a kernel bug.
	 */
	if (vdev->map == &virtio_dmb_map_ops) {
		dmb = vdev->vmap.dmb;

		if (dmb->shm_id == shm_id && dmb->map_phys == region.addr &&
		    dmb->map_len == map_len)
			return 0;

		if (virtio_dmb_vqs_live(vdev))
			return -EBUSY;

		virtio_dmb_destroy(vdev);
	}

	dmb = kzalloc(sizeof(*dmb), GFP_KERNEL);
	if (!dmb)
		return -ENOMEM;

	dmb->vdev = vdev;
	dmb->shm_id = shm_id;
	dmb->base_off = base_off;
	dmb->map_phys = region.addr;
	dmb->map_len = map_len;
	dmb->nslots = nslots;

	/*
	 * A shared memory region need not lie in a BAR the transport already
	 * claimed, so record a claim on the range here.  The claim is advisory:
	 * where the region does lie in such a BAR the transport's own claim
	 * covers it already, which is not a conflict, so it must not fail the
	 * device.
	 */
	dmb->map_claimed = request_mem_region(region.addr, map_len,
					      "virtio-dmb") != NULL;
	if (!dmb->map_claimed)
		dev_dbg(&vdev->dev,
			"device memory buffer region at 0x%llx already reserved\n",
			region.addr);

	/*
	 * MEMREMAP_WB - All transports require VIRTIO_DMB_MEM_TYPE_COHERENT
	 * which indicates that the DMB SHM region is cache coherent.
	 * MEMREMAP_DEC - The DMB region is shared memory to a hypervisor so
	 * that both sides can see it.
	 */
	dmb->map_va = memremap(region.addr, map_len,
			       MEMREMAP_WB | MEMREMAP_DEC);
	if (!dmb->map_va) {
		/* memremap() is silent, and every sibling failure names itself. */
		dev_warn(&vdev->dev,
			 "cannot map device memory buffer region at 0x%llx\n",
			 region.addr);
		err = -ENOMEM;
		goto err_unclaim;
	}

	/*
	 * The skew was derived from the physical base, so the pool is aligned
	 * in the mapping only if the mapping kept that page offset.
	 */
	if (offset_in_page(dmb->map_va) != skew) {
		dev_warn(&vdev->dev,
			 "device memory buffer mapping is not page aligned\n");
		err = -EINVAL;
		goto err_unmap;
	}
	dmb->base_va = dmb->map_va + base_off;

	/*
	 * One record per pool page, allocated up front so that claiming an
	 * allocation never has to allocate one.  kvcalloc() because the array
	 * grows with the region and a large one has no need to be physically
	 * contiguous.
	 */
	dmb->allocs = kvcalloc(dmb->nslots, sizeof(*dmb->allocs), GFP_KERNEL);
	if (!dmb->allocs) {
		err = -ENOMEM;
		goto err_unmap;
	}

	/*
	 * PAGE_SHIFT as the minimum allocation order, so that every handle is
	 * page-aligned and one page is the granule a record accounts in.  The
	 * pool is addressed by kernel address and carries the address within
	 * the region as its physical base, so gen_pool_virt_to_phys() yields
	 * the address the driver publishes to the device.
	 */
	dmb->pool = gen_pool_create(PAGE_SHIFT, -1);
	if (!dmb->pool) {
		err = -ENOMEM;
		goto err_free_allocs;
	}

	err = gen_pool_add_virt(dmb->pool, (unsigned long)dmb->base_va,
				base_off, virtio_dmb_pool_size(dmb), -1);
	if (err)
		goto err_destroy_pool;

	dmb->prev_map = vdev->map;
	dmb->prev_vmap = vdev->vmap;

	vdev->vmap.dmb = dmb;
	vdev->map = &virtio_dmb_map_ops;

	dev_info(&vdev->dev,
		 "device memory buffer %u at 0x%016llx, %u usable pages\n",
		 shm_id, region.addr, nslots);

	return 0;

err_destroy_pool:
	gen_pool_destroy(dmb->pool);
err_free_allocs:
	kvfree(dmb->allocs);
err_unmap:
	memunmap(dmb->map_va);
err_unclaim:
	if (dmb->map_claimed)
		release_mem_region(region.addr, map_len);
	kfree(dmb);

	return err;
}
EXPORT_SYMBOL_GPL(virtio_dmb_init);

MODULE_DESCRIPTION("Virtio device memory buffer allocator");
MODULE_LICENSE("GPL");
