==========================
HiSilicon SoC Cache Driver
==========================

Introduction
============

HiSilicon SoC cache provides the capabilities of preventing given range of
memory from being evicted from L3 cache. The driver exports the lockdown API to
userspace, allowing allocation of memory that is guranteed to be placed in L3
cache, thus decreasing average memory access latency.

Usage
=====

Kernel built with CONFIG_HISI_SOC_CACHE on will have the device file at
`/dev/hisi_l3c`, cache operations can be performed through it.

mmap():
-------

This interface can be used to allocate memory that is guranteed to not be
evicted out of HiSilicon L3 cache. Newly allocated memory will be prefetched to
L3 cache automatically.

Users should set `PROT_READ` or `PROT_WRITE` to enable read/write to the memory
region. Once mmap call succeeds, read and write can be applied to the memory
region indicated by the returned pointer.

Calling `munmap()` to the pointer can be used to unlock the memory regions.

Restrictions of the cache lockdown are listed below:
  - Only limited number of memory regions are supported, the exact number is
    reported by firmware.
  - Sum of the sizes of locked memory regions should be less than 70% of the
    total size of cache instance.
  - Lock/unlock can only be performed during allocation/deallocation, locking
    existing memory is not supported yet.

ioctl():
--------

This interface provides useful information of HiSilicon L3 cache.

HISI_L3C_LOCK_INFO
  - struct hisi_l3c_lock_info (read)

  Gets detailed information of L3 cache lock restrictions.

This ioctl call returns the detailed information of HiSilicon L3 cache lock
restriction. Information will be presented in the form of::

        struct hisi_l3c_lock_info {
                unsigned int lock_region_num;
                size_t lock_size;
                bool address_alignment;
                size_t max_lock_size;
                size_t min_lock_size;
        };

User may perform a query before issueing cache lock to check for available
resource.
