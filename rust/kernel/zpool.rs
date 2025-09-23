// SPDX-License-Identifier: GPL-2.0

//! Implementation of Rust interface towards zpool API.

use crate::{error::Result, kernel::alloc::Flags, str::CString, types::ForeignOwnable};
use core::ptr::NonNull;
use kernel::alloc::NumaNode;

/// The Rust representation of zpool handle.
pub struct ZpoolHandle(usize);

impl ZpoolHandle {
    /// Create `ZpoolHandle` from the raw representation.
    pub fn from_raw(h: usize) -> Self {
        Self(h)
    }

    /// Get the raw representation of the handle.
    pub fn as_raw(self) -> usize {
        self.0
    }
}

/// Zpool API.
///
/// The [`ZpoolDriver`] trait serves as an interface for Zpool drivers implemented in Rust.
/// Such drivers implement memory storage pools in accordance with the zpool API.
///
/// # Example
///
/// A zpool driver implementation which uses KVec of 2**n sizes, n = 6, 7, ..., PAGE_SHIFT.
/// Every zpool object is packed into a KVec that is sufficiently large, and n (the
/// denominator) is saved in the least significant bits of the handle, which is guaranteed
/// to be at least 2**6 aligned by kmalloc.
///
/// ```
/// use core::ptr::{NonNull, copy_nonoverlapping};
/// use core::sync::atomic::{AtomicU64, Ordering};
/// use kernel::alloc::{Flags, flags, KBox, KVec, NumaNode};
/// use kernel::page::PAGE_SHIFT;
/// use kernel::prelude::EINVAL;
/// use kernel::str::CString;
/// use kernel::zpool::*;
///
/// struct MyZpool {
///     name: CString,
///     bytes_used: AtomicU64,
/// }
///
/// struct MyZpoolDriver;
///
/// impl ZpoolDriver for MyZpoolDriver {
///     type Pool = KBox<MyZpool>;
///
///     fn create(name: CString, gfp: Flags) -> Result<KBox<MyZpool>> {
///         let my_pool = MyZpool { name, bytes_used: AtomicU64::new(0) };
///         let pool = KBox::new(my_pool, gfp)?;
///
///         pr_debug!("Pool {:?} created\n", pool.name);
///         Ok(pool)
///     }
///
///     fn malloc(pool: &MyZpool, size: usize, _gfp: Flags, _nid: NumaNode) -> Result<ZpoolHandle> {
///         let pow = size.next_power_of_two().trailing_zeros().max(6);
///         match pow {
///             0 => Err(EINVAL),
///             m if m > PAGE_SHIFT as u32 => Err(ENOSPC),
///             _ => {
///                 let vec = KVec::<u8>::with_capacity(1 << pow, GFP_KERNEL)?;
///                 let (ptr, _len, _cap) = vec.into_raw_parts();
///
///                 // We assume that kmalloc-64, kmalloc-128 etc. kmem caches will be used for
///                 // our allocations, so it's actually `1 << pow` bytes that we have consumed.
///                 pool.bytes_used.fetch_add(1 << pow, Ordering::Relaxed);
///
///                 // `kmalloc` guarantees that an allocation of size x*2^n is 2^n aligned.
///                 // Therefore the 6 lower bits are zeros and we can use these to store `pow`.
///                 Ok(ZpoolHandle::from_raw(ptr as usize | (pow as usize - 6)))
///             }
///         }
///     }
///
///     unsafe fn free(pool: &MyZpool, handle: ZpoolHandle) {
///         let h = handle.as_raw();
///         let n = (h & 0x3F) + 6;
///         let uptr = h & !0x3F;
///
///         // SAFETY:
///         // - we derive `uptr` from handle by zeroing 6 lower bits where we store the power
///         //   denominator for the vector capacity. As noted above, the result will be exactly the
///         //   pointer to the area allocated by `KVec`. Thus, uptr is a valid pointer pointing to
///         //   the vector allocated by `alloc` function above.
///         // - 1 << n == capacity and is coming from the first 6 bits of handle.
///         let vec = unsafe { KVec::<u8>::from_raw_parts(uptr as *mut u8, 0, 1 << n) };
///         drop(vec);
///         pool.bytes_used.fetch_sub(1 << n, Ordering::Relaxed);
///     }
///
///     unsafe fn read_begin(_pool: &MyZpool, handle: ZpoolHandle) -> NonNull<u8> {
///         let uptr = handle.as_raw() & !0x3F;
///         // SAFETY:
///         // - we derive `uptr` from handle by zeroing 6 lower bits where we store the power
///         //   denominator for the vector capacity. As noted above, the result will be exactly the
///         //   pointer to the area allocated by `KVec`. Thus, uptr is a valid pointer pointing to
///         //   the vector allocated by `alloc` function above.
///         unsafe { NonNull::new_unchecked(uptr as *mut u8) }
///     }
///
///     unsafe fn read_end(_pool: &MyZpool, _handle: ZpoolHandle, _handle_mem: NonNull<u8>) {}
///
///     unsafe fn write(_p: &MyZpool, handle: ZpoolHandle, h_mem: NonNull<u8>, mem_len: usize) {
///         let uptr = handle.as_raw() & !0x3F;
///         // SAFETY:
///         // - `h_mem` is a valid non-null pointer provided by zswap.
///         // - `uptr` is derived from handle by zeroing 6 lower bits where we store the power
///         //   denominator for the vector capacity. As noted above, the result will be exactly the
///         //   pointer to the area allocated by `KVec`. Thus, uptr is a valid pointer pointing to
///         //   the vector allocated by `alloc` function above.
///         unsafe {
///             copy_nonoverlapping(h_mem.as_ptr().cast(), uptr as *mut c_void, mem_len)
///         };
///     }
///
///     fn total_pages(pool: &MyZpool) -> u64 {
///         pool.bytes_used.load(Ordering::Relaxed) >> PAGE_SHIFT
///     }
/// }
///
/// // Uncomment this for compile time registration (disabled to avoid build error when KUNIT
/// // tests and zsmalloc are enabled):
/// // kernel::DECLARE_ZPOOL!(MyZpoolDriver);
/// ```
///
pub trait ZpoolDriver {
    /// Opaque Rust representation of `struct zpool`.
    type Pool: ForeignOwnable;

    /// Create a pool.
    fn create(name: CString, gfp: Flags) -> Result<Self::Pool>;

    /// Allocate an object of `size` bytes from `pool`, with the allocation flags `gfp` and
    /// preferred NUMA node `nid`. If the allocation is successful, an opaque handle is returned.
    fn malloc(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        size: usize,
        gfp: Flags,
        nid: NumaNode,
    ) -> Result<ZpoolHandle>;

    /// Free an object previously allocated from the `pool`, represented by `handle`.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle previously returned by `malloc`.
    /// - `handle` must not be used any more after the call to `free`.
    unsafe fn free(pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>, handle: ZpoolHandle);

    /// Make all the necessary preparations for the caller to be able to read from the object
    /// represented by `handle` and return a valid pointer to that object's memory to be read.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle previously returned by `malloc`.
    /// - `read_end` with the same `handle` must be called for each `read_begin`.
    unsafe fn read_begin(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: ZpoolHandle,
    ) -> NonNull<u8>;

    /// Finish reading from a previously allocated `handle`. `handle_mem` must be the pointer
    /// previously returned by `read_begin`.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle previously returned by `malloc`.
    /// - `handle_mem` must be the pointer previously returned by `read_begin`.
    unsafe fn read_end(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: ZpoolHandle,
        handle_mem: NonNull<u8>,
    );

    /// Write to the object represented by a previously allocated `handle`. `handle_mem` points
    /// to the memory to copy data from, and `mem_len` defines the length of the data block to
    /// be copied.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle previously returned by `malloc`.
    /// - `handle_mem` must be a valid pointer into the allocated memory aread represented by
    ///   `handle`.
    /// - `handle_mem + mem_len - 1` must not point outside the allocated memory area.
    unsafe fn write(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: ZpoolHandle,
        handle_mem: NonNull<u8>,
        mem_len: usize,
    );

    /// Get the number of pages used by the `pool`.
    fn total_pages(pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>) -> u64;
}

/// Provide a zpool allocator to zswap
#[macro_export]
macro_rules! DECLARE_ZPOOL {
    ($t: ident) => {
        use core::ptr::null_mut;
        use kernel::error::from_result;
        use kernel::types::ForeignOwnable;

        const __LOG_PREFIX: &[u8] = b"zpool_rust\0";

        fn handle_from_result<T, F>(f: F) -> T
        where
            T: From<usize>,
            F: FnOnce() -> Result<T>,
        {
            match f() {
                Ok(v) => v,
                Err(e) => T::from(0),
            }
        }

        /// Create a pool.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_create_pool(name: *const c_uchar) -> *mut c_void {
            match (|| -> Result<<$t as ZpoolDriver>::Pool> {
                // SAFETY: the memory pointed to by name is guaranteed by `zpool` to be a valid
                // string.
                let name_r = unsafe { CStr::from_char_ptr(name).to_cstring() }?;
                $t::create(name_r, flags::GFP_KERNEL)
            })() {
                Err(_) => null_mut(),
                Ok(pool) => <$t as ZpoolDriver>::Pool::into_foreign(pool),
            }
        }

        /// Destroy the pool.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_destroy_pool(pool: *mut c_void) {
            // SAFETY: The pointer originates from an `into_foreign` call.
            drop(unsafe { <$t as ZpoolDriver>::Pool::from_foreign(pool) })
        }

        /// Allocate an object of `size` bytes from `pool`, with the allocation flags `gfp` and
        /// preferred NUMA node `nid`. If the allocation is successful, an opaque handle
        /// is returned.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_malloc(
            pool: *mut c_void,
            size: usize,
            gfp: u32,
            nid: c_int,
        ) -> c_ulong {
            // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
            // `from_foreign`, then that happens in `zpool_destroy_pool` which will not be called
            // during this method.
            let pool = unsafe { <$t as ZpoolDriver>::Pool::borrow(pool) };
            handle_from_result(|| {
                let the_nid = match nid {
                    kernel::bindings::NUMA_NO_NODE => NumaNode::NO_NODE,
                    _ => NumaNode::new(nid)?,
                };
                let h = $t::malloc(
                    pool,
                    size,
                    flags::GFP_NOWAIT | flags::__GFP_HIGHMEM,
                    the_nid,
                )?;
                Ok(h.as_raw())
            })
        }

        /// Free an object previously allocated from the `pool`, represented by `handle`.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_free(pool: *mut c_void, handle: usize) {
            // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
            // `from_foreign`, then that happens in `zpool_destroy_pool` which will not be called
            // during this method.
            let pool = unsafe { <$t as ZpoolDriver>::Pool::borrow(pool) };

            // SAFETY:
            // - the caller (`zswap`) guarantees that `handle` is a valid handle previously
            // allocated by `malloc`.
            // - the caller (`zswap`) guarantees that it will not call any other function with this
            //   `handle` as a parameter after this call.
            unsafe { $t::free(pool, ZpoolHandle::from_raw(handle)) }
        }

        /// Make all the necessary preparations for the caller to be able to read from the object
        /// represented by `handle` and return a valid pointer to that object's memory to be read.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_obj_read_begin(
            pool: *mut c_void,
            handle: usize,
            _local_copy: *mut c_void,
        ) -> *mut c_void {
            // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
            // `from_foreign`, then that happens in `zpool_destroy_pool` which will not be called
            // during this method.
            let pool = unsafe { <$t as ZpoolDriver>::Pool::borrow(pool) };

            // SAFETY: the caller (`zswap`) guarantees that `handle` is a valid handle previously
            // allocated by `malloc`.
            let non_null_ptr = unsafe { $t::read_begin(pool, ZpoolHandle::from_raw(handle)) };
            non_null_ptr.as_ptr().cast()
        }

        /// Finish reading from a previously allocated `handle`. `handle_mem` must be the pointer
        /// previously returned by `read_begin`.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_obj_read_end(
            pool: *mut c_void,
            handle: usize,
            handle_mem: *mut c_void,
        ) {
            // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
            // `from_foreign`, then that happens in `zpool_destroy_pool` which will not be called
            // during this method.
            let pool = unsafe { <$t as ZpoolDriver>::Pool::borrow(pool) };

            // SAFETY: `handle_mem` is guaranteed to be non-null by the caller (`zswap`).
            let handle_mem_ptr = unsafe { NonNull::new_unchecked(handle_mem.cast()) };

            // SAFETY: the caller (`zswap`) guarantees that `handle` is a valid handle previously
            // allocated by `malloc`.
            unsafe { $t::read_end(pool, ZpoolHandle::from_raw(handle), handle_mem_ptr) }
        }

        /// Write to the object represented by a previously allocated `handle`. `handle_mem` points
        /// to the memory to copy data from, and `mem_len` defines the length of the data block to
        /// be copied.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_obj_write(
            pool: *mut c_void,
            handle: usize,
            handle_mem: *mut c_void,
            mem_len: usize,
        ) {
            // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
            // `from_foreign`, then that happens in `zpool_destroy_pool` which will not be called
            // during this method.
            let pool = unsafe { <$t as ZpoolDriver>::Pool::borrow(pool) };

            // SAFETY: `handle_mem` is guaranteed to be non-null by the caller (`zswap`).
            let handle_mem_ptr = unsafe { NonNull::new_unchecked(handle_mem.cast()) };

            // SAFETY: the caller (`zswap`) guarantees that `handle` is a valid handle previously
            // allocated by `malloc`.
            unsafe {
                $t::write(pool, ZpoolHandle::from_raw(handle), handle_mem_ptr, mem_len);
            }
        }

        /// Get the number of pages used by the `pool`.
        #[no_mangle]
        pub unsafe extern "C" fn zpool_get_total_pages(pool: *mut c_void) -> u64 {
            // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
            // `from_foreign`, then that happens in `zpool_destroy_pool` which will not be called
            // during this method.
            let pool = unsafe { <$t as ZpoolDriver>::Pool::borrow(pool) };
            $t::total_pages(pool)
        }
    };
}
