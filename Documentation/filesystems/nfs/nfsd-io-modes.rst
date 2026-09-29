.. SPDX-License-Identifier: GPL-2.0

=============
NFSD IO MODES
=============

Overview
========

NFSD has historically always used buffered IO when servicing READ and
WRITE operations. BUFFERED is NFSD's default IO mode, but it is possible
to override that default to use either DONTCACHE or DIRECT IO modes.

Experimental NFSD debugfs interfaces are available to allow the NFSD IO
mode used for READ and WRITE to be configured independently. See both:

- /sys/kernel/debug/nfsd/io_cache_read
- /sys/kernel/debug/nfsd/io_cache_write

The default value for both io_cache_read and io_cache_write reflects
NFSD's default IO mode (which is NFSD_IO_BUFFERED=0).

Based on the configured settings, NFSD's IO will either be:

- cached using page cache (NFSD_IO_BUFFERED=0)
- cached but removed from page cache on completion (NFSD_IO_DONTCACHE=1)
- not cached stable_how=NFS_UNSTABLE (NFSD_IO_DIRECT=2)

To set an NFSD IO mode, write a supported value (0 - 2) to the
corresponding IO operation's debugfs interface, e.g.::

  echo 2 > /sys/kernel/debug/nfsd/io_cache_read
  echo 2 > /sys/kernel/debug/nfsd/io_cache_write

To check which IO mode NFSD is using for READ or WRITE, simply read the
corresponding IO operation's debugfs interface, e.g.::

  cat /sys/kernel/debug/nfsd/io_cache_read
  cat /sys/kernel/debug/nfsd/io_cache_write

If you experiment with NFSD's IO modes on a recent kernel and have
interesting results, please report them to linux-nfs@vger.kernel.org

READ and WRITE IO mode interlock
================================

Although io_cache_read and io_cache_write are separate interfaces, NFSD
keeps them from being configured such that one of READ or WRITE uses
DIRECT IO while the other uses the page cache. Mixing DIRECT and
buffered IO to the same file causes needless page cache invalidation and
writeback (see the DIRECT IO discussion in the Linux open(2) manpage),
so writing one interface may adjust the other:

- Setting io_cache_read to NFSD_IO_DIRECT (2) elevates io_cache_write
  to NFSD_IO_DIRECT (2) if it was BUFFERED or DONTCACHE. A WRITE mode
  that is already DIRECT (2, 3 or 4) is left unchanged.
- Setting io_cache_write to any DIRECT mode (2, 3 or 4) elevates
  io_cache_read to NFSD_IO_DIRECT (2) if it was BUFFERED or DONTCACHE.
- Setting io_cache_read to NFSD_IO_BUFFERED (0) or NFSD_IO_DONTCACHE (1)
  while io_cache_write is a DIRECT mode demotes io_cache_write to that
  same value (0 or 1). A WRITE mode that is already BUFFERED or
  DONTCACHE is left unchanged.
- Setting io_cache_write to NFSD_IO_BUFFERED (0) or NFSD_IO_DONTCACHE
  (1) while io_cache_read is NFSD_IO_DIRECT demotes io_cache_read to
  that same value (0 or 1).

Setting either interface to a value other than NFSD_IO_BUFFERED also
disables NFSD's use of splice for READ, because both DONTCACHE and
DIRECT READ must copy into the RPC reply buffer. This is reflected in
/sys/kernel/debug/nfsd/disable-splice-read reading as 1. Writing 0 to
disable-splice-read re-enables splice, which requires buffered READ, so
it forces io_cache_read back to NFSD_IO_BUFFERED (0) and, if
io_cache_write was a DIRECT mode, demotes it to NFSD_IO_BUFFERED (0) as
well.

Always read both interfaces back after writing either of them to
confirm the resulting configuration.

NFSD DONTCACHE
==============

DONTCACHE offers a hybrid approach to servicing IO that aims to offer
the benefits of using DIRECT IO without any of the strict alignment
requirements that DIRECT IO imposes. To achieve this buffered IO is used
but the IO is flagged to "drop behind" (meaning associated pages are
dropped from the page cache) when IO completes.

DONTCACHE aims to avoid what has proven to be a fairly significant
limition of Linux's memory management subsystem if/when large amounts of
data is infrequently accessed (e.g. read once _or_ written once but not
read until much later). Such use-cases are particularly problematic
because the page cache will eventually become a bottleneck to servicing
new IO requests.

For more context on DONTCACHE, please see these Linux commit headers:

- Overview:  9ad6344568cc3 ("mm/filemap: change filemap_create_folio()
  to take a struct kiocb")
- for READ:  8026e49bff9b1 ("mm/filemap: add read support for
  RWF_DONTCACHE")
- for WRITE: 974c5e6139db3 ("xfs: flag as supporting FOP_DONTCACHE")

NFSD_IO_DONTCACHE will fall back to NFSD_IO_BUFFERED if the underlying
filesystem doesn't indicate support by setting FOP_DONTCACHE.

NFSD DIRECT
===========

DIRECT IO doesn't make use of the page cache, as such it is able to
avoid the Linux memory management's page reclaim scalability problems
without resorting to the hybrid use of page cache that DONTCACHE does.

Some workloads benefit from NFSD avoiding the page cache, particularly
those with a working set that is significantly larger than available
system memory. The pathological worst-case workload that NFSD DIRECT has
proven to help most is: NFS client issuing large sequential IO to a file
that is 2-3 times larger than the NFS server's available system memory.
The reason for such improvement is NFSD DIRECT eliminates a lot of work
that the memory management subsystem would otherwise be required to
perform (e.g. page allocation, dirty writeback, page reclaim). When
using NFSD DIRECT, kswapd and kcompactd are no longer commanding CPU
time trying to find adequate free pages so that forward IO progress can
be made.

The performance win associated with using NFSD DIRECT was previously
discussed on linux-nfs, see:
https://lore.kernel.org/linux-nfs/aEslwqa9iMeZjjlV@kernel.org/

But in summary:

- NFSD DIRECT can significantly reduce memory requirements
- NFSD DIRECT can reduce CPU load by avoiding costly page reclaim work
- NFSD DIRECT can offer more deterministic IO performance

As always, your mileage may vary and so it is important to carefully
consider if/when it is beneficial to make use of NFSD DIRECT. When
assessing comparative performance of your workload please be sure to log
relevant performance metrics during testing (e.g. memory usage, cpu
usage, IO performance). Using perf to collect perf data that may be used
to generate a "flamegraph" for work Linux must perform on behalf of your
test is a really meaningful way to compare the relative health of the
system and how switching NFSD's IO mode changes what is observed.

If NFSD_IO_DIRECT is specified by writing 2 (or 3 and 4 for WRITE) to
NFSD's debugfs interfaces, ideally the IO will be aligned relative to
the underlying block device's logical_block_size. Also the memory buffer
used to store the READ or WRITE payload must be aligned relative to the
underlying block device's dma_alignment.

But NFSD DIRECT does handle misaligned IO in terms of O_DIRECT as best
it can:

Misaligned READ:
    If NFSD_IO_DIRECT is used, expand any misaligned READ to the next
    DIO-aligned block (on either end of the READ). The expanded READ is
    verified to have proper offset/len (logical_block_size) and
    dma_alignment checking.

Misaligned WRITE:
    If NFSD_IO_DIRECT is used, split any misaligned WRITE into a start,
    middle and end as needed. The large middle segment is DIO-aligned
    and the start and/or end are misaligned. Buffered IO is used for the
    misaligned segments and O_DIRECT is used for the middle DIO-aligned
    segment. If the underlying filesystem supports FOP_DONTCACHE, the
    misaligned segments use DONTCACHE buffered IO so that their pages
    are dropped from the page cache once written back.

    The O_DIRECT middle segment also carries the DONTCACHE flag. It has
    no effect while the IO really is O_DIRECT, but a filesystem may
    decide on its own to service the segment with buffered IO instead
    (XFS does so when it cannot invalidate page cache that overlaps the
    segment, which can happen when another WRITE's buffered start or
    end segment dirties the shared boundary page at the same time).
    The flag makes that fallback DONTCACHE buffered IO rather than
    normal buffered IO. Such fallbacks are visible through the
    iomap_dio_invalidate_fail trace event; see Tracing below.

    Whenever no part of a WRITE can use O_DIRECT, the whole WRITE is
    issued as a single DONTCACHE buffered IO (normal buffered IO if the
    filesystem lacks FOP_DONTCACHE). This covers: a filesystem that
    advertises no DIO alignment requirements at all; a WRITE smaller
    than the larger of the offset and memory alignments; a WRITE whose
    DIO-aligned middle segment is smaller than
    /sys/kernel/debug/nfsd/direct_misaligned_num_pages pages (default 2)
    while also having a misaligned start or end; and a WRITE whose
    payload memory is not aligned to the block device's dma_alignment,
    which rules out O_DIRECT for the middle segment as well.

Tracing:
    The nfsd_read_direct trace event shows how NFSD expands any
    misaligned READ to the next DIO-aligned block (on either end of the
    original READ, as needed).

    This combination of trace events is useful for READs::

      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_vector/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_direct/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_io_done/enable
      echo 1 > /sys/kernel/tracing/events/xfs/xfs_file_direct_read/enable

    The nfsd_write_direct trace event shows how NFSD splits a given
    misaligned WRITE into a DIO-aligned middle segment.

    This combination of trace events is useful for WRITEs::

      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_opened/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_direct/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_io_done/enable
      echo 1 > /sys/kernel/tracing/events/xfs/xfs_file_direct_write/enable
