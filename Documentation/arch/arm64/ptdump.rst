======================
Kernel page table dump
======================

ptdump is a debugfs interface that provides a detailed dump of the kernel page
tables. It offers a comprehensive overview of the kernel virtual memory layout
as well as the attributes associated with the various regions in a
human-readable format. It is useful to dump the kernel page tables to verify
permissions and memory types. Examining the page table entries and permissions
helps identify potential security vulnerabilities such as mappings with overly
permissive access rights or improper memory protections.

Memory hotplug allows dynamic expansion or contraction of available memory
without requiring a system reboot. To maintain the consistency and integrity of
the memory management data structures, arm64 makes use of the
``mem_hotplug_lock`` semaphore in write mode. Additionally, in read mode,
``mem_hotplug_lock`` supports an efficient implementation of
``get_online_mems()`` and ``put_online_mems()``. These protect the offlining of
memory being accessed by the ptdump code.

In order to dump the kernel page tables, enable the following configurations
and mount debugfs::

 CONFIG_GENERIC_PTDUMP=y
 CONFIG_PTDUMP_CORE=y
 CONFIG_PTDUMP_DEBUGFS=y

 mount -t debugfs nodev /sys/kernel/debug
 cat /sys/kernel/debug/kernel_page_tables

On analysing the output of ``cat /sys/kernel/debug/kernel_page_tables`` one can
derive information about the virtual address range of a contiguous group of
page table entries, followed by size of the memory region covered by this
group, the hierarchical structure of the page tables and finally the attributes
associated with each page in the group. Groups are broken up either according
to a change in attributes or by parent descriptor, such as a PMD. Note that the
set of attributes, and therefore formatting, is not equivalent between entry
types. For example, PMD entries have a separate set of attributes from leaf
level PTE entries, because they support both the UXNTable and PXNTable
permission bits.

The page attributes provide information about access permissions, execution
capability, type of mapping such as leaf level PTE or block level PGD, PMD and
PUD, and access status of a page within the kernel memory. Non-PTE block or
page level entries are denoted with either "BLK" or "TBL", respectively.
Assessing these attributes can assist in understanding the memory layout,
access patterns and security characteristics of the kernel pages.

Kernel virtual memory layout example::

 start address        end address         size type  leaf    attributes
 +-----------------------------------------------------------------------------------------------------------------+
 | ---[ Linear Mapping start ]---                                                                                  |
 | ...                                                                                                             |
 | 0xffff0d02c3200000-0xffff0d02c3400000    2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL    |
 | 0xffff0d02c3200000-0xffff0d02c3218000   96K PTE           ro NX SHD AF NG     UXN    MEM/NORMAL-TAGGED          |
 | 0xffff0d02c3218000-0xffff0d02c3250000  224K PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED          |
 | 0xffff0d02c3250000-0xffff0d02c33b3000 1420K PTE           ro NX SHD AF NG     UXN    MEM/NORMAL-TAGGED          |
 | 0xffff0d02c33b3000-0xffff0d02c3400000  308K PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED          |
 | 0xffff0d02c3400000-0xffff0d02c3600000    2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL    |
 | 0xffff0d02c3400000-0xffff0d02c3600000    2M PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED          |
 | ...                                                                                                             |
 | 0xffff0d02c3200000-0xffff0d02c3400000    2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL    |
 | ...                                                                                                             |
 | ---[ Linear Mapping end ]---                                                                                    |
 +-----------------------------------------------------------------------------------------------------------------+
 | ---[ Modules start ]---                                                                                         |
 | ...                                                                                                             |
 | 0xffff800000000000-0xffff800000000080 128B PGD   TBL     RW               x     UXNTbl    MEM/NORMAL            |
 | 0xffff800000000000-0xffff800080000000   2G PUD F BLK     RW               x               MEM/NORMAL            |
 | ...                                                                                                             |
 | ---[ Modules end ]---                                                                                           |
 +-----------------------------------------------------------------------------------------------------------------+
 | ---[ vmalloc() area ]---                                                                                        |
 | ...                                                                                                             |
 | 0xffff800080000000-0xffff8000c0000000   1G PUD   TBL     RW               x     UXNTbl    MEM/NORMAL            |
 | ...                                                                                                             |
 | 0xffff800080200000-0xffff800080400000   2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL     |
 | 0xffff800080200000-0xffff80008022f000 188K PTE           RW NX SHD AF NG     UXN    MEM/NORMAL                  |
 | 0xffff80008022f000-0xffff800080230000   4K PTE F BLK     RW x                       MEM/NORMAL                  |
 | 0xffff800080230000-0xffff800080233000  12K PTE           RW NX SHD AF NG     UXN    MEM/NORMAL                  |
 | 0xffff800080233000-0xffff800080234000   4K PTE F BLK     RW x                       MEM/NORMAL                  |
 | 0xffff800080234000-0xffff800080237000  12K PTE           RW NX SHD AF NG     UXN    MEM/NORMAL                  |
 | ...                                                                                                             |
 | 0xffff800080400000-0xffff800084000000  60M PMD F BLK     RW               x      x     x         MEM/NORMAL     |
 | ...                                                                                                             |
 | ---[ vmalloc() end ]---                                                                                         |
 +-----------------------------------------------------------------------------------------------------------------+
 | ---[ vmemmap start ]---                                                                                         |
 | ...                                                                                                             |
 | 0xfffffe33cb000000-0xfffffe33cc000000  16M PMD   BLK     RW SHD AF NG     NX UXN x     x         MEM/NORMAL     |
 | 0xfffffe33cc000000-0xfffffe3400000000 832M PMD F BLK     RW               x      x     x         MEM/NORMAL     |
 | ...                                                                                                             |
 | ---[ vmemmap end ]---                                                                                           |
 +-----------------------------------------------------------------------------------------------------------------+
 | ---[ PCI I/O start ]---                                                                                         |
 | ...                                                                                                             |
 | 0xffffffffc0800000-0xffffffffc0810000 64K PTE           RW NX SHD AF NG     UXN    DEVICE/nGnRE                 |
 | ...                                                                                                             |
 | ---[ PCI I/O end ]---                                                                                           |
 +-----------------------------------------------------------------------------------------------------------------+
 | ---[ Fixmap start ]---                                                                                          |
 | ...                                                                                                             |
 | 0xffffffffff5f6000-0xffffffffff5f9000 12K PTE           ro x  SHD AF        UXN    MEM/NORMAL                   |
 | 0xffffffffff5f9000-0xffffffffff5fa000  4K PTE           ro NX SHD AF NG     UXN    MEM/NORMAL                   |
 | ...                                                                                                             |
 | ---[ Fixmap end ]---                                                                                            |
 +-----------------------------------------------------------------------------------------------------------------+

``cat /sys/kernel/debug/kernel_page_tables`` output::

 0xffff000000000000-0xffff020000000000           2T PGD
 0xffff020000000000-0xffff020000000080         128B PGD   TBL     RW               NXTbl UXNTbl    MEM/NORMAL
     0xffff020000000000-0xffff023080000000         194G PUD
     0xffff023080000000-0xffff0230c0000000           1G PUD   TBL     RW               NXTbl UXNTbl    MEM/NORMAL
       0xffff023080000000-0xffff023080200000           2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL
         0xffff023080000000-0xffff023080200000           2M PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED
       0xffff023080200000-0xffff023080400000           2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL
         0xffff023080200000-0xffff023080210000          64K PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED
         0xffff023080210000-0xffff023080400000        1984K PTE           ro NX SHD AF NG     UXN    MEM/NORMAL
       0xffff023080400000-0xffff023081c00000          24M PMD   BLK     ro SHD AF NG     NX UXN x     x         MEM/NORMAL
       0xffff023081c00000-0xffff023081e00000           2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL
         0xffff023081c00000-0xffff023081dd0000        1856K PTE           ro NX SHD AF NG     UXN    MEM/NORMAL
         0xffff023081dd0000-0xffff023081e00000         192K PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED
       0xffff023081e00000-0xffff023082000000           2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL
         0xffff023081e00000-0xffff023082000000           2M PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED
       0xffff023082000000-0xffff023082200000           2M PMD   TBL     RW               x      NXTbl UXNTbl    MEM/NORMAL
         0xffff023082000000-0xffff023082200000           2M PTE           RW NX SHD AF NG     UXN    MEM/NORMAL-TAGGED
