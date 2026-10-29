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

NFSD PER-EXPORT IO MODES
========================

In addition to the global debugfs knobs above, the IO mode may be set
independently for each export via the ``read_io=`` and ``write_io=``
export options, e.g.::

  /srv/data  client(ro,read_io=direct,write_io=dontcache)

Each option accepts one of ``buffered``, ``dontcache``, or ``direct``,
with the same meanings as the corresponding debugfs values.

An export without an explicit ``read_io=`` (or ``write_io=``) setting
follows the global ``io_cache_read`` (or ``io_cache_write``) knob, so
the effective default for both reads and writes remains buffered IO.
An explicit per-export setting pins that export's IO mode regardless
of the global knob.

NFSD uses splice only for READs whose effective IO mode is
``buffered``. The ``disable-splice-read`` debugfs knob turns splice off
for all exports, but changes neither the global nor any per-export IO
mode.

The per-export settings are reported in
/proc/net/rpc/nfsd.export/content, e.g.::

  /srv/data  client(...,read_io=direct,write_io=dontcache)

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
    If NFSD_IO_DIRECT is used, expand a misaligned READ to the next
    DIO-aligned block (on either end of the READ), except as noted
    below. The expanded READ is verified to have proper offset/len
    (logical_block_size) and dma_alignment checking.

    A READ smaller than the file system's DIO read offset alignment
    (statx dio_read_offset_align, or dio_offset_align on a file system
    that does not report STATX_DIO_READ_ALIGN) is not expanded: it
    would read one or two whole alignment units to return less than
    one unit of data. It is issued as DONTCACHE buffered IO instead
    (normal buffered IO if the filesystem lacks FOP_DONTCACHE).

Misaligned WRITE:
    If NFSD_IO_DIRECT is used, NFSD splits a misaligned WRITE into a
    start, middle and end as needed (see below for when it does not).
    The large middle segment is DIO-aligned and the start and/or end
    are misaligned. Buffered IO is used for the misaligned segments and
    O_DIRECT is used for the middle DIO-aligned segment. A misaligned
    start or end segment runs to the nearest boundary of the smallest
    folio the mapping allows, not just to the nearest DIO-aligned
    offset, so the O_DIRECT segment does not share a folio of that size
    with a buffered one. A larger folio already in the page cache can
    still span the cut. A start or end that is already DIO-aligned gets
    no buffered segment at all. If the filesystem supports
    FOP_DONTCACHE, the O_DIRECT segment is marked DONTCACHE, and so are
    the misaligned segments unless direct_misaligned_dontcache is N
    (below). The flag has no effect on the O_DIRECT segment unless the
    filesystem services it with buffered IO instead, as XFS does when it
    cannot invalidate page cache that overlaps the segment (see
    iomap_dio_invalidate_fail under Tracing). A split FILE_SYNC or
    DATA_SYNC WRITE is persisted once, after all of its segments are
    written, not once per segment.

    If NFSD does not split a WRITE, it issues the whole WRITE
    as a single DONTCACHE buffered IO (normal buffered IO if the
    filesystem lacks FOP_DONTCACHE or direct_misaligned_dontcache is N).
    NFSD does not split a WRITE when:

    - the filesystem advertises no DIO alignment;
    - the WRITE is smaller than the larger of the offset and memory
      alignments;
    - the WRITE has a misaligned start or end and its DIO-aligned middle
      is empty or smaller than
      /sys/kernel/debug/nfsd/direct_misaligned_min_bytes (default 8192);
    - the DIO-aligned middle does not start at a memory address aligned
      to the block device's dma_alignment, so the middle cannot be
      issued as O_DIRECT.

    /sys/kernel/debug/nfsd/direct_misaligned_dontcache (default Y)
    controls the DONTCACHE marking of the buffered segments described
    above. Set it to N for a workload that reads back or rewrites what
    it just wrote.

Tracing:
    The nfsd_read_direct trace event marks a READ that NFSD issues as
    O_DIRECT. It records the offset and length the client requested.
    NFSD expands a misaligned READ to the next DIO-aligned block (on
    either end of the original READ, as needed); the filesystem's own
    event, xfs_file_direct_read on XFS, shows the expanded range that
    is actually issued. A READ that is serviced with buffered IO
    instead emits nfsd_read_dontcache (DONTCACHE buffered IO) or
    nfsd_read_vector (normal buffered IO), in any IO mode, or
    nfsd_read_splice when NFSD uses splice, which only NFSD_IO_BUFFERED
    mode permits.

    In NFSD_IO_DIRECT mode, a READ that is not issued as O_DIRECT also
    emits an event, where NFSD makes that decision, naming the reason::

      nfsd_read_dio_no_alignment  the filesystem advertises no DIO read
                                  alignment
      nfsd_read_dio_too_small     smaller than the DIO read offset
                                  alignment
      nfsd_read_dio_pages_in_use  an earlier operation in the same
                                  COMPOUND already put data in the
                                  reply's pages
      nfsd_read_dio_not_last_op   an NFSv4 READ that is not the last
                                  operation of its COMPOUND

    This combination of trace events is useful for READs::

      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_vector/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_dontcache/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_direct/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_read_io_done/enable
      echo 1 > /sys/kernel/tracing/events/xfs/xfs_file_direct_read/enable
      for e in /sys/kernel/tracing/events/nfsd/nfsd_read_dio_*; do
              echo 1 > $e/enable
      done

    In NFSD_IO_DIRECT mode, each WRITE emits one event, where NFSD
    decides whether to split it, naming the outcome. Each records the
    alignments the filesystem advertised, the sizes of the start,
    middle and end segments (zero where not yet computed), and the
    offset within its page at which the middle segment starts, which is
    what the memory alignment test checks (for a decision made before
    the middle is placed, the offset at which the payload starts)::

      nfsd_write_dio_direct          split: the aligned middle segment
                                     uses O_DIRECT
      nfsd_write_dio_no_alignment    the filesystem advertises no DIO
                                     alignment; not split
      nfsd_write_dio_too_small       smaller than the larger of the
                                     offset and memory alignments; not
                                     split
      nfsd_write_dio_no_middle       no aligned middle, or a misaligned
                                     start or end with a middle smaller
                                     than direct_misaligned_min_bytes
                                     bytes; not split
      nfsd_write_dio_mem_misaligned  the middle does not start at a
                                     memory address aligned to the
                                     block device's dma_alignment; not
                                     split

    Enabling all of them but nfsd_write_dio_direct shows only the WRITEs
    that are not split, and why.

    Each segment of a WRITE in NFSD_IO_DIRECT mode, and each WRITE in
    the other modes, emits nfsd_write_direct (O_DIRECT),
    nfsd_write_dontcache (DONTCACHE buffered IO) or nfsd_write_vector
    (normal buffered IO).

    This combination of trace events is useful for WRITEs::

      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_opened/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_direct/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_dontcache/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_vector/enable
      echo 1 > /sys/kernel/tracing/events/nfsd/nfsd_write_io_done/enable
      echo 1 > /sys/kernel/tracing/events/xfs/xfs_file_direct_write/enable
      echo 1 > /sys/kernel/tracing/events/iomap/iomap_dio_invalidate_fail/enable
      for e in /sys/kernel/tracing/events/nfsd/nfsd_write_dio_*; do
              echo 1 > $e/enable
      done

    iomap_dio_invalidate_fail reports one reason a filesystem services
    an O_DIRECT segment with buffered IO instead: page cache over the
    segment that it could not invalidate. Only a filesystem that uses
    iomap for direct IO emits it, and other fallbacks, such as ext4's
    own, are not traced.

CHOOSING AN NFSD IO MODE
========================

The correct IO mode for an export depends on the workload of its
clients. The most important question is this: does NFSD read the same
data again before the page cache evicts it?

NFS clients keep their own page cache. When a client reads the same
data again, its own cache usually supplies the data. The READ request
does not get to the server. Thus, the server page cache helps mostly
in these conditions:

- Different clients read the same data.
- A client reads data that a different client wrote recently.
- A client reads data again after its own page cache evicted the data.

A low server page cache hit rate is thus normal. It does not by itself
show that buffered IO is the wrong choice.

Use buffered IO for an export when:

- The working set of the export fits in server memory.
- Many clients read the same files.
- Clients send small or misaligned WRITEs. The page cache collects
  these WRITEs and writes them to storage in larger units.

Use dontcache IO for an export when:

- Clients read or write most data only one time.
- The working set of the export is much larger than server memory.
- The data of the export causes the page cache to evict data of other
  exports.

If the underlying filesystem does not support dontcache, NFSD uses
buffered IO instead. See NFSD DONTCACHE above.

Use direct IO for an export when the conditions for dontcache are true,
and also:

- Clients send large, aligned READs and WRITEs.
- Page reclaim uses a large quantity of CPU time, or makes IO stop.

Change the IO mode of one export at a time. Compare the metrics below
before and after each change.

Metrics to monitor
------------------

The metrics that follow can help you choose an IO mode. Measure them
while the export uses buffered IO.

Miss ratio for each export
~~~~~~~~~~~~~~~~~~~~~~~~~~

``/proc/fs/nfsd/export_stats`` shows the number of bytes that NFSD read
(``io_read``) and wrote (``io_write``) for each export and client. If the
kernel has ``CONFIG_TASK_IO_ACCOUNTING``, it also shows the number of
bytes that storage supplied for these READs (``io_read_storage``). This
value includes the readahead that the READs started, and the filesystem
metadata that they needed.

The kernel adds an export to ``export_stats`` when a client first uses
it. The counters of an export continue when mountd refreshes the export
or when ``exportfs -r`` flushes the export cache. They restart if the
export stays out of the export cache for an hour. The start time shows
when the counters started.

Measure ``io_read`` and ``io_read_storage`` over the same interval. Then
divide the increase of ``io_read_storage`` by the increase of
``io_read``. This is the miss ratio of the export.

- A ratio near 1 shows that storage supplies almost all data that NFSD
  reads. The page cache gives little benefit. Dontcache or direct IO
  can be a better choice.
- A ratio much smaller than 1 shows that the page cache supplies a
  large part of the data. Buffered IO is probably the correct choice.
- A ratio larger than 1 can show that readahead reads data that clients
  do not request, for example with small random READs. Metadata reads
  also increase ``io_read_storage`` without increasing ``io_read``.

A READ of a hole increases ``io_read`` but not ``io_read_storage``,
because the filesystem supplies zeros without a storage read. Thus, the
ratio is not an exact page cache miss ratio.

Without ``io_read_storage``, compare ``io_read`` with the number of bytes
that the block device read. Field 3 of ``/sys/block/<dev>/stat`` shows
the device reads in 512-byte sectors. You can also use iostat(1). This
ratio is for all exports on the device. Local applications also cause
device reads. Thus, this ratio is only an approximate value.

Reads of recently evicted data
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When page reclaim evicts a file page, the kernel records the eviction.
If a task reads that page again soon, the kernel counts a refault.
``/proc/vmstat`` shows these counters:

- ``workingset_refault_file``: the number of file pages that the kernel
  read again while it still had a record of their eviction. Some of
  these reads come too late to count as part of the working set.
- ``workingset_activate_file``: the number of refaulted file pages that
  the kernel identified as part of the working set.
- ``pgsteal_kswapd`` and ``pgsteal_direct``: the number of pages that
  page reclaim freed.

Use these counters as follows:

- The ``pgsteal`` counters increase quickly, but
  ``workingset_refault_file`` stays near zero. This shows that clients
  read most data only one time. To find the export that causes this,
  use the miss ratio above or cachestat(2).
- ``workingset_refault_file`` increases quickly. This shows that clients
  read the data again, but the page cache is too small to keep it.
  Set dontcache or direct IO on other exports that read data only one
  time. This gives more page cache to this workload.

These counters are for the full system, not for each export. Dontcache
IO drops pages without a record of the eviction. Direct IO does not use
the page cache. Thus, these counters do not include READs that run as
dontcache or direct IO. A READ that NFSD cannot run as direct IO runs
as buffered IO and is included. A dontcache READ of a page that the
kernel evicted earlier also counts a refault.

Page cache state of a file
~~~~~~~~~~~~~~~~~~~~~~~~~~

The cachestat(2) system call shows the page cache state of a range in a
file. It is available in Linux 6.5 and later. Its output includes:

- ``nr_cache``: the number of pages in the page cache.
- ``nr_dirty`` and ``nr_writeback``: the number of pages that wait for
  writeback, or that are in writeback.
- ``nr_evicted``: the number of pages that the kernel evicted.
- ``nr_recently_evicted``: the number of evicted pages that the kernel
  would still count as part of the working set if a task read them now.

Use cachestat(2) on the frequently used files of an export. This gives
data for each export. Monitor ``nr_recently_evicted`` on the files of
one export while a different export is busy. If the value increases
only while the busy export runs, the busy export probably causes the
page cache to evict useful data. Local memory pressure also evicts
pages, so confirm with the refault counters above before you set
dontcache or direct IO on the busy export.

fincore(1) shows how much of a file is in the page cache.

Cost of page reclaim
~~~~~~~~~~~~~~~~~~~~

Direct IO helps most when page reclaim uses much CPU time or makes IO
stop. Monitor these metrics:

- ``/proc/pressure/memory``: the "some" and "full" lines show the
  percentage of time that tasks waited for memory.
- ``allocstall_*`` and ``pgscan_direct`` in ``/proc/vmstat``: direct
  reclaim activity.
- ``compact_stall`` in ``/proc/vmstat``: memory compaction stalls.
- The CPU time of the kswapd and kcompactd kernel threads.
- ``Dirty`` and ``Writeback`` in ``/proc/meminfo``, for WRITE workloads.

High values can occur while NFSD moves large quantities of data that
clients use only one time. In this condition, direct IO can decrease
the CPU load and make IO performance more constant.

Client page cache
~~~~~~~~~~~~~~~~~

On a Linux NFS client, the ``bytes:`` line in ``/proc/self/mountstats``
shows the bytes that applications read (field 1). It also shows the
bytes that the client read from the server (field 5). mountstats(8) and
nfsiostat(8) also show these values.

Field 5 can be much smaller than field 1. This shows that the client
page cache supplies most reads. In this condition, the server page
cache keeps copies of data that the clients already have. If other
clients do not read the same data, dontcache IO on the server can be a
better choice.
