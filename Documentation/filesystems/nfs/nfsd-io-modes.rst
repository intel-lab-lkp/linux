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

    A READ smaller than dio_read_offset_align is not expanded: it would
    read one or two whole alignment units to return fewer bytes than
    one. It is issued as DONTCACHE buffered IO instead (normal buffered
    IO if the filesystem lacks FOP_DONTCACHE).

Misaligned WRITE:
    If NFSD_IO_DIRECT is used, split any misaligned WRITE into a start,
    middle and end as needed. The large middle segment is DIO-aligned
    and the start and/or end are misaligned. Buffered IO is used for the
    misaligned segments and O_DIRECT is used for the middle DIO-aligned
    segment. If the filesystem supports FOP_DONTCACHE, the O_DIRECT
    segment is marked DONTCACHE, and so are the misaligned segments
    unless direct_misaligned_dontcache is N (below). The flag has no
    effect on the O_DIRECT segment unless the filesystem services it
    with buffered IO instead, as XFS does when it cannot invalidate page
    cache that overlaps the segment. The iomap_dio_invalidate_fail trace
    event reports such a fallback.

    If NFSD does not split a misaligned WRITE, it issues the whole WRITE
    as a single DONTCACHE buffered IO (normal buffered IO if the
    filesystem lacks FOP_DONTCACHE). NFSD does not split a WRITE when:

    - the filesystem advertises no DIO alignment;
    - the WRITE is smaller than the larger of the offset and memory
      alignments;
    - the WRITE has a misaligned start or end and its DIO-aligned middle
      is smaller than /sys/kernel/debug/nfsd/direct_misaligned_num_pages
      pages (default 2);
    - the WRITE payload is not aligned in memory to the block device's
      dma_alignment, so the middle cannot be O_DIRECT either.

    A FILE_SYNC or DATA_SYNC WRITE is persisted once after all of its
    segments are written, not once per segment.

    The page holding a start or end segment is shared with the
    neighbouring WRITE, which may arrive before, after or at the same
    time, from any client. So that the second WRITE to such a page does
    not have to read it back from disk, NFSD puts an empty page there
    immediately before writing a start or end segment, if there is not
    one already, without marking it DONTCACHE. The WRITE that finds the
    page already there marks it DONTCACHE once it has written it, and
    the next writeback drops it. The first and last pages of a WRITE
    that is not split are handled the same way. None of this applies
    when direct_misaligned_dontcache is N.

    Writing N to /sys/kernel/debug/nfsd/direct_misaligned_dontcache
    (default Y) issues the start and end segments, and a WRITE that is
    not split, as normal buffered IO instead of DONTCACHE, which suits a
    workload that reads back or rewrites what it just wrote. The O_DIRECT
    middle segment keeps its DONTCACHE flag either way.

Tracing:
    The nfsd_read_direct trace event shows how NFSD expands any
    misaligned READ to the next DIO-aligned block (on either end of the
    original READ, as needed). A READ that is serviced with buffered IO
    instead emits nfsd_read_dontcache (DONTCACHE buffered IO) or
    nfsd_read_vector (normal buffered IO).

    This combination of trace events is useful for READs::

      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_vector/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_dontcache/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_direct/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_io_done/enable
      echo 1 > /sys/kernel/tracing/events/xfs/xfs_file_direct_read/enable

    The nfsd_write_dio_split trace event is emitted once per WRITE in a
    DIRECT IO mode, before any IO is issued. It records the alignments
    the filesystem advertised, the payload's offset in its first page,
    the sizes of the start, middle and end segments, the number of
    segments issued, and a disposition::

      direct          the aligned middle segment uses O_DIRECT
      mem_misaligned  payload memory is misaligned; not split
      no_alignment    the filesystem advertises no DIO alignment; not split
      too_small       smaller than the larger alignment; not split
      no_middle       no aligned middle, or one smaller than
                      direct_misaligned_num_pages; not split

    dontcache=1 means the buffered segments were DONTCACHE: the start and
    end segments of a direct WRITE, or the whole of one that was not
    split.

    Each segment then emits nfsd_write_direct (O_DIRECT),
    nfsd_write_dontcache (DONTCACHE buffered IO) or nfsd_write_vector
    (normal buffered IO).

    This combination of trace events is useful for WRITEs::

      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_opened/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_dio_split/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_direct/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_dontcache/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_vector/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_io_done/enable
      echo 1 > /sys/kernel/tracing/events/xfs/xfs_file_direct_write/enable
      echo 1 > /sys/kernel/tracing/events/iomap/iomap_dio_invalidate_fail/enable

    iomap_dio_invalidate_fail reports an O_DIRECT middle segment that
    the filesystem serviced with buffered IO instead.
