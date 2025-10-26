.. SPDX-License-Identifier: GPL-2.0

==========
Cleancache
==========

Motivation
==========

Cleancache is a feature to utilize unused reserved memory for extending
page cache.

Cleancache can be thought of as a folio-granularity victim cache for clean
file-backed pages that the kernel's pageframe replacement algorithm would
like to keep around, but can't since there isn't enough memory. When the
memory reclaim mechanism "evicts" a folio, it stores the data contained
in the folio into cleancache memory which is not directly accessible or
addressable by the kernel and is of unknown and possibly time-varying
size.

Later, when a filesystem wishes to access a folio in a file on disk, it
first checks cleancache to see if it already contains required data; if
it does, the folio data is copied into the kernel and a disk access is
avoided.

The memory cleancache uses is donated by other system components, which
reserve memory not directly addressable by the kernel. By donating this
memory to cleancache, the memory owner enables its utilization while it
is not used. Memory donation is done using cleancache backend API and any
donated memory can be taken back at any time by its donor with no delay
and with guaranteed success. Since cleancache uses this memory only to
store clean file-backed data, it can be dropped at any time and therefore
the donor's request to take back the memory can always be satisfied.

Implementation Overview
=======================

Cleancache "backend" registers itself with cleancache "frontend" and gets
a unique pool_id, which it can use in all later API calls to identify the
pool of folios it donates.
Once registered, backend can call cleancache_backend_put_folio() or
cleancache_backend_put_folios() to donate memory to cleancache. Note that
cleancache currently supports only 0-order folios and will not accept
larger-order ones. Once the backend needs that memory back, it can get it
by calling cleancache_backend_get_folio(). Only the original backend can
take the folio it donated from the cleancache.

Kernel uses cleancache by first calling cleancache_add_fs() to register
each file system and then using a combination of cleancache_store_folio(),
cleancache_restore_folio(), cleancache_invalidate_{folio|inode} to store,
restore and invalidate folio content.
cleancache_{start|end}_inode_walk() are used to walk over folios inside
an inode and cleancache_restore_from_inode() is used to restore folios
during such walks.

From kernel's point of view folios which are copied into cleancache have
an indefinite lifetime which is completely unknowable by the kernel and so
may or may not still be in cleancache at any later time. Thus, as its name
implies, cleancache is not suitable for dirty folios. Cleancache has
complete discretion over what folios to preserve and what folios to discard
and when.

Cleancache Performance Metrics
==============================

Cleancache performance can be measured and monitored using metrics provided
via sysfs interface under ``/sys/kernel/mm/cleancache`` directory. The
interface is described in Documentation/admin-guide/mm/cleancache_sysfs.rst.
