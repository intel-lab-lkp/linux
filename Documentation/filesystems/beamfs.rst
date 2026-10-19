.. SPDX-License-Identifier: GPL-2.0-only

======
beamfs
======

beamfs is a block-device filesystem that corrects corrupted bytes in
place, on a single device, with no second copy to repair from. File
data, directory blocks, inodes, the allocation bitmap and the
superblock carry Reed-Solomon parity, built on lib/reed_solomon, and
every read decodes. A read the decoder cannot vouch for fails with
``-EIO`` instead of returning wrong data.

It is meant for systems that keep their data on one storage device in
a radiation environment: on-board computers in space, and equipment in
radiotherapy rooms, where high-energy photon beams and proton or ion
beams produce secondary neutrons that cause single-event upsets in
electronics [#tg203]_.

beamfs protects the path between the medium and the page cache. It
does not protect memory after a read, and it does not replace ECC
memory or redundancy across devices; a beamfs volume can sit on a
mirror.


Data blocks
===========

The block size is 4096 bytes. A data block holds sixteen RS(255,239)
codewords over GF(2^8), primitive polynomial 0x187: 3824 coded bytes
and 256 bytes of parity, then a 16-byte tail. Each codeword corrects
up to eight corrupted bytes. In the default layout 288 bytes of every
4096 are overhead, 256 of parity, 16 of descriptor and 16 of
generation, which leaves 3808 bytes of file data per block.

By default the symbols of the sixteen codewords are interleaved across
the block, so a burst in the coded bytes is spread over all of them.
The parity of each codeword stays together, so nine corrupted bytes in
the parity of one codeword are enough to defeat it.

A read decodes every codeword of the block into a private copy. The
corrected bytes reach the page cache; the read does not write them
back to the medium. A codeword the decoder reports as uncorrectable
fails the read with ``-EIO``.

Beyond eight corrupted bytes in a codeword, a Reed-Solomon decoder can
also land on a different valid codeword and report success. With the
``DATA_CSUM`` feature a CRC32 of the payload, stored in the block, is
checked after decoding, and such a read fails closed. ``DATA_CSUM``
and ``DATA_SELFID`` are enabled together by ``mkfs.beamfs
--data-csum``; neither is on by default.


Metadata
========

The superblock carries a CRC32 and Reed-Solomon parity over the region
the CRC covers. It also holds the correction journal: a ring of 64
events, each naming the block, the codeword and the number of symbols
corrected, with a monotonic timestamp. The ``raf-decode`` tool in the
beamfs repository decodes it.

Each inode is one Reed-Solomon codeword, with a CRC32 that covers its
block pointers. Directory blocks carry sixteen codewords of inline
parity, as data blocks do, and hold variable-length entries. The block
allocation bitmap is Reed-Solomon protected.

An indirect block is 512 little-endian 64-bit pointers and has no room
for parity. Its parity lives in a separate region, at a slot computed
from the block number; ``mkfs.beamfs -I`` chooses ``rs`` (the
default), ``crc`` or ``none``. In ``rs`` mode the parity detects and
journals a damaged indirect block but does not repair the buffer,
pointers 478 to 511 have no parity, and a pointer flipped onto another
allocated block is detected only with ``DATA_SELFID``.


Background scrubbing
====================

Correction happens on read, so a block nobody reads accumulates upsets
until it is past the correction radius. Each mounted volume runs a
kernel thread, ``beamfs-scrub/<dev>``, that walks the allocated blocks
at a bounded pace and decodes each one, so that drift is found while
it is still correctable.

When the volume has an error budget, each data block also records the
largest number of symbols corrected in any of its codewords: how much
of its correction capacity it has used.


Using beamfs
============

The userspace tools are in https://github.com/roastercode/beamfs,
under ``tools/``::

    mkfs.beamfs /dev/sdX
    mount -t beamfs /dev/sdX /mnt
    fsck.beamfs /dev/sdX

beamfs takes no mount options.

Module parameters
-----------------

``scrub_interval_ms``
  Milliseconds between two blocks for volumes mounted from then on
  (default 100). 0 parks the scrubber; correction on read is
  unaffected.

``clock_source``
  Declared quality of the wall clock that anchors journal timestamps:
  0 unknown (the default), 1 RTC, 2 NTP, 3 hardware (PTP or GPS).

sysfs
-----

Each mounted volume has a directory ``/sys/fs/beamfs/<dev>/``:

``interval`` (rw)
  Milliseconds between two blocks for this volume's scrubber.

``cursor``, ``passes``, ``blocks`` (ro)
  Position of the scrubber, completed sweeps, blocks decoded.

``corrected``, ``uncorrectable`` (ro)
  Blocks the scrubber corrected, and blocks it could not correct.

``error_budget`` (ro)
  For each number of corrected symbols, how many blocks reached it;
  ``disabled`` if the volume has no error budget.

``clock_anchor`` (ro)
  Monotonic and wall-clock time at the same instant, and the declared
  quality of the wall clock.

``scrub_pace`` (ro)
  The scrubber's current pace.

``alert_rate_limit`` (rw)
  Blocks corrected in one sweep at or above which an alert is sent; 0,
  the default, disables the check.

Alerts
------

When a threshold is crossed, beamfs sends a ``KOBJ_CHANGE`` uevent on
``/sys/fs/beamfs/<dev>`` with ``BEAMFS_ALERT=1``, ``BEAMFS_REASON``
and ``BEAMFS_DETAIL``, and logs the same line. ``BEAMFS_REASON`` is
one of:

``uncorrectable``
  A block could not be recovered.

``no-margin``
  A block has used all its correctable symbols. It still reads
  correctly.

``correction-rate``
  A sweep corrected at least ``alert_rate_limit`` blocks.

Each reason is sent at most once a minute. What to do about an alert
is left to userspace.

debugfs and tracepoints
-----------------------

Reading ``/sys/kernel/debug/beamfs/rs_bench`` encodes and decodes in
memory and reports the cost per block; ``rs_bench_iters`` sets the
number of iterations. The tracepoints are in the ``beamfs`` trace
system.


Limitations
===========

- There is no journal and no copy-on-write. Metadata reaches the
  medium through writeback, in no guaranteed order; after an unclean
  shutdown the volume is checked with ``fsck.beamfs``.
- Metadata goes through buffer heads.
- Without ``DATA_SELFID``, an indirect pointer flipped onto another
  allocated block in range is not detected.
- The measurements published so far emulate single-bit upsets on the
  read path, with at most two erroneous symbols per codeword and per
  decode [#beamfsv3]_. The eight-symbol limit, bursts, miscorrection
  and performance are not measured.


References
==========

.. [#tg203] M. Miften et al., Management of radiotherapy patients with
   implanted cardiac pacemakers and defibrillators: a report of the
   AAPM TG-203, Medical Physics, 2019. doi:10.1002/mp.13838

.. [#beamfsv3] A. Desbrieres, beamfs v3 technical report, Zenodo,
   2026. doi:10.5281/zenodo.23253350
