.. SPDX-License-Identifier: GPL-2.0

==================================================
Tag Compression for Memory Tagging Extension (MTE)
==================================================

This document describes the algorithm used to compress memory tags used by the
ARM Memory Tagging Extension (MTE).

Introduction
============

MTE assigns tags to memory pages: for 4K pages those tags occupy 128 bytes
(256 4-bit tags each corresponding to a 16-byte MTE granule), for 16K pages -
512 bytes, for 64K pages - 2048 bytes. By default, MTE carves out 3.125% (1/16)
of the available physical memory to store the tags.

When MTE pages are saved to swap, their tags need to be stored in the kernel
memory. If the system swap is used heavily, these tags may take a substantial
portion of the physical memory. To reduce memory waste, ``CONFIG_ARM64_MTE_COMP``
allows the kernel to store the tags in compressed form.

Implementation details
======================

The algorithm attempts to compress an array of ``MTE_PAGE_TAG_STORAGE``
tag bytes into a byte sequence that can be stored in an 8-byte pointer. If that
is not possible, the data is stored uncompressed.

Tag manipulation and storage
----------------------------

Tags for swapped pages are stored in an XArray that maps swap entries to 63-bit
values (see ``arch/arm64/mm/mteswap.c``). Bit 0 of these values indicates how
their contents should be treated:

 - 0: value is a pointer to an uncompressed buffer allocated with kmalloc()
   (always the case if ``CONFIG_ARM64_MTE_COMP=n``) with the highest bit set
   to 0;
 - 1: value contains compressed data.

``arch/arm64/include/asm/mtecomp.h`` declares the following functions that
manipulate with tags:

- mte_compress() - compresses the given ``MTE_PAGE_TAG_STORAGE``-byte ``tags``
  buffer into a pointer;
- mte_decompress() - decompresses the tags from a pointer;
- mte_is_compressed() - returns ``true`` iff the pointer passed to it should be
  treated as compressed data.

Tag compression
---------------

The compression algorithm is a variation of RLE (run-length encoding) and works
as follows (we will be considering 4K pages and 128-byte tag buffers, but the
same approach scales to 16K and 64K pages):

1. The input array of 128 (``MTE_PAGE_TAG_STORAGE``) bytes is transformed into
   tag ranges (two arrays: ``r_tags[]`` containing tag values and ``r_sizes[]``
   containing range lengths) by mte_tags_to_ranges(). Note that
   ``r_sizes[]`` sums up to 256 (``MTE_GRANULES_PER_PAGE``).

   If ``r_sizes[]`` consists of a single element
   (``{ MTE_GRANULES_PER_PAGE }``), the corresponding range is split into two
   halves, i.e.::

     r_sizes_new[2] = { MTE_GRANULES_PER_PAGE/2, MTE_GRANULES_PER_PAGE/2 };
     r_tags_new[2] = { r_tags[0], r_tags[0] };

2. The number of the largest element of ``r_sizes[]`` is stored in
   ``largest_idx``. The element itself is thrown away from ``r_sizes[]``,
   because it can be reconstructed from the sum of the remaining elements. Note
   that now none of the remaining ``r_sizes[]`` elements exceeds
   ``MTE_GRANULES_PER_PAGE/2``.

3. If the number ``N`` of ranges does not exceed ``6``, the ranges can be
   compressed into 64 bits. This is done by storing the following values packed
   into the pointer (``i<size>`` means a ``<size>``-bit unsigned integer)
   treated as a bitmap (see ``include/linux/bitmap.h``)::

    bit 0      :      (always 1) : i1
    bits 1-3   :     largest_idx : i3
    bits 4-27  :    r_tags[0..5] : i4 x 6
    bits 28-62 :   r_sizes[0..4] : i7 x 5
    bit 63     :      (always 0) : i1

   If N is less than 6, ``r_tags`` and ``r_sizes`` are padded up with zero
   values. The unused bits in the pointer, including bit 63, are also set to 0,
   so the compressed data can be stored in XArray.

   Range size of ``MTE_GRANULES_PER_PAGE/2`` (at most one) does not fit into
   i7 and will be written as 0. This case is handled separately by the
   decompressing procedure.

Tag decompression
-----------------

The decompression algorithm performs the steps below.

1. Read the lowest bit of the data from the input buffer and check that it is 1,
   otherwise bail out.

2. Read ``largest_idx``, ``r_tags[]`` and ``r_sizes[]`` from the
   input buffer.

   If ``largest_idx`` is zero, and all ``r_sizes[]`` are zero, set
   ``r_sizes[0] = MTE_GRANULES_PER_PAGE/2``.

   Calculate the removed largest element of ``r_sizes[]`` as
   ``largest = 256 - sum(r_sizes)`` and insert it into ``r_sizes`` at
   position ``largest_idx``.

6. For each ``r_sizes[i] > 0``, add a 4-bit value ``r_tags[i]`` to the output
   buffer ``r_sizes[i]`` times.


Why these numbers?
------------------

To be able to reconstruct ``N`` tag ranges from the compressed data, we need to
store the indicator bit together with ``largest_idx``, ``r_tags[N]``, and
``r_sizes[N-1]`` in 63 bits.
Knowing that the sizes do not exceed ``MTE_PAGE_TAG_STORAGE``, each of them can be
packed into ``S = ilog2(MTE_PAGE_TAG_STORAGE)`` bits, whereas a single tag occupies
4 bits.

It is evident that the number of ranges that can be stored in 63 bits is
strictly less than 8, therefore we only need 3 bits to store ``largest_idx``.

The maximum values of ``N`` so that the number ``1 + 3 + N * 4 + (N-1) * S`` of
storage bits does not exceed 63, are shown in the table below::

 +-----------+-----------------+----+---+-------------------+
 | Page size | Tag buffer size |  S | N |    Storage bits   |
 +-----------+-----------------+----+---+-------------------+
 |      4 KB |           128 B |  7 | 6 | 63 = 1+3+6*4+5*7  |
 |     16 KB |           512 B |  9 | 5 | 60 = 1+3+5*4+4*9  |
 |     64 KB |          2048 B | 11 | 4 | 53 = 1+3+4*4+3*11 |
 +-----------+-----------------+----+---+-------------------+

Note
----

Tag compression and decompression implicitly rely on the fixed MTE tag size
(4 bits) and number of tags per page. Should these values change, the algorithm
may need to be revised.


Programming Interface
=====================

 .. kernel-doc:: arch/arm64/include/asm/mtecomp.h
 .. kernel-doc:: arch/arm64/mm/mtecomp.c
   :export:
