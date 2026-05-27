.. SPDX-License-Identifier: GPL-2.0

=================
KVM Lock Overview
=================

1. Acquisition Orders
---------------------

The acquisition orders for mutexes are as follows:

- cpus_read_lock() is taken outside kvm_lock
    - Taking cpus_read_lock() outside of kvm_lock is problematic,
      despite it being the official ordering, as it is quite easy to
      unknowingly trigger cpus_read_lock() while holding kvm_lock.
      Use caution when walking vm_list, e.g. avoid complex operations
      when possible.

- kvm_usage_lock is taken outside cpus_read_lock()

- kvm->lock is taken outside vcpu->mutex

- kvm->lock is taken outside kvm->slots_lock and kvm->irq_lock
    - kvm->slots_lock is taken outside kvm->irq_lock, though acquiring
      them together is quite rare.

- vcpu->mutex is taken outside kvm->slots_lock and kvm->slots_arch_lock

- kvm->mn_active_invalidate_count ensures that pairs of
  invalidate_range_start() and invalidate_range_end() callbacks
  use the same memslots array.  kvm->slots_lock and kvm->slots_arch_lock
  are taken on the waiting side when modifying memslots, so MMU notifiers
  must not take either kvm->slots_lock or kvm->slots_arch_lock.

For SRCU:

- ``synchronize_srcu(&kvm->srcu)`` is called inside critical sections
  for vcpu->mutex and kvm->slots_lock.  (For example, when there is a
  ``KVM_REQ_APICV_UPDATE`` request, ``vcpu->mutex`` is held in
  ``kvm_vcpu_ioctl()``, and then when the memslots get updated,
  ``kvm->slots_lock`` is taken.)  These locks _cannot_ be taken inside
  a kvm->srcu read-side critical section; that is, the following is
  broken::

      srcu_read_lock(&kvm->srcu);
      mutex_lock(&kvm->slots_lock);

- kvm->slots_arch_lock instead is released before the call to
  ``synchronize_srcu()``.  It _can_ therefore be taken inside a
  kvm->srcu read-side critical section, for example while processing
  a vmexit.

On x86:

- vcpu->mutex is taken outside kvm->arch.hyperv.hv_lock and kvm->arch.xen.xen_lock

- kvm->arch.mmu_lock is an rwlock; critical sections for
  kvm->arch.tdp_mmu_pages_lock and kvm->arch.mmu_unsync_pages_lock must
  also take kvm->arch.mmu_lock

Everything else is a leaf: no other lock is taken inside the critical
sections.

2. Exception
------------

The general rule in KVM is that any modification to shadow page tables
(and their entries (SPTEs)) must be protected by ``kvm->mmu_lock``,
with the exceptions described below.

2.1. Fast page fault
^^^^^^^^^^^^^^^^^^^^

Fast page fault is the fast path which fixes the guest page fault out of
the mmu-lock on x86. Currently, the page fault can be fast in one of the
following two cases:

1. Access Tracking: The SPTE is not present, but it is marked for access
   tracking. That means we need to restore the saved R/X bits. This is
   described in more detail later below.

2. Write-Protection: The SPTE is present and the fault is caused by
   write-protect. That means we just need to change the W bit of the spte.

What we use to avoid all the races is the Host-writable bit and MMU-writable bit
on the spte:

- Host-writable means the gfn is writable in the host kernel page tables and in
  its KVM memslot.
- MMU-writable means the gfn is writable in the guest's mmu and it is not
  write-protected by shadow page write-protection.

On fast page fault path, we will use cmpxchg to atomically set the spte W
bit if spte.HOST_WRITEABLE = 1 and spte.WRITE_PROTECT = 1, to restore the saved
R/X bits if for an access-traced spte, or both. This is safe because whenever
changing these bits can be detected by cmpxchg.

But we need carefully check these cases:

1) The mapping from gfn to pfn

The mapping from gfn to pfn may be changed since we can only ensure the pfn
is not changed during cmpxchg. This is a ABA problem, for example, below case
will happen:

+------------------------------------------------------------------------+
| At the beginning::                                                     |
|                                                                        |
|	gpte = gfn1                                                      |
|	gfn1 is mapped to pfn1 on host                                   |
|	spte is the shadow page table entry corresponding with gpte and  |
|	spte = pfn1                                                      |
+------------------------------------------------------------------------+
| On fast page fault path:                                               |
+------------------------------------+-----------------------------------+
| CPU 0:                             | CPU 1:                            |
+------------------------------------+-----------------------------------+
| ::                                 |                                   |
|                                    |                                   |
|   old_spte = *spte;                |                                   |
+------------------------------------+-----------------------------------+
|                                    | pfn1 is swapped out::             |
|                                    |                                   |
|                                    |    spte = 0;                      |
|                                    |                                   |
|                                    | pfn1 is re-alloced for gfn2.      |
|                                    |                                   |
|                                    | gpte is changed to point to       |
|                                    | gfn2 by the guest::               |
|                                    |                                   |
|                                    |    spte = pfn1;                   |
+------------------------------------+-----------------------------------+
| ::                                                                     |
|                                                                        |
|   if (cmpxchg(spte, old_spte, old_spte+W)                              |
|	mark_page_dirty(vcpu->kvm, gfn1)                                 |
|            OOPS!!!                                                     |
+------------------------------------------------------------------------+

We dirty-log for gfn1, that means gfn2 is lost in dirty-bitmap.

For direct sp, we can easily avoid it since the spte of direct sp is fixed
to gfn.  For indirect sp, we disabled fast page fault for simplicity.

A solution for indirect sp could be to pin the gfn before the cmpxchg.  After
the pinning:

- We have held the refcount of pfn; that means the pfn can not be freed and
  be reused for another gfn.
- The pfn is writable and therefore it cannot be shared between different gfns
  by KSM.

Then, we can ensure the dirty bitmaps is correctly set for a gfn.

2) Dirty bit tracking

In the original code, the spte can be fast updated (non-atomically) if the
spte is read-only and the Accessed bit has already been set since the
Accessed bit and Dirty bit can not be lost.

But it is not true after fast page fault since the spte can be marked
writable between reading spte and updating spte. Like below case:

+-------------------------------------------------------------------------+
| At the beginning::                                                      |
|                                                                         |
|  spte.W = 0                                                             |
|  spte.Accessed = 1                                                      |
+-------------------------------------+-----------------------------------+
| CPU 0:                              | CPU 1:                            |
+-------------------------------------+-----------------------------------+
| In mmu_spte_update()::              |                                   |
|                                     |                                   |
|  old_spte = *spte;                  |                                   |
|                                     |                                   |
|                                     |                                   |
|  /* 'if' condition is satisfied. */ |                                   |
|  if (old_spte.Accessed == 1 &&      |                                   |
|       old_spte.W == 0)              |                                   |
|     spte = new_spte;                |                                   |
+-------------------------------------+-----------------------------------+
|                                     | on fast page fault path::         |
|                                     |                                   |
|                                     |    spte.W = 1                     |
|                                     |                                   |
|                                     | memory write on the spte::        |
|                                     |                                   |
|                                     |    spte.Dirty = 1                 |
+-------------------------------------+-----------------------------------+
|  ::                                 |                                   |
|                                     |                                   |
|   else                              |                                   |
|     old_spte = xchg(spte, new_spte);|                                   |
|   if (old_spte.Accessed &&          |                                   |
|       !new_spte.Accessed)           |                                   |
|     flush = true;                   |                                   |
|   if (old_spte.Dirty &&             |                                   |
|       !new_spte.Dirty)              |                                   |
|     flush = true;                   |                                   |
|     OOPS!!!                         |                                   |
+-------------------------------------+-----------------------------------+

The Dirty bit is lost in this case.

In order to avoid this kind of issue, we always treat the spte as "volatile"
if it can be updated out of mmu-lock [see spte_needs_atomic_update()]; it means
the spte is always atomically updated in this case.

3) flush tlbs due to spte updated

If the spte is updated from writable to read-only, we should flush all TLBs,
otherwise rmap_write_protect will find a read-only spte, even though the
writable spte might be cached on a CPU's TLB.

As mentioned before, the spte can be updated to writable out of mmu-lock on
fast page fault path. In order to easily audit the path, we see if TLBs needing
to be flushed caused this reason in mmu_spte_update() since this is a common
function to update spte (present -> present).

Since the spte is "volatile" if it can be updated out of mmu-lock, we always
atomically update the spte and the race caused by fast page fault can be avoided.
See the comments in spte_needs_atomic_update() and mmu_spte_update().

2.2 Lockless Access Tracking
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This is used for Intel CPUs that are using EPT but do not support the EPT A/D
bits. In this case, PTEs are tagged as A/D disabled (using ignored bits), and
when the KVM MMU notifier is called to track accesses to a page (via
kvm_mmu_notifier_clear_flush_young), it marks the PTE not-present in hardware
by clearing the RWX bits in the PTE and storing the original R & X bits in more
unused/ignored bits. When the VM tries to access the page later on, a fault is
generated and the fast page fault mechanism described above is used to
atomically restore the PTE to a Present state. The W bit is not saved when the
PTE is marked for access tracking and during restoration to the Present state,
the W bit is set depending on whether or not it was a write access. If it
wasn't, then the W bit will remain clear until a write access happens, at which
time it will be set using the Dirty tracking mechanism described above.

3. Reference
------------

``kvm_lock``
^^^^^^^^^^^^

:Type:		mutex
:Arch:		any
:Protects:	- vm_list

``kvm_usage_lock``
^^^^^^^^^^^^^^^^^^

:Type:		mutex
:Arch:		any
:Protects:	- kvm_usage_count
		- hardware virtualization enable/disable
:Comment:       ``kvm_usage_count`` serves to deduplicate hardware
    virtualization enabling and disabling requests from different VMs
    being created.

    Hardware virtualization enabling/disabling requires taking
    ``cpus_read_lock()``.

    ``kvm_lock`` used to also protect ``kvm_usage_count``, but other
    parts of the Linux kernel holding ``cpus_read_lock()`` need to
    call into KVM to ensure that VM state remains consistent with the
    host's state. For example, when the CPU frequency changes, KVM is
    notified. ``kvmclock_cpufreq_notifier()`` takes ``kvm_lock`` to
    iterate ``vm_list``.

    To decouple these, use different locks, ``kvm_lock`` for
    ``vm_list`` and ``kvm_usage_lock`` for enabling/disabling hardware
    virtualization.

``kvm->mn_invalidate_lock``
^^^^^^^^^^^^^^^^^^^^^^^^^^^

:Type:          spinlock_t
:Arch:          any
:Protects:      mn_active_invalidate_count, mn_memslots_update_rcuwait

``kvm_arch::tsc_write_lock``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

:Type:		raw_spinlock_t
:Arch:		x86
:Protects:	- kvm_arch::{last_tsc_write,last_tsc_nsec,last_tsc_offset}
		- tsc offset in vmcb
:Comment:	'raw' because updating the tsc offsets must not be preempted.

``kvm->mmu_lock``
^^^^^^^^^^^^^^^^^
:Type:		spinlock_t or rwlock_t
:Arch:		any
:Protects:	- shadow page/shadow tlb entry
:Comment:	it is a spinlock since it is used in mmu notifier.

``kvm->srcu``
^^^^^^^^^^^^^
:Type:		srcu lock
:Arch:		any
:Protects:	- kvm->memslots
		- kvm->buses
:Comment:	The srcu read lock must be held while accessing memslots (e.g.
		when using gfn_to_* functions) and while accessing in-kernel
		MMIO/PIO address->device structure mapping (kvm->buses).
		The srcu index can be stored in kvm_vcpu->srcu_idx per vcpu
		if it is needed by multiple functions.

``kvm->slots_arch_lock``
^^^^^^^^^^^^^^^^^^^^^^^^
:Type:          mutex
:Arch:          any (only needed on x86 though)
:Protects:      any arch-specific fields of memslots that have to be modified
                in a ``kvm->srcu`` read-side critical section.
:Comment:       must be held before reading the pointer to the current memslots,
                until after all changes to the memslots are complete

``wakeup_vcpus_on_cpu_lock``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^
:Type:		spinlock_t
:Arch:		x86
:Protects:	wakeup_vcpus_on_cpu
:Comment:	This is a per-CPU lock and it is used for VT-d posted-interrupts.
		When VT-d posted-interrupts are supported and the VM has assigned
		devices, we put the blocked vCPU on the list blocked_vcpu_on_cpu
		protected by blocked_vcpu_on_cpu_lock. When VT-d hardware issues
		wakeup notification event since external interrupts from the
		assigned devices happens, we will find the vCPU on the list to
		wakeup.

``vendor_module_lock``
^^^^^^^^^^^^^^^^^^^^^^
:Type:		mutex
:Arch:		x86
:Protects:	loading a vendor module (kvm_amd or kvm_intel)
:Comment:	Exists because using kvm_lock leads to deadlock.  kvm_lock is taken
    in notifiers, e.g. __kvmclock_cpufreq_notifier(), that may be invoked while
    cpu_hotplug_lock is held, e.g. from cpufreq_boost_trigger_state(), and many
    operations need to take cpu_hotplug_lock when loading a vendor module, e.g.
    updating static calls.

4. Synchronization while managing guest faults
----------------------------------------------

This section explains the intersection of these synchronization mechanisms:

- ``kvm->srcu`` (for memslots)
- ``kvm->mmu_invalidate_*`` (pending invalidations)
- ``kvm->mn_*`` (synchronization for ``kvm->mmu_invalidate_*``)

4.1 Overview
^^^^^^^^^^^^

KVM resolves guest page faults by translating the Guest Frame Number (GFN) into
a Page Frame Number (PFN) via memslots and then populating its shadow page
tables with the resulting mapping.

While handling the guest page fault, KVM must ensure a consistent view of the
active memslots container, so KVM takes ``srcu_read_lock(&kvm->srcu);``.

Guest page fault handling can race with some request from host userspace to
invalidate shadow page tables. These requests originate from a few places, such
as

1. MMU Notifiers: KVM registers callbacks with the kernel’s memory management
   subsystem to know when there are changes to mappings in the host userspace
   page tables.
2. Memslot Updates: The host userspace VMM, such as QEMU may use the
   ``KVM_SET_USER_MEMORY_REGION`` ioctl to add, delete, or move a memslot. KVM
   must zap the affected shadow page tables to ensure the guest doesn't access
   stale mappings.
3. Memory Attribute Changes: The ``KVM_SET_MEMORY_ATTRIBUTES`` ioctl allows
   userspace to change attributes for a range of guest memory (e.g., setting a
   range as "private" for Confidential Computing). This also requires
   invalidating existing shadow mappings.

When such a race occurs, KVM optimistically allows the faulting logic to
proceed, but just before committing the fault, KVM will check for a pending
invalidation, and retry the fault process if there is a pending invalidation
affecting the GFN where the fault occurred.

4.2 Tracking pending invalidations with ``kvm->mmu_invalidate*`` fields
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A "pending invalidation" is determined using a combination of

- ``kvm->mmu_invalidate_in_progress``
- ``kvm->mmu_invalidate_range_start`` and ``kvm->mmu_invalidate_range_end``
- ``kvm->mmu_invalidate_seq``

``is_page_fault_stale()`` shows how the above fields are used to determine if
the page fault is stale and requires a retry.

To protect the above combination of fields, a lock is used, which is the
``kvm->mmu_lock``.

4.2.1 Derived information vs pending invalidations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Generally, the result of any information derived from GFN aka page
attribute/page metadata lookups may race with invalidations. Here are some
examples of lookups:

- ``host_pfn_mapping_level()`` uses memslot information to find the mapping
  level of pages in host userspace page tables. If there's an invalidation, the
  pages that were mapped would no longer be mapped and hence the mapping level
  result would be stale.

There are several ways to ensure valid results:

- Check ``mmu_invalidate_retry_gfn()`` after grabbing the result, before
  consuming it. In this case, ``mmu_lock`` doesn't need to be held during the
  lookup, but it does need to be held while checking the MMU notifier. KVM's
  guest page fault handling uses this option.
- Hold ``mmu_lock`` AND ensure there is no in-progress MMU notifier invalidation
  event for the hva. This can be done by explicit checking the MMU notifier or
  by ensuring that KVM already has a valid mapping that covers the
  hva. ``kvm_mmu_recover_huge_pages()`` uses this option.

4.3 Further optimization: ignoring invalidations if there is no matching memslot
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Invalidation is only really required when the invalidated memory range overlaps
with some memslot. Without a matching memslot, the invalidation request could
actually just be ignored. Hence, KVM only updates the ``kvm->mmu_invalidate_*``
fields and takes ``kvm->mmu_lock`` if it finds a matching memslot.

This creates another problem: if memslots are updated while there is an ongoing
invalidation, then the updates to the fields and the lock would be imbalanced.

4.4 Synchronization for invalidation lock/fields: ``kvm->mn_*``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

To make sure the updates to the invalidation lock/fields are balanced, KVM has a
further layer of synchronization. ``kvm_swap_active_memslots()`` enforces that
changes to memslots are only committed once all pending invalidations are
complete.

In other words, ``kvm->mn_*`` ensures the following does not happen:

1. Some memslot existed, causing a pending invalidation request to be recorded
   in the ``kvm->mmu_invalidate_*`` fields
2. Memslot got removed, so the invalidation request was never removed from the
   ``kvm->mmu_invalidate_*`` fields.

In addition, ``kvm_swap_active_memslots()`` also enforces that changes to
memslots are complete before doing ``synchronize_srcu(&kvm->srcu)`` to make sure
running readers of the old memslots container are done before freeing it.
