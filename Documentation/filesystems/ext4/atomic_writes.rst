.. SPDX-License-Identifier: GPL-2.0
.. _atomic_writes:

Atomic Block Writes
-------------------------

Introduction
~~~~~~~~~~~~

Atomic (untorn) block writes ensure that either the entire write is committed
to disk or none of it is. This prevents "torn writes" during power loss or
system crashes. The ext4 filesystem supports atomic writes (only with Direct
I/O) on regular files with extents, provided the underlying storage device
supports hardware atomic writes. This is supported in the following two ways:

1. **Single-fsblock Atomic Writes**:
   EXT4's supports atomic write operations with a single filesystem block since
   v6.13. In this the atomic write unit minimum and maximum sizes are both set
   to filesystem blocksize.
   e.g. doing atomic write of 16KB with 16KB filesystem blocksize on 64KB
   pagesize system is possible.

2. **Multi-fsblock Atomic Writes with Bigalloc**:
   EXT4 now also supports atomic writes spanning multiple filesystem blocks
   using a feature known as bigalloc. The atomic write unit's minimum and
   maximum sizes are determined by the filesystem block size and cluster size,
   based on the underlying device’s supported atomic write unit limits.

Requirements
~~~~~~~~~~~~

Basic requirements for atomic writes in ext4:

 1. The extents feature must be enabled (default for ext4)
 2. The underlying block device must support atomic writes
 3. For single-fsblock atomic writes:

    1. A filesystem with appropriate block size (up to the page size)
 4. For multi-fsblock atomic writes:

    1. The bigalloc feature must be enabled
    2. The cluster size must be appropriately configured

NOTE: EXT4 does not support software or COW based atomic write, which means
atomic writes on ext4 are only supported if underlying storage device supports
it.

Multi-fsblock Implementation Details
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The bigalloc feature changes ext4 to use clustered allocations. With bigalloc
each bit within block bitmap represents clusters (power of 2 number of blocks)
rather than individual filesystem blocks. EXT4 supports atomic writes using
bigalloc by making sure that atomic write min and max are within [blocksize,
clustersize].

Here is the block allocation strategy in bigalloc for atomic writes:

 * For regions with fully mapped extents, no additional allocation is needed
 * For append writes, a new mapped extent is allocated
 * For regions that are entirely holes, unwritten extent is created
 * For large unwritten extents, the extent gets split into two unwritten
   extents of appropriate requested size
 * For mixed mapping regions (combinations of holes, unwritten extents, or
   mapped extents), ext4_map_blocks() is called in a loop with
   EXT4_GET_BLOCKS_ZERO flag to convert the region into a single contiguous
   mapped extent

Note: Writing on a single contiguous underlying extent, whether mapped or
unwritten, is not inherently problematic. However, writing to a mixed mapping
region (i.e. one containing a combination of mapped and unwritten extents)
must be avoided when performing atomic writes.

The reason is that, atomic writes when issued via pwritev2() with the RWF_ATOMIC
flag, requires that either all data is written or none at all. In the event of
a system crash or unexpected power loss during the write operation, the affected
region (when later read) must reflect either the complete old data or the
complete new data, but never a mix of both.

To enforce this guarantee, we ensure that the write target is backed by
a single, contiguous extent before any data is written. This is critical because
ext4 defers the conversion of unwritten extents to written extents until the I/O
completion path (typically in ->end_io()). If a write is allowed to proceed over
a mixed mapping region (with mapped and unwritten extents) and a failure occurs
mid-write, the system could observe partially updated regions after reboot, i.e.
new data over mapped areas, and stale (old) data over unwritten extents that
were never marked written. This violates the atomicity and/or torn write
prevention guarantee.

To prevent such torn writes, ext4 proactively allocates a single contiguous
extent for the entire requested region in ``ext4_iomap_alloc`` via
``ext4_map_blocks_atomic()``. Only after this allocation, is the write
operation performed by iomap.

Handling Split Extents Across Leaf Blocks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

There can be a special edge case where we have logically and physically
contiguous extents stored in separate leaf nodes of the on-disk extent tree.
This occurs because on-disk extent tree merges only happens within the leaf
blocks except for a case where we have 2-level tree which can get merged and
collapsed entirely into the inode.
If such a layout exists and, in the worst case, the extent status cache entries
are reclaimed due to memory pressure, ``ext4_map_blocks()`` may never return
a single contiguous extent for these split leaf extents.

To address this edge case, a new get block flag
``EXT4_GET_BLOCKS_QUERY_LEAF_BLOCKS flag`` is added to enhance the
``ext4_map_query_blocks()`` lookup behavior.

This new get block flag allows ``ext4_map_blocks()`` to first checks if there is
an entry in the extent status cache for the full range.
If not present, it consults the on-disk extent tree using
``ext4_map_query_blocks()``.
If the located extent is at the end of a leaf node, it probes the next logical
block (lblk) to detect a contiguous extent in the adjacent leaf.

For now only one additional leaf block is queried to maintain efficiency, as
atomic writes are typically constrained to small sizes
(e.g. [blocksize, clustersize]).


Handling Journal transactions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To support multi-fsblock atomic writes, we ensure enough journal credits are
reserved during:

 1. Block allocation time in ``ext4_iomap_alloc()``. We first query if there
    could be a mixed mapping for the underlying requested range. If yes, then we
    reserve credits of up to ``m_len``, assuming every alternate block can be
    an unwritten extent followed by a hole.

 2. During ``->end_io()`` call, we make sure a single transaction is started for
    doing unwritten-to-written conversion. The loop for conversion is mainly
    only required to handle a split extent across leaf blocks.

How to
------

Creating Filesystems with Atomic Write Support
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For single-fsblock atomic writes with a larger block size
(on systems with block size < page size):

.. code-block:: bash

    # Create an ext4 filesystem with a 16KB block size
    # (requires page size >= 16KB)
    mkfs.ext4 -b 16384 /dev/device

For multi-fsblock atomic writes with bigalloc:

.. code-block:: bash

    # Create an ext4 filesystem with bigalloc and 64KB cluster size
    mkfs.ext4 -F -O bigalloc -b 4096 -C 65536 /dev/device

Where ``-b`` specifies the block size, ``-C`` specifies the cluster size in bytes,
and ``-O bigalloc`` enables the bigalloc feature.

Application Interface
~~~~~~~~~~~~~~~~~~~~~

Applications can use the ``pwritev2()`` system call with the ``RWF_ATOMIC`` flag
to perform atomic writes:

.. code-block:: c

    pwritev2(fd, iov, iovcnt, offset, RWF_ATOMIC);

The write must be aligned to the filesystem's block size and not exceed the
filesystem's maximum atomic write unit size.
See ``generic_atomic_write_valid()`` for more details.

``statx()`` system call with ``STATX_WRITE_ATOMIC`` flag can provides following
details:

 * ``stx_atomic_write_unit_min``: Minimum size of an atomic write request.
 * ``stx_atomic_write_unit_max``: Maximum size of an atomic write request.
 * ``stx_atomic_write_segments_max``: Upper limit for segments. Tthe number of
   separate memory buffers that can be gathered into a write operation
   (e.g., the iovcnt parameter for IOV_ITER). Currently, this is always set to one.

The STATX_ATTR_WRITE_ATOMIC flag in ``statx->attributes`` is set if atomic
writes are supported.

Hardware Support
----------------

The underlying storage device must support atomic write operations.
Modern NVMe and SCSI devices often provide this capability.
The Linux kernel exposes this information through sysfs:

* ``/sys/block/<device>/queue/atomic_write_unit_min`` - Minimum atomic write size
* ``/sys/block/<device>/queue/atomic_write_unit_max`` - Maximum atomic write size

Nonzero values for these attributes indicate that the device supports
atomic writes.

See Also
--------

* :doc:`bigalloc` - Documentation on the bigalloc feature
* :doc:`allocators` - Documentation on block allocation in ext4
* Support for atomic block writes in 6.13:
  https://lwn.net/Articles/1009298/
