.. SPDX-License-Identifier: (GPL-2.0+ OR MIT)

===============
VM_BIND locking
===============

This document attempts to describe what's needed to get VM_BIND locking right,
including the userptr mmu_notifier locking and it will also discuss some
optimizations to get rid of the looping through of all userptr mappings and
external / shared object mappings that is needed in the simplest
implementation. It will also discuss some implications for faulting gpu_vms.

Nomenclature
============

* ``Context``: GPU execution context.
* ``gpu_vm``: Abstraction of a virtual GPU address space with
  meta-data. Typically one per client (DRM file-private), or one per
  context.
* ``gpu_vma``: Abstraction of a GPU address range within a gpu_vm with
  associated meta-data. The backing storage of a gpu_vma can either be
  a gem buffer object or anonymous pages mapped also into the CPU
  address space for the process.
* ``userptr gpu_vma or just userptr``: A gpu_vma, the backing store of
  which is anonymous pages as described above.
* ``revalidating``: Revalidating a gpu_vma means making the latest version
  of the backing store resident and making sure the gpu_vma's
  page-table entries point to that backing store.
* ``dma_fence``: A struct dma_fence that is similar to a struct completion
  and which tracks GPU activity. When the GPU activity is finished,
  the dma_fence signals.
* ``dma_resv``: A struct dma_resv (AKA reservation object) that is used
  to track GPU activity in the form of multiple dma_fences on a
  gpu_vm or a gem buffer object. The dma_resv contains an array / list
  of dma_fences and a lock that needs to be held when adding
  additional dma_fences to the dma_resv. The lock is of a type that
  allows deadlock-safe locking of multiple dma_resvs in arbitrary order.
* ``exec function``: An exec function is a function that revalidates all
  affected gpu_vmas, submits a GPU command batch and registers the
  dma_fence representing the GPU command's activity with all affected
  dma_resvs. For completeness, although not covered by this document,
  it's worth mentioning that an exec function may also be the
  revalidation worker that is used by some drivers in compute /
  long-running mode.
* ``local object``: A GEM object which is local to a gpu_vm. Shared gem
  objects also share the gpu_vm's dma_resv.
* ``shared object``: AKA external object: A GEM object which may be shared
  by multiple gpu_vms and whose backing storage may be shared with
  other drivers.


Introducing the locks
=====================

One of the benefits of VM_BIND is that local GEM objects share the gpu_vm's
dma_resv object and hence the dma_resv lock. So even with a huge
number of local GEM objects, only one lock is needed to make the exec
sequence atomic.

The following locks and locking orders are used:

* The ``gpu_vm->lock`` (optionally an rwsem). Protects how the gpu_vm is
  partitioned into gpu_vmas, protects the gpu_vm's list of external objects,
  and can also with some simplification protect the gpu_vm's list of
  userptr gpu_vmas. With the CPU mm analogy this would correspond to the
  mmap_lock.
* The ``userptr_seqlock``. This lock is taken in read mode for each
  userptr gpu_vma on the gpu_vm's userptr list, and in write mode during mmu
  notifier invalidation. This is not a real seqlock but described in
  ``mm/mmu_notifier.c` as a "Collision-retry read-side/write-side
  'lock' a lot like a seqcount, however this allows multiple
  write-sides to hold it at once...". The read side critical section
  is enclosed by ``mmu_interval_read_begin() /
  mmu_interval_read_retry()`` with ``mmu_interval_read_begin()``
  sleeping uninterruptibly if the write side is held.
  The write side is held by the core mm while calling mmu interval
  invalidation notifiers.
* The ``gpu_vm->resv`` lock. Protects the gpu_vm's list of gpu_vmas needing
  rebinding, and also the residency of all the gpu_vm's local GEM object.
* The ``gpu_vm->userptr_notifier_lock``. This is an rwsem that is taken in read
  mode during exec and write mode during a mmu notifier invalidation. In
  the absence of a separate page-table lock, this lock can serve
  together with the gpu_vm's dma_resv lock as a page-table lock. More on
  this below. The userptr notifier lock is per gpu_vm.
* The ``gpu_vm->page_table_lock``. Protects the gpu_vm's page-table updates. For
  simplicity the gpu_vm's dma_resv lock can be reused as page-table lock.

There are certain optimizations described below that require
additional locks. More on that later.

.. code-block:: C

   dma_resv_lock(&gpu_vm->resv);

   for_each_gpu_vma_on_revalidate_list(gpu_vm, &gpu_vma) {
		revalidate_gpu_vma(&gpu_vma);
		remove_from_revalidate_list(&gpu_vma);
   }

   add_dependencies(&gpu_job, &gpu_vm->resv);
   job_dma_fence = gpu_submit(&gpu_job));

   add_dma_fence(job_dma_fence, &gpu_vm->resv);
   dma_resv_unlock(&gpu_vm->resv);

Eviction of one of these local objects will then be something like the
following:

.. code-block:: C

   obj = get_object_from_lru();

   dma_resv_lock(obj->resv);
   for_each_gpu_vma_of_obj(obj, &gpu_vma);
		put_gpu_vma_on_revalidate_list(&gpu_vma);

   add_dependencies(&eviction_job, &obj->resv);
   job_dma_fence = gpu_submit(&eviction_job);
   add_dma_fence(&obj->resv, job_dma_fence);

   dma_resv_unlock(&obj->resv);
   put_object(obj);

Note that since the object is local to the gpu_vm, it will share the gpu_vm's
``dma_resv`` lock so that ``obj->resv == gpu_vm->resv``. Invalidated gpu_vmas are put
on the gpu_vm's revalidation list, which is protected by ``gpu_vm->resv``, which
is always locked while evicting, due to the above equality.

For VM_BIND gpu_vms, gpu_vmas don't need to be unbound before eviction,
Since the eviction blit or copy will wait for GPU idle, any attempt by
the GPU to access freed memory through the gpu_vma will be preceded by
a new exec function, which will make sure the gpu_vma is
revalidated. The eviction code holding the object's dma_resv while
revalidating will ensure a new exec function may not race with the eviction.

Introducing external (or shared) buffer objects
===============================================

Since shared buffer objects may be shared by multiple gpu_vm's they
can't share their reservation object with a single gpu_vm, but will rather
have a reservation object of their own. The shared objects bound to a
gpu_vm using one or many
gpu_vmas are therefore typically put on a per-gpu_vm list which is
protected by the gpu_vm lock. One could in theory protect it also with
the ``gpu_vm->resv``, but since the list of dma_resvs to take is typically
built before the ``gpu_vm->resv`` is locked due to a limitation in
the current locking helpers, that is typically not done. Also see
below for userptr gpu_vmas.

At eviction time we now need to invalidate *all* gpu_vmas of a shared
object, but we can no longer be certain that we hold the gpu_vm's
dma_resv of all the object's gpu_vmas. We can only be certain that we
hold the object's private dma_resv. We can trylock the dma_resvs for
the affected gpu_vm's but that might be unnecessarily complex. If we
have a ww_acquire context at hand at eviction time we can also perform
sleeping locks of those dma_resvs but that could cause expensive
rollbacks. One option is to just mark the invalidated gpu_vmas with a bool
which is inspected on the next exec function, when the gpu_vm's
dma_resv and the object's dma_resv is held, and the invalidated
gpu_vmas could then be put on the gpu_vm's list of invalidated
gpu_vmas. That bool would then, although being per-gpu_vma formally be
protected by the object's dma_resv.

The exec function would then look something like the following:

.. code-block:: C

   read_lock(&gpu_vm->lock);

   dma_resv_lock(&gpu_vm->resv);

   // Shared object list is protected by the gpu_vm->lock.
   for_each_shared_obj(gpu_vm, &obj) {
		dma_resv_lock(&obj->resv);
		move_marked_gpu_vmas_to_revalidate_gpu_vma_list(obj, &gpu_vm);
   }

   for_each_gpu_vma_to_revalidate(gpu_vm, &gpu_vma) {
		revalidate_gpu_vma(&gpu_vma);
		remove_from_revalidate_list(&gpu_vma);
   }

   add_dependencies(&gpu_job, &gpu_vm->resv);
   job_dma_fence = gpu_submit(&gpu_job));

   add_dma_fence(job_dma_fence, &gpu_vm->resv);
   for_each_shared_obj(gpu_vm, &obj)
          add_dma_fence(job_dma_fence, &obj->resv);
   dma_resv_unlock_all_resv_locks();

   read_unlock(&gpu_vm->lock);

And the corresponding shared-object aware eviction would look like:

.. code-block:: C

   obj = get_object_from_lru();

   dma_resv_lock(obj->resv);
   for_each_gpu_vma_of_obj(obj, &gpu_vma);
		if (object_is_vm_local(obj))
		             put_gpu_vma_on_revalidate_list(&gpu_vma, &gpu_vm);
		else
		             mark_gpu_vma_for_revalidation(&gpu_vma);

   add_dependencies(&eviction_job, &obj->resv);
   job_dma_fence = gpu_submit(&eviction_job);
   add_dma_fence(&obj->resv, job_dma_fence);

   dma_resv_unlock(&obj->resv);
   put_object(obj);

Yet another option is to put the gpu_vmas to be invalidated on a separate
gpu_vm list protected by a lower level lock that can be taken both at eviction
time and at transfer-to-revalidate list time. The details are not in
this document, but this for reference implemented in the Intel xe
driver.

Introducing userptr gpu_vmas
============================

A userptr gpu_vma is a gpu_vma that, instead of mapping a buffer object to a
GPU virtual address range, directly maps a CPU mm range of anonymous-
or file page-cache pages.
A very simple approach would be to just pin the pages using
pin_user_pages() at bind time and unpin them at unbind time, but this
creates a Denial-Of-Service vector since a single user-space process
would be able to pin down all of system memory, which is not
desirable. (For special use-cases and with proper accounting pinning might
still be a desirable feature, though). What we need to do in the general case is
to obtain a reference to the desired pages, make sure we are notified
using a MMU notifier just before the CPU mm unmaps the pages, dirty
them if they are not mapped read-only to the GPU, and then drop the reference.
When we are notified by the MMU notifier that CPU mm is about to drop the
pages, we need to stop GPU access to the pages,
GPU page-table and make sure that before the next time the GPU tries to access
whatever is now present in the CPU mm range, we unmap the old pages
from the GPU page tables and repeat the process of obtaining new page
references. Note that when the core mm decides to laundry pages, we get such
an unmap MMU notification and can mark the pages dirty again before the
next GPU access. We also get similar MMU notifications for NUMA accounting
which the GPU driver doesn't really need to care about, but so far
it's proven difficult to exclude certain notifications.

Using a MMU notifier for device DMA (and other methods) is described in
`this document
<https://docs.kernel.org/core-api/pin_user_pages.html#case-3-mmu-notifier-registration-with-or-without-page-faulting-hardware>`_.

Now the method of obtaining struct page references using
get_user_pages() unfortunately can't be used under a dma_resv lock
since that would violate the locking order of the dma_resv lock vs the
mmap_lock that is grabbed when resolving a CPU pagefault. This means the gpu_vm's
list of userptr gpu_vmas needs to be protected by an outer lock, and this
is the first time we strictly need the gpu_vm->lock. While it was
previously used also to protect the list of the gpu_vm's shared objects,
we could in theory have used the gpu_vm->resv for that.

The MMU interval seqlock for a userptr gpu_vma is used in the following
way:

.. code-block:: C

   down_read(&gpu_vm->lock);

   retry:

   // Note: mmu_interval_read_begin() blocks until there is no
   // invalidation notifier running anymore.
   seq = mmu_interval_read_begin(&gpu_vma->userptr_interval);
   if (seq != gpu_vma->saved_seq) {
           obtain_new_page_pointers(&gpu_vma);
	   dma_resv_lock(&gpu_vm->resv);
	   put_gpu_vma_on_revalidate_list(&gpu_vma, &gpu_vm);
	   dma_resv_unlock(&gpu_vm->resv);
	   gpu_vma->saved_seq = seq;
   }

   // The usual revalidation goes here.

   // Final userptr sequence validation may not happen before the
   // submission dma_fence is added to the gpu_vm's resv, from the POW
   // of the MMU invalidation notifier. Hence the
   // userptr_notifier_lock that will make them appear atomic.

   add_dependencies(&gpu_job, &gpu_vm->resv);
   down_read(&gpu_vm->userptr_notifier_lock);
   if (mmu_interval_read_retry(&gpu_vma->userptr_interval, gpu_vma->saved_seq)) {
          up_read(&gpu_vm->userptr_notifier_lock);
	  goto retry;
   }

   job_dma_fence = gpu_submit(&gpu_job));

   add_dma_fence(job_dma_fence, &gpu_vm->resv);

   for_each_shared_obj(gpu_vm, &obj)
          add_dma_fence(job_dma_fence, &obj->resv);

   dma_resv_unlock_all_resv_locks();
   up_read(&gpu_vm->userptr_notifier_lock);
   up_read(&gpu_vm->lock);

The code between ``mmu_interval_read_begin()`` and the
``mmu_interval_read_retry()`` marks the read side critical section of
what we call the ``userptr_seqlock``. In reality the gpu_vm's userptr
gpu_vma list is looped through, and the check is done for *all* of its
userptr gpu_vmas, although we only show a single one here.

The userptr gpu_vma MMU invalidation notifier might be called from
reclaim context and, again to avoid locking order violations, we can't
take any dma_resv lock nor the gpu_vm->lock from within it.

.. code-block:: C

  bool gpu_vma_userptr_invalidate(userptr_interval, cur_seq)
  {
          // Make sure the exec function either sees the new sequence
	  // and backs off or we wait for the dma-fence:

          down_write(&gpu_vm->userptr_notifier_lock);
	  mmu_interval_set_seq(userptr_interval, cur_seq);
	  up_write(&gpu_vm->userptr_notifier_lock);

	  dma_resv_wait_timeout(&gpu_vm->resv, DMA_RESV_USAGE_BOOKKEEP,
		                false, MAX_SCHEDULE_TIMEOUT);
	  return true;
  }

When this invalidation notifier returns, the GPU can no longer be
accessing the old pages of the userptr gpu_vma and needs to redo the page-binding
before a new GPU submission can succeed.

Optimizing gpu_vma iteration
----------------------------

Iterating through all of a gpu_vm's userptr gpu_vmas to check the validity
on each exec function may be very costly. There is a scheme to avoid
this and only iterate through the userptr gpu_vmas that actually saw an
invalidation notifier call since the last exec. T

TODO: describe that scheme here. It's implemented in the xe driver.

Locking for page-table updates at bind- and unbind time
=======================================================

TODO.

Recoverable page-fault implications
===================================

TODO.
