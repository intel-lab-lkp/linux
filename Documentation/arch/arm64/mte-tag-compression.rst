.. SPDX-License-Identifier: GPL-2.0

==================================================
Tag Compression for Memory Tagging Extension (MTE)
==================================================

This document describes the algorithm used to compress memory tags used by the
ARM Memory Tagging Extension (MTE)

Introduction
============

MTE assigns tags to memory pages: for 4K pages those tags occupy 128 bytes
(256 4-bit tags each corresponding to a 16-byte MTE granule), for 16K pages -
512 bytes, for 64K pages - 2048 bytes. By default, MTE carves out 3.125% (1/16)
of the available physical memory to store the tags.

When MTE pages are saved to swap, their tags need to be stored in the kernel
memory. If the system swap is used heavily, these tags may take a substantial
portion of the physical memory. To reduce memory waste,
``CONFIG_ARM64_MTE_COMP`` allows the kernel to store the tags in compressed
form.

Implementation details
======================

The algorithm attempts to compress an array of ``MTE_PAGE_TAG_STORAGE``
tag bytes into a byte sequence that can be stored in one of the smaller size
class allocations (for 4K pages those are 16-, 32-, or 64-byte allocations).
A special case is storing the tags inline in an 8-byte pointer.

Tag manipulation and storage
----------------------------

Tags for swapped pages are stored in an XArray that maps swap entries to 63-bit
values (see ``arch/arm64/mm/mteswap.c``). In the case when
``CONFIG_ARM64_MTE_COMP=n``, these values contain pointers to uncompressed
buffers allocated with kmalloc(). Otherwise, they are 63-bit handles used by the
functions declared in ``arch/arm64/include/asm/mtecomp.h``:

- mte_compress() compresses the given ``MTE_PAGE_TAG_STORAGE``-byte ``tags``
  buffer, allocates storage for it, and returns an opaque handle addressing
  that storage;
- mte_decompress() decompresses the tags addressed by ``handle``
  and fills the ``MTE_PAGE_TAG_STORAGE``-byte ``tags`` buffer;
- mte_release_handle() releases the storage handle returned by
  mte_compress() (so that this handle cannot be used anymore);
- mte_storage_size() calculates the size occupied by the tags addressed
  by ``handle``.

Depending on the size of compressed data, ``mte_compress()`` stores it in one of
the size classes backed by kmem caches: ``mte-tags-{16,32,64,128}`` for the
4K-page case (``mte-tags-128`` being used for the data that cannot be compressed
into 64 bytes and is stored uncompressed).
A practical common case allows the tags to be compressed into 8 bytes - then
they are stored in the handle itself.

Handle format
-------------

The handle returned by ``mte_compress()`` is an ``unsigned long`` that has its
bit 63 set to 0 (XArray entries must not exceed ``LONG_MAX``)::

   63  62    60  ...   2         0
  +---+--------+-----+------------+
  | 0 | INLINE | ... |  SIZE_LOG  |
  +---+--------+-----+------------+

Bits ``62..60`` is the inline/out-of-line marker: if they all are set to 1, the
data is stored out-of-line in the buffer pointed to by
``(handle | BIT(63)) & ~7UL``. Otherwise, the data is stored inline in the
handle itself.

Bits ``2..0`` denote the size for out-of-line allocations::

  size = 16 << (handle & 0b111)


Tag compression
---------------

The compression algorithm is a variation of RLE (run-length encoding) and works
as follows (we'll be considering 4K pages and 128-byte tag buffers, but the same
approach scales to 16- and 64K pages):

1. The input array of 128 (``MTE_PAGE_TAG_STORAGE``) bytes is transformed into
   tag ranges (two arrays: ``r_tags[]`` containing tag values and ``r_sizes[]``
   containing range lengths) by ``mte_tags_to_ranges()``. Note that
   ``r_sizes[]`` sums up to 256 (``MTE_GRANULES_PER_PAGE``).

2. The number of the largest element of ``r_sizes[]`` is stored in
   ``largest_idx``. The element itself is thrown away from ``r_sizes[]``,
   because it can be reconstructed from the sum of the remaining elements. Note
   that now none of the remaining ``r_sizes[]`` elements exceeds
   ``MTE_PAGE_TAG_STORAGE - 1``.

3. Depending on the number ``N`` of ranges, a storage class is picked::

           N <= 6:  8 bytes (inline case, no allocation required);
      6 < N <= 11: 16 bytes
     11 < N <= 23: 32 bytes
     23 < N <= 46: 64 bytes
     46 < N:       128 bytes (no compression will be performed)

(See `Why these numbers?`_ below).

4. For the inline case, the following values are stored packed in the 8-byte
   handle (``i<size>`` means a ``<size>``-bit unsigned integer)::

      largest_idx : i4
     r_tags[0..5] : i4 x 6
    r_sizes[0..4] : i7 x 5

   (if N is less than 6, ``r_tags`` and ``r_sizes`` are padded up with zero
   values)

   Because ``largest_idx`` is <= 5, bit 63 of the handle is always 0 (so the
   handle can be stored in an Xarray), and bits 62..60 cannot all be 1 (so the
   handle can be distinguished from a kernel pointer).

5. For the out-of-line case, the storage is allocated from one of the
   ``mte-tags-{16,32,64,128}`` kmem caches. The resulting pointer is aligned
   on 8 bytes, so its bits 2..0 can be used to store the size class (see above).

   Bit 63 of the pointer is zeroed out, so that it can be stored in XArray.

6. The data layout in the allocated storage is as follows::

        largest_idx : i6
       r_tags[0..N] : i4 x N
    r_sizes[0..N-1] : i7 x (N-1)

Tag decompression
-----------------

The decompression algorithm performs the steps below.

1. Decide if data is stored inline (bits ``62..60`` of the handle ``!= 0b111``)
   or out-of line.

2. For the inline case, treat the handle itself as the input buffer.

3. For the out-of-line case, look at bits ``2..0`` of the handle to understand
   the input buffer length. To obtain the pointer to the input buffer, unset
   bits ``2..0`` of the handle and set bit ``63``.

4. If the input buffer is 128 byte long, copy its contents to the output
   buffer.

5. Otherwise, read ``largest_idx``, ``r_tags[]`` and ``r_sizes[]`` from the
   input buffer. Calculate the removed largest element of ``r_sizes[]`` as
   ``largest = 256 - sum(r_sizes)`` and insert it into ``r_sizes`` at
   position ``largest_idx``.

6. For each ``r_sizes[i] > 0``, add a 4-bit value ``r_tags[i]`` to the output
   buffer ``r_sizes[i]`` times.


Why these numbers?
------------------

To be able to reconstruct N tag ranges from the compressed data, we need to
store ``largest_idx``, ``r_tags[N]``, and ``r_sizes[N-1]``. Knowing that the
sizes do not exceed ``MTE_PAGE_TAG_STORAGE``, those can be packed into
``S = ilog2(MTE_PAGE_TAG_STORAGE)`` bits, whereas a single tag occupies
4 bits, and ``largest_idx`` cannot take more than
``Lmax = ilog2(MTE_GRANULES_PER_PAGE)`` bits.

Now, for each ``B``-byte size class it is possible to find the maximal number
``M`` such as ``Lmax + 4 * M + S * (M - 1) <= 8 * B``,
i.e. ``M = (8 * B - 1) / 11``::

 4K pages: S = 7
 +-------------+----+--------------+
 | Buffer size |  M | Storage bits |
 +-------------+----+--------------+
 |          8  |  5 |          56  |
 |         16  | 11 |         122  |
 |         32  | 23 |         254  |
 |         64  | 46 |         507  |
 +-------------+----+--------------+

We can notice that ``M`` (and therefore ``largest_idx``) actually always fits
into 6 bits. For the inline case it is even guaranteed to fit into 3 bits, which
lets us squeeze an extra range into a 8-byte buffer. Because the inline case
requires bit 63 of the handle to be zero, we add that bit to ``largest_idx``,
knowing it will not be used.

For the revised ``largest_idx`` sizes, we now pick the maximal number ``N``
such as ``(L + 4 * N + 7 * (N - 1) <= 8 * S``, where ``L = 4`` in the inline
case and ``L = 6`` otherwise.
In other words, ``N = (8 * S + 7 - L) / 11``, therefore::

  4K pages: S = 7, L_i = 4, L_o = 6
  +-------------+----+--------------+
  | Buffer size |  N | Storage bits |
  +-------------+----+--------------+
  |          8  |  6 |          63  |
  |         16  | 11 |         120  |
  |         32  | 23 |         252  |
  |         64  | 46 |         505  |
  +-------------+----+--------------+

Similarly, for other page sizes::

  16K pages: S = 9, L_i = 4, L_o = 8
  +-------------+-----+--------------+
  | Buffer size |  N  | Storage bits |
  +-------------+-----+--------------+
  |          8  |   5 |          60  |
  |         16  |   9 |         116  |
  |         32  |  19 |         246  |
  |         64  |  39 |         506  |
  |        128  |  78 |        1013  |
  |        256  | 157 |        2040  |
  +-------------+-----+--------------+

  64K pages: S = 11, L_i = 4, L_o = 10
  +-------------+-----+--------------+
  | Buffer size |  N  | Storage bits |
  +-------------+-----+--------------+
  |          8  |   4 |          53  |
  |         16  |   8 |         119  |
  |         32  |  17 |         254  |
  |         64  |  34 |         509  |
  |        128  |  68 |        1019  |
  |        256  | 136 |        2039  |
  |        512  | 273 |        4094  |
  |       1024  | 546 |        8189  |
  +-------------+-----+--------------+


Note
----

Tag compression and decompression implicitly rely on the fixed MTE tag size
(4 bits) and number of tags per page. Should these values change, the algorithm
may need to be revised.


Programming Interface
=====================

 .. kernel-doc:: arch/arm64/mm/mtecomp.c
   :export:
