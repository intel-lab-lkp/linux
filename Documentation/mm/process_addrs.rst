.. SPDX-License-Identifier: GPL-2.0

=================
Process Addresses
=================

.. toctree::
   :maxdepth: 3


Userland memory ranges are tracked by the kernel via Virtual Memory Areas or
'VMA's of type :c:struct:`!struct vm_area_struct`.

Each VMA describes a virtually contiguous memory range with identical
attributes, each of which described by a :c:struct:`!struct vm_area_struct`
object. Userland access outside of VMAs is invalid except in the case where an
adjacent stack VMA could be extended to contain the accessed address.

All VMAs are contained within one and only one virtual address space, described
by a :c:struct:`!struct mm_struct` object which is referenced by all tasks (that is,
threads) which share the virtual address space. We refer to this as the
:c:struct:`!mm`.

Each mm object contains a maple tree data structure which describes all VMAs
within the virtual address space.

.. note:: An exception to this is the 'gate' VMA which is provided for
	  architectures which use :c:struct:`!vsyscall` and is a global static
	  object which does not belong to any specific mm.

-------
Locking
-------

The kernel is designed to be highly scalable against concurrent read operations
on VMA **metadata** so a complicated set of locks are required to ensure memory
corruption does not occur.

.. note:: Locking VMAs for their metadata does not have any impact on the memory
	  they describe or the page tables that map them.

Terminology
-----------

* **mmap locks** - Each MM has a read/write semaphore `mmap_lock` which locks at
  a process address space granularity which can be acquired via
  :c:func:`!mmap_read_lock`, :c:func:`!mmap_write_lock` and variants.
* **VMA locks** - The VMA lock is at VMA granularity (of course) which behaves
  as a read/write semaphore in practice. A VMA read lock is obtained via
  :c:func:`!lock_vma_under_rcu` (and unlocked via :c:func:`!vma_end_read`) and a
  write lock via :c:func:`!vma_start_write` (all VMA write locks are unlocked
  automatically when the mmap write lock is released). To take a VMA write lock
  you **must** have already acquired an :c:func:`!mmap_write_lock`.
* **rmap locks** - When trying to access VMAs through the reverse mapping via a
  :c:struct:`!struct address_space *` or :c:struct:`!struct anon_vma *` object
  (each obtainable from a folio), VMAs must be stabilised via
  :c:func:`!anon_vma_[try]lock_read` or :c:func:`!anon_vma_[try]lock_write` for
  anonymous memory and :c:func:`!i_mmap_[try]lock_read` or
  :c:func:`!i_mmap_[try]lock_write` for file-backed memory. We refer to these
  locks as the reverse mapping locks, or 'rmap locks' for brevity.

We discuss page table locks separately in the dedicated section below.

The first thing **any** of these locks achieve is to **stabilise** the VMA
within the MM tree. That is, guaranteeing that the VMA object will not be
deleted from under you nor modified (except for some specific exceptions
describe below).

Stabilising a VMA also keeps the address space described by it around.

Using address space locks
-------------------------

If you want to **read** VMA metadata fields or just keep the VMA stable, you
must do one of the following:

* Obtain an mmap read lock at the MM granularity via :c:func:`!mmap_read_lock` (or a
  suitable variant), unlocking it with a matching :c:func:`!mmap_read_unlock` when
  you're done with the VMA, *or*
* Try to obtain a VMA read lock via :c:func:`!lock_vma_under_rcu`. This tries to
  acquire the lock atomically so might fail, in which case fall-back logic is
  required to instead obtain an mmap read lock if this returns :c:macro:`!NULL`,
  *or*
* Acquire an rmap lock before traversing the locked interval tree (whether
  anonymous or file-backed) to obtain the required VMA.

If you want to **write** VMA metadata fields, then things vary depending on the
field (we explore each VMA field in detail below). For the majority you must:

* Obtain an mmap write lock at the MM granularity via :c:func:`!mmap_write_lock` (or a
  suitable variant), unlocking it with a matching :c:func:`!mmap_write_unlock` when
  you're done with the VMA, *and*
* Obtain a VMA write lock via :c:func:`!vma_start_write` for each VMA you wish to
  modify, which will be released automatically when :c:func:`!mmap_write_unlock` is
  called.
* If you want to be able to write to **any** field, you must also hide the VMA
  from the reverse mapping by obtaining an **rmap write lock**.

VMA locks are special in that you must obtain an mmap **write** lock **first**
in order to obtain a VMA **write** lock. A VMA **read** lock however can be
obtained under an RCU lock alone.

.. note:: The primary users of VMA read locks are page fault handlers, which
	  means that without a VMA write lock, page faults will run concurrent with
	  whatever you are doing.

Examining all valid lock states:

.. table::

   ========= ======== ========= ======= ===== =========== ==========
   mmap lock VMA lock rmap lock Stable? Read? Write most? Write all?
   ========= ======== ========= ======= ===== =========== ==========
   \-        \-       \-        N       N     N           N
   \-        R        \-        Y       Y     N           N
   \-        \-       R/W       Y       Y     N           N
   R/W       \-/R     \-/R/W    Y       Y     N           N
   W         W        \-/R      Y       Y     Y           N
   W         W        W         Y       Y     Y           Y
   ========= ======== ========= ======= ===== =========== ==========

.. warning:: While it's possible to obtain a VMA lock while holding an mmap read lock,
	     attempting to do the reverse is invalid as it can result in deadlock - if
	     another task already holds an mmap write lock and attempts to acquire a VMA
	     write lock that will deadlock on the VMA read lock.

All of these locks behave as read/write semaphores in practice, so you can
obtain either a read or a write lock for both.

.. note:: Generally speaking, a read/write semaphore is a class of lock which
	  permits concurrent readers. However a write lock can only be obtained
	  once all readers have left the critical region (and pending readers
	  made to wait).

	  This renders read locks on a read/write semaphore concurrent with other
	  readers and write locks exclusive against all others holding the semaphore.

VMA fields
^^^^^^^^^^

We can subdivide :c:struct:`!struct vm_area_struct` fields by their purpose, which makes it
easier to explore their locking characteristics:

.. note:: We exclude VMA lock-specific fields here to avoid confusion, as these
	  are in effect an internal implementation detail.

.. table:: Virtual layout fields

   ===================== ======================================== ===========
   Field                 Description                              Write lock
   ===================== ======================================== ===========
   :c:member:`!vm_start` Inclusive start virtual address of range mmap write,
                         VMA describes.                           VMA write,
                                                                  rmap write.
   :c:member:`!vm_end`   Exclusive end virtual address of range   mmap write,
                         VMA describes.                           VMA write,
                                                                  rmap write.
   :c:member:`!vm_pgoff` Describes the page offset into the file, rmap write.
                         the original page offset within the      mmap write,
                         virtual address space (prior to any      rmap write.
                         :c:func:`!mremap`), or PFN if a PFN map
                         and the architecture does not support
                         :c:macro:`!CONFIG_ARCH_HAS_PTE_SPECIAL`.
   ===================== ======================================== ===========

These fields describes the size, start and end of the VMA, and as such cannot be
modified without first being hidden from the reverse mapping since these fields
are used to locate VMAs within the reverse mapping interval trees.

.. table:: Core fields

   ============================ ======================================== =========================
   Field                        Description                              Write lock
   ============================ ======================================== =========================
   :c:member:`!vm_mm`           Containing mm_struct.                    None - written once on
                                                                         initial map.
   :c:member:`!vm_page_prot`    Architecture-specific page table         mmap write, VMA write.
                                protection bits determined from VMA
                                flags
   :c:member:`!vm_flags`        Read-only access to VMA flags describing N/A
                                attributes of the VMA, in union with
                                private writable
				:c:member:`!__vm_flags`.
   :c:member:`!__vm_flags`      Private, writable access to VMA flags    mmap write, VMA write.
                                field, updated by
                                :c:func:`!vm_flags_*` functions.
   :c:member:`!vm_file`         If the VMA is file-backed, points to a   None - written once on
                                struct file object describing the        initial map.
                                underlying file, if anonymous then
				:c:macro:`!NULL`.
   :c:member:`!vm_ops`          If the VMA is file-backed, then either   None - Written once on
                                the driver or file-system provides a     initial map by
                                :c:struct:`!struct vm_operations_struct` :c:func:`!f_ops->mmap()`.
				object describing callbacks to be
                                invoked on VMA lifetime events.
   :c:member:`!vm_private_data` A :c:member:`!void *` field for          Handled by driver.
                                driver-specific metadata.
   ============================ ======================================== =========================

These are the core fields which describe the MM the VMA belongs to and its attributes.

.. table:: Config-specific fields

   ================================= ===================== ======================================== ===============
   Field                             Configuration option  Description                              Write lock
   ================================= ===================== ======================================== ===============
   :c:member:`!anon_name`            CONFIG_ANON_VMA_NAME  A field for storing a                    mmap write,
                                                           :c:struct:`!struct anon_vma_name`        VMA write.
                                                           object providing a name for anonymous
                                                           mappings, or :c:macro:`!NULL` if none
							   is set or the VMA is file-backed.
   :c:member:`!swap_readahead_info`  CONFIG_SWAP           Metadata used by the swap mechanism      mmap read.
                                                           to perform readahead.
   :c:member:`!vm_policy`            CONFIG_NUMA           :c:type:`!mempolicy` object which        mmap write,
                                                           describes the NUMA behaviour of the      VMA write.
							   VMA.
   :c:member:`!numab_state`          CONFIG_NUMA_BALANCING :c:type:`!vma_numab_state` object which  mmap read.
                                                           describes the current state of
                                                           NUMA balancing in relation to this VMA.
                                                           Updated under mmap read lock by
							   :c:func:`!task_numa_work`.
   :c:member:`!vm_userfaultfd_ctx`   CONFIG_USERFAULTFD    Userfaultfd context wrapper object of    mmap write,
                                                           type :c:type:`!vm_userfaultfd_ctx`,      VMA write.
                                                           either of zero size if userfaultfd is
                                                           disabled, or containing a pointer
                                                           to an underlying
							   :c:type:`!userfaultfd_ctx` object which
                                                           describes userfaultfd metadata.
   ================================= ===================== ======================================== ===============

These fields are present or not depending on whether the relevant kernel
configuration option is set.

.. table:: Reverse mapping fields

   =================================== ========================================= ================
   Field                               Description                               Write lock
   =================================== ========================================= ================
   :c:member:`!shared.rb`              A red/black tree node used, if the        mmap write,
                                       mapping is file-backed, to place the VMA  VMA write,
                                       in the                                    i_mmap write.
                                       :c:member:`!struct address_space->i_mmap`
				       red/black interval tree.
   :c:member:`!shared.rb_subtree_last` Metadata used for management of the
                                       interval tree if the VMA is file-backed.  mmap write,
                                                                                 VMA write,
                                                                                 i_mmap write.
   :c:member:`!anon_vma_chain`         List of links to forked/CoW’d anon_vma    mmap read,
                                       objects.                                  anon_vma write.
   :c:member:`!anon_vma`               :c:type:`!anon_vma` object used by        mmap_read,
                                       anonymous folios mapped exclusively to    page_table_lock.
				       this VMA.
   =================================== ========================================= ================

These fields are used to both place the VMA within the reverse mapping, and for
anonymous mappings, to be able to access both related :c:struct:`!struct anon_vma` objects
and the :c:struct:`!struct anon_vma` which folios mapped exclusively to this VMA should
reside.

Page tables
-----------

We won't speak exhaustively on the subject but broadly speaking, page tables map
virtual addresses to physical ones through a series of page tables, each of
which contain entries with physical addresses for the next page table level
(along with flags), and at the leaf level the physical addresses of the
underlying physical data pages (with offsets into these pages provided by the
virtual address itself).

In Linux these are divided into five levels - PGD, P4D, PUD, PMD and PTE. Huge
pages might eliminate one or two of these levels, but when this is the case we
typically refer to the leaf level as the PTE level regardless.

.. note:: In instances where the architecture supports fewer page tables than
   five the kernel cleverly 'folds' page table levels, that is skips them within
   the logic, regardless we can act as if there were always five.

There are three key operations typically performed on page tables:

1. **Installing** page table mappings - whether creating a new mapping or
   modifying an existing one.
2. **Zapping/unmapping** page tables - This is what the kernel calls clearing page
   table mappings at the leaf level only, whilst leaving all page tables in
   place. This is a very common operation in the kernel performed on file
   truncation, the :c:macro:`!MADV_DONTNEED` operation via :c:func:`!madvise`,
   and others. This is performed by a number of functions including
   :c:func:`!unmap_mapping_range`, :c:func:`!unmap_mapping_pages` and reverse
   mapping logic.
3. **Freeing** page tables - When finally the kernel removes page tables from a
   userland process (typically via :c:func:`!free_pgtables`) extreme care must
   be taken to ensure this is done safely, as this logic finally frees all page
   tables in the specified range, taking no care whatsoever with existing
   mappings (it assumes the caller has both zapped the range and prevented any
   further faults within it).

For most kernel developers, cases 1 and 3 are transparent memory management
implementation details that are handled behind the scenes for you (we explore
these details below in the implementation section).

When **zapping** ranges, this can be done holding any one of the locks described
in the terminology section above - that is the mmap lock, the VMA lock or either
of the reverse mapping locks.

That is - as long as you keep the relevant VMA **stable**, you are good to go
ahead and zap memory in that VMA's range.

.. warning:: When **freeing** page tables, it must not be possible for VMAs
	     containing the ranges those page tables map to be accessible via
	     the reverse mapping.

	     The :c:func:`!free_pgtables` function removes the relevant VMAs
	     from the reverse mappings, but no other VMAs can be permitted to be
	     accessible and span the specified range.

Lock ordering
-------------

As we have multiple locks across the kernel which may or may not be taken at the
same time as explicit mm or VMA locks, we have to be wary of lock inversion, and
the **order** in which locks are acquired and released becomes very important.

.. note:: Lock inversion occurs when two threads need to acquire multiple locks,
   but in doing so inadvertently cause a mutual deadlock.

   For example, consider thread 1 which holds lock A and tries to acquire lock B,
   while thread 2 holds lock B and tries to acquire lock A.

   Both threads are now deadlocked on each other. However, had they attempted to
   acquire locks in the same order, one would have waited for the other to
   complete its work and no deadlock would have occurred.

The opening comment in `mm/rmap.c` describes in detail the required ordering of
locks within memory management code:

.. code-block::

  inode->i_rwsem	(while writing or truncating, not reading or faulting)
    mm->mmap_lock
      mapping->invalidate_lock (in filemap_fault)
        folio_lock
          hugetlbfs_i_mmap_rwsem_key (in huge_pmd_share, see hugetlbfs below)
            vma_start_write
              mapping->i_mmap_rwsem
                anon_vma->rwsem
                  mm->page_table_lock or pte_lock
                    swap_lock (in swap_duplicate, swap_info_get)
                      mmlist_lock (in mmput, drain_mmlist and others)
                      mapping->private_lock (in block_dirty_folio)
                          i_pages lock (widely used)
                            lruvec->lru_lock (in folio_lruvec_lock_irq)
                      inode->i_lock (in set_page_dirty's __mark_inode_dirty)
                      bdi.wb->list_lock (in set_page_dirty's __mark_inode_dirty)
                        sb_lock (within inode_lock in fs/fs-writeback.c)
                        i_pages lock (widely used, in set_page_dirty,
                                  in arch-dependent flush_dcache_mmap_lock,
                                  within bdi.wb->list_lock in __sync_single_inode)

Please check the current state of this comment which may have changed since the
time of writing of this document.

------------------------------
Locking Implementation Details
------------------------------

Page table locking details
--------------------------

In addition to the locks described in the terminology section above, we have
additional locks dedicated to page tables:

* **Higher level page table locks** - Higher level page tables, that is PGD, P4D
  and PUD each make use of the process address space granularity
  :c:member:`!mm->page_table_lock` lock when modified.

* **Fine-grained page table locks** - PMDs and PTEs each have fine-grained locks
  either kept within the folios describing the page tables or allocated
  separated and pointed at by the folios if :c:macro:`!ALLOC_SPLIT_PTLOCKS` is
  set. The PMD spin lock is obtained via :c:func:`!pmd_lock`, however PTEs are
  mapped into higher memory (if a 32-bit system) and carefully locked via
  :c:func:`!pte_offset_map_lock`.

These locks represent the minimum required to interact with each page table
level, but there are further requirements.

Locking rules
^^^^^^^^^^^^^

We establish basic locking rules when interacting with page tables:

* When changing a page table entry the page table lock for that page table
  **must** be held.
* Reads from and writes to page table entries must be appropriately atomic. See
  the section on atomicity below.
* Populating previously empty entries requires that the mmap or VMA locks are
  held, doing so with only rmap locks would risk a race with unmapping logic
  invoking :c:func:`!unmap_vmas`, so is forbidden.
* As mentioned above, zapping can be performed while simply keeping the VMA
  stable, that is holding any one of the mmap, VMA or rmap locks.
* Special care is required for PTEs, as on 32-bit architectures these must be
  mapped into high memory and additionally, careful consideration must be
  applied to racing with THP, migration or other concurrent kernel operations
  that might steal the entire PTE table from under us. All this is handled by
  :c:func:`!pte_offset_map_lock`.

There are additional rules applicable when moving page tables, which we discuss
in the section on this topic below.

.. note:: Interestingly, :c:func:`!pte_offset_map_lock` also maintains an RCU
          read lock over the mapping (and therefore combined mapping and
          locking) operation.

Atomicity
^^^^^^^^^

Page table entries must always be retrieved once and only once before being
interacted with, as we are operating concurrently with other operations and the
hardware.

Regardless of page table locks, the MMU hardware will update accessed and dirty
bits (and in some architectures, perhaps more), and kernel functionality like
GUP-fast locklessly traverses page tables, so we cannot safely assume that page
table locks give us exclusive access.

If we hold page table locks and are reading page table entries, then we need
only ensure that the compiler does not rearrange our loads. This is achieved via
:c:func:`!pXXp_get` functions - :c:func:`!pgdp_get`, :c:func:`!p4dp_get`,
:c:func:`!pudp_get`, :c:func:`!pmdp_get`, and :c:func:`!ptep_get`.

Each of these uses :c:func:`!READ_ONCE` to guarantee that the compiler reads
the page table entry only once.

However, if we wish to manipulate an existing page table entry and care about
the previously stored data, we must go further and use an hardware atomic
operation as, for example, in :c:func:`!ptep_get_and_clear`.

Equally, operations that do not rely on the page table locks, such as GUP-fast
(for instance see :c:func:`!gup_fast` and its various page table level handlers
like :c:func:`!gup_fast_pte_range`), must very carefully interact with page
table entries, using functions such as :c:func:`!ptep_get_lockless` and
equivalent for higher page table levels.

Writes to page table entries must also be appropriately atomic, as established
by :c:func:`!set_pXX` functions - :c:func:`!set_pgd`, :c:func:`!set_p4d`,
:c:func:`!set_pud`, :c:func:`!set_pmd`, and :c:func:`!set_pte`.


Page table installation
^^^^^^^^^^^^^^^^^^^^^^^

When allocating a P4D, PUD or PMD and setting the relevant entry in the above
PGD, P4D or PUD, the :c:member:`!mm->page_table_lock` must be held. This is
acquired in :c:func:`!__p4d_alloc`, :c:func:`!__pud_alloc` and
:c:func:`!__pmd_alloc` respectively.

.. note:: :c:func:`!__pmd_alloc` actually invokes :c:func:`!pud_lock` and
   :c:func:`!pud_lockptr` in turn, however at the time of writing it ultimately
   references the :c:member:`!mm->page_table_lock`.

Allocating a PTE will either use the :c:member:`!mm->page_table_lock` or, if
:c:macro:`!USE_SPLIT_PMD_PTLOCKS` is defined, used a lock embedded in the PMD
physical page metadata in the form of a :c:struct:`!struct ptdesc`, acquired by
:c:func:`!pmd_ptdesc` called from :c:func:`!pmd_lock` and ultimately
:c:func:`!__pte_alloc`.

Finally, modifying the contents of the PTE has special treatment, as this is a
lock that we must acquire whenever we want stable and exclusive access to
entries pointing to data pages within a PTE, especially when we wish to modify
them.

This is performed via :c:func:`!pte_offset_map_lock` which carefully checks to
ensure that the PTE hasn't changed from under us, ultimately invoking
:c:func:`!pte_lockptr` to obtain a spin lock at PTE granularity contained within
the :c:struct:`!struct ptdesc` associated with the physical PTE page. The lock
must be released via :c:func:`!pte_unmap_unlock`.

.. note:: There are some variants on this, such as
   :c:func:`!pte_offset_map_rw_nolock` when we know we hold the PTE stable but
   for brevity we do not explore this.  See the comment for
   :c:func:`!__pte_offset_map_lock` for more details.

When modifying data in ranges we typically only wish to allocate higher page
tables as necessary, using these locks to avoid races or overwriting anything,
and set/clear data at the PTE level as required (for instance when page faulting
or zapping).

Page table freeing
^^^^^^^^^^^^^^^^^^

Tearing down page tables themselves is something that requires significant
care. There must be no way that page tables designated for removal can be
traversed or referenced by concurrent tasks.

It is insufficient to simply hold an mmap write lock and VMA lock (which will
prevent racing faults, and rmap operations), as a file-backed mapping can be
truncated under the :c:struct:`!struct address_space` i_mmap_lock alone.

As a result, no VMA which can be accessed via the reverse mapping (either
anon_vma or the :c:member:`!struct address_space->i_mmap` interval tree) can
have its page tables torn down.

The operation is typically performed via :c:func:`!free_pgtables`, which assumes
either the mmap write lock has been taken (as specified by its
:c:member:`!mm_wr_locked` parameter), or that the VMA is already unreachable.

It carefully removes the VMA from all reverse mappings, however it's important
that no new ones overlap these or any route remain to permit access to addresses
within the range whose page tables are being torn down.

As a result of these careful conditions, note that page table entries are
cleared without page table locks, as it is assumed that all of these precautions
have already been taken (in the :c:func:`!pgd_clear`, :c:func:`!p4d_clear`,
:c:func:`!pud_clear`, and :c:func:`!pmd_clear` functions - note that at this
stage it is assumed that PTE entries have been zapped).

.. note:: It is possible for leaf page tables to be torn down, independent of
          the page tables above it, as is done by
          :c:func:`!retract_page_tables`, which is performed under the i_mmap
          read lock, PMD, and PTE page table locks, without this level of care.

Page table moving
^^^^^^^^^^^^^^^^^

Some functions manipulate page table levels above PMD (that is PUD, P4D and PGD
page tables). Most notable of these is :c:func:`!mremap`, which is capable of
moving higher level page tables.

In these instances, it is either required that **all** locks are taken, that is
the mmap lock, the VMA lock and the relevant rmap lock, or that the mmap lock
and VMA locks are taken and some other measure is taken to avoid rmap races (see
the comment in :c:func:`!move_ptes` in the :c:func:`!mremap` implementation for
details of how this is handled in this instance).

You can observe that in the :c:func:`!mremap` implementation in the functions
:c:func:`!take_rmap_locks` and :c:func:`!drop_rmap_locks` which perform the rmap
side of lock acquisition, invoked ultimately by :c:func:`!move_page_tables`.

VMA lock internals
------------------

This kind of locking is entirely optimistic - if the lock is contended or a
competing write has started, then we do not obtain a read lock.

The :c:func:`!lock_vma_under_rcu` function first calls :c:func:`!rcu_read_lock`
to ensure that the VMA is acquired in an RCU critical section, then attempts to
VMA lock it via :c:func:`!vma_start_read`, before releasing the RCU lock via
:c:func:`!rcu_read_unlock`.

VMA read locks hold the read lock on the :c:member:`!vma->vm_lock` semaphore for
their duration and the caller of :c:func:`!lock_vma_under_rcu` must release it
via :c:func:`!vma_end_read`.

VMA **write** locks are acquired via :c:func:`!vma_start_write` in instances where a
VMA is about to be modified, unlike :c:func:`!vma_start_read` the lock is always
acquired. An mmap write lock **must** be held for the duration of the VMA write
lock, releasing or downgrading the mmap write lock also releases the VMA write
lock so there is no :c:func:`!vma_end_write` function.

Note that a semaphore write lock is not held across a VMA lock. Rather, a
sequence number is used for serialisation, and the write semaphore is only
acquired at the point of write lock to update this.

This ensures the semantics we require - VMA write locks provide exclusive write
access to the VMA.

The VMA lock mechanism is designed to be a lightweight means of avoiding the use
of the heavily contended mmap lock. It is implemented using a combination of a
read/write semaphore and sequence numbers belonging to the containing
:c:struct:`!struct mm_struct` and the VMA.

Read locks are acquired via :c:func:`!vma_start_read`, which is an optimistic
operation, i.e. it tries to acquire a read lock but returns false if it is
unable to do so. At the end of the read operation, :c:func:`!vma_end_read` is
called to release the VMA read lock. This can be done under RCU alone.

Writing requires the mmap to be write-locked and the VMA lock to be acquired via
:c:func:`!vma_start_write`, however the write lock is released by the termination or
downgrade of the mmap write lock so no :c:func:`!vma_end_write` is required.

All this is achieved by the use of per-mm and per-VMA sequence counts, which are
used in order to reduce complexity, especially for operations which write-lock
multiple VMAs at once.

If the mm sequence count, :c:member:`!mm->mm_lock_seq` is equal to the VMA
sequence count :c:member:`!vma->vm_lock_seq` then the VMA is write-locked. If
they differ, then they are not.

Each time an mmap write lock is acquired in :c:func:`!mmap_write_lock`,
:c:func:`!mmap_write_lock_nested`, :c:func:`!mmap_write_lock_killable`, the
:c:member:`!mm->mm_lock_seq` sequence number is incremented via
:c:func:`!mm_lock_seqcount_begin`.

Each time the mmap write lock is released in :c:func:`!mmap_write_unlock` or
:c:func:`!mmap_write_downgrade`, :c:func:`!vma_end_write_all` is invoked which
also increments :c:member:`!mm->mm_lock_seq` via
:c:func:`!mm_lock_seqcount_end`.

This way, we ensure regardless of the VMA's sequence number count, that a write
lock is not incorrectly indicated (since we increment the sequence counter on
acquiring the mmap write lock, which is required in order to obtain a VMA write
lock), and that when we release an mmap write lock, we efficiently release
**all** VMA write locks contained within the mmap at the same time.

The exclusivity of the mmap write lock ensures this is what we want, as there
would never be a reason to persist per-VMA write locks across multiple mmap
write lock acquisitions.

Each time a VMA read lock is acquired, we acquire a read lock on the
:c:member:`!vma->vm_lock` read/write semaphore and hold it, while checking that
the sequence count of the VMA does not match that of the mm.

If it does, the read lock fails. If it does not, we hold the lock, excluding
writers, but permitting other readers, who will also obtain this lock under RCU.

Importantly, maple tree operations performed in :c:func:`!lock_vma_under_rcu`
are also RCU safe, so the whole read lock operation is guaranteed to function
correctly.

On the write side, we acquire a write lock on the :c:member:`!vma->vm_lock`
read/write semaphore, before setting the VMA's sequence number under this lock,
also simultaneously holding the mmap write lock.

This way, if any read locks are in effect, :c:func:`!vma_start_write` will sleep
until these are finished and mutual exclusion is achieved.

After setting the VMA's sequence number, the lock is released, avoiding
complexity with a long-term held write lock.

This clever combination of a read/write semaphore and sequence count allows for
fast RCU-based per-VMA lock acquisition (especially on page fault, though
utilised elsewhere) with minimal complexity around lock ordering.

mmap write lock downgrading
---------------------------

When an mmap write lock is held, one has exclusive access to resources within
the mmap (with the usual caveats about requiring VMA write locks to avoid races
with tasks holding VMA read locks).

It is then possible to **downgrade** from a write lock to a read lock via
:c:func:`!mmap_write_downgrade` which, similar to :c:func:`!mmap_write_unlock`,
implicitly terminates all VMA write locks via :c:func:`!vma_end_write_all`, but
importantly does not relinquish the mmap lock while downgrading, therefore
keeping the locked virtual address space stable.

An interesting consequence of this is that downgraded locks will be exclusive
against any other task possessing a downgraded lock (since they'd have to
acquire a write lock first to do so, and the lock now being a read lock prevents
this).

For clarity, mapping read (R)/downgraded write (D)/write (W) locks against one
another showing which locks exclude the others:

.. list-table:: Lock exclusivity
   :widths: 5 5 5 5
   :header-rows: 1
   :stub-columns: 1

   * -
     - R
     - D
     - W
   * - R
     - N
     - N
     - Y
   * - D
     - N
     - Y
     - Y
   * - W
     - Y
     - Y
     - Y

Here a Y indicates the locks in the matching row/column exclude one another, and
N indicates that they do not.

Stack expansion
---------------

Stack expansion throws up additional complexities in that we cannot permit there
to be racing page faults, as a result we invoke :c:func:`!vma_start_write` to
prevent this in :c:func:`!expand_downwards` or :c:func:`!expand_upwards`.
