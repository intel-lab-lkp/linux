==========================
Cleancache Sysfs Interface
==========================

If CONFIG_CLEANCACHE_SYSFS is enabled, monitoring of cleancache performance
can be done via sysfs in the ``/sys/kernel/mm/cleancache`` directory.
The effectiveness of cleancache can be measured (across all filesystems)
with provided stats.
Global stats are published directly under ``/sys/kernel/mm/cleancache`` and
include:

``stored``
       number of successful cleancache folio stores.

``skipped``
       number of folios skipped during cleancache store operation.

``restored``
       number of successful cleancache folio restore operations.

``missed``
       number of failed cleancache folio restore operations.

``reclaimed``
       number of folios reclaimed from the cleancache due to insufficient
       memory.

``recalled``
       number of times cleancache folio content was discarded as a result
       of the cleancache backend taking the folio back.

``invalidated``
       number of times cleancache folio content was discarded as a result
       of invalidation.

``cached``
       number of folios currently cached in the cleancache.

Per-pool stats are published under ``/sys/kernel/mm/cleancache/<pool name>``
where "pool name" is the name pool was registered under. These stats
include:

``size``
       number of folios donated to this pool.

``cached``
       number of folios currently cached in the pool.

``recalled``
       number of times cleancache folio content was discarded as a result
       of the cleancache backend taking the folio back from the pool.
