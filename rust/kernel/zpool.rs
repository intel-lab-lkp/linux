use crate::{
    bindings,
    error::{from_result, Result},
    kernel::alloc::Flags,
    str::CStr,
    types::{ForeignOwnable, Opaque},
};
use core::ffi::{c_int, c_uchar, c_void};
use core::ptr::{null_mut, NonNull};
use kernel::alloc::NumaNode;
use kernel::driver;
use kernel::ThisModule;

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
/// use kernel::alloc::{Flags, KBox, KVec, NumaNode};
/// use kernel::page::PAGE_SHIFT;
/// use kernel::prelude::EINVAL;
/// use kernel::zpool::*;
///
/// struct MyZpool {
///     name: &'static CStr,
///     bytes_used: AtomicU64,
/// }
///
/// struct MyZpoolDriver;
///
/// impl ZpoolDriver for MyZpoolDriver {
///     type Pool = KBox<MyZpool>;
///
///     fn create(name: &'static CStr, gfp: Flags) -> Result<KBox<MyZpool>> {
///         let my_pool = MyZpool { name, bytes_used: AtomicU64::new(0) };
///         let pool = KBox::new(my_pool, gfp)?;
///
///         Ok(pool)
///     }
///
///     fn destroy(p: KBox<MyZpool>) {
///         drop(p);
///     }
///
///     fn malloc(pool: &mut MyZpool, size: usize, gfp: Flags, _nid: NumaNode) -> Result<usize> {
///         let mut pow: usize = 0;
///         for n in 6..=PAGE_SHIFT {
///             if size <= 1 << n {
///                 pow = n;
///                 break;
///             }
///         }
///         match pow {
///             0 => Err(EINVAL),
///             _ => {
///                 let vec = KVec::<u64>::with_capacity(1 << (pow - 3), gfp)?;
///                 let (ptr, _len, _cap) = vec.into_raw_parts();
///                 pool.bytes_used.fetch_add(1 << pow, Ordering::Relaxed);
///                 Ok(ptr as usize | (pow - 6))
///             }
///         }
///     }
///
///     unsafe fn free(pool: &MyZpool, handle: usize) {
///         let n = (handle & 0x3F) + 3;
///         let uptr = handle & !0x3F;
///
///         // SAFETY:
///         // - uptr comes from handle which points to the KVec allocation from `alloc`.
///         // - size == capacity and is coming from the first 6 bits of handle.
///         let vec = unsafe { KVec::<u64>::from_raw_parts(uptr as *mut u64, 1 << n, 1 << n) };
///         drop(vec);
///         pool.bytes_used.fetch_sub(1 << (n + 3), Ordering::Relaxed);
///     }
///
///     unsafe fn read_begin(_pool: &MyZpool, handle: usize) -> NonNull<u8> {
///         let uptr = handle & !0x3F;
///         // SAFETY: uptr points to a memory area allocated by KVec
///         unsafe { NonNull::new_unchecked(uptr as *mut u8) }
///     }
///
///     unsafe fn read_end(_pool: &MyZpool, _handle: usize, _handle_mem: NonNull<u8>) {}
///
///     unsafe fn write(_p: &MyZpool, handle: usize, handle_mem: NonNull<u8>, mem_len: usize) {
///         let uptr = handle & !0x3F;
///         // SAFETY: handle_mem is a valid non-null pointer provided by zpool, uptr points to
///         // a KVec allocated in `malloc` and is therefore also valid.
///         unsafe {
///             copy_nonoverlapping(handle_mem.as_ptr().cast(), uptr as *mut c_void, mem_len)
///         };
///     }
///
///     fn total_pages(pool: &MyZpool) -> u64 {
///         pool.bytes_used.load(Ordering::Relaxed) >> PAGE_SHIFT
///     }
/// }
/// ```
///
pub trait ZpoolDriver {
    /// Opaque Rust representation of `struct zpool`.
    type Pool: ForeignOwnable;

    /// Create a pool.
    fn create(name: &'static CStr, gfp: Flags) -> Result<Self::Pool>;

    /// Destroy the pool.
    fn destroy(pool: Self::Pool);

    /// Allocate an object of size `size` bytes from `pool`, with the allocation flags `gfp` and
    /// preferred NUMA node `nid`. If the allocation is successful, an opaque handle is returned.
    fn malloc(
        pool: <Self::Pool as ForeignOwnable>::BorrowedMut<'_>,
        size: usize,
        gfp: Flags,
        nid: NumaNode,
    ) -> Result<usize>;

    /// Free a previously allocated from the `pool` object, represented by `handle`.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle previously returned by `malloc`.
    /// - `handle` must not be used any more after the call to `free`.
    unsafe fn free(pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>, handle: usize);

    /// Make all the necessary preparations for the caller to be able to read from the object
    /// represented by `handle` and return a valid pointer to the `handle` memory to be read.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle previously returned by `malloc`.
    /// - `read_end` with the same `handle` must be called for each `read_begin`.
    unsafe fn read_begin(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: usize,
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
        handle: usize,
        handle_mem: NonNull<u8>,
    );

    /// Write to the object represented by a previously allocated `handle`. `handle_mem` points
    /// to the memory to copy data from, and `mem_len` defines the length of the data block to
    /// be copied.
    ///
    /// # Safety
    ///
    /// - `handle` must be a valid handle previously returned by `malloc`.
    /// - `handle_mem` must be a valid pointer to an allocated memory area.
    /// - `handle_mem` + `mem_len` must not point outside the allocated memory area.
    unsafe fn write(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: usize,
        handle_mem: NonNull<u8>,
        mem_len: usize,
    );

    /// Get the number of pages used by the `pool`.
    fn total_pages(pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>) -> u64;
}

/// An "adapter" for the registration of zpool drivers.
pub struct Adapter<T: ZpoolDriver>(T);

impl<T: ZpoolDriver> Adapter<T> {
    extern "C" fn create_(name: *const c_uchar, gfp: u32) -> *mut c_void {
        // SAFETY: the memory pointed to by name is guaranteed by zpool to be a valid string
        let pool = unsafe { T::create(CStr::from_char_ptr(name), Flags::from_raw(gfp)) };
        match pool {
            Err(_) => null_mut(),
            Ok(p) => T::Pool::into_foreign(p),
        }
    }
    extern "C" fn destroy_(pool: *mut c_void) {
        // SAFETY: The pointer originates from an `into_foreign` call.
        T::destroy(unsafe { T::Pool::from_foreign(pool) })
    }
    extern "C" fn malloc_(
        pool: *mut c_void,
        size: usize,
        gfp: u32,
        handle: *mut usize,
        nid: c_int,
    ) -> c_int {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow_mut(pool) };

        from_result(|| {
            let real_nid = match nid {
                bindings::NUMA_NO_NODE => NumaNode::NO_NODE,
                _ => NumaNode::new(nid)?,
            };
            let h = T::malloc(pool, size, Flags::from_raw(gfp), real_nid)?;
            // SAFETY: handle is guaranteed to be a valid pointer by zpool.
            unsafe { *handle = h };
            Ok(0)
        })
    }
    extern "C" fn free_(pool: *mut c_void, handle: usize) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };

        // SAFETY: the caller (zswap) guarantees that `handle` is a valid handle previously
        // allocated by `malloc`.
        unsafe { T::free(pool, handle) }
    }
    extern "C" fn obj_read_begin_(
        pool: *mut c_void,
        handle: usize,
        _local_copy: *mut c_void,
    ) -> *mut c_void {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };

        // SAFETY: the caller (zswap) guarantees that `handle` is a valid handle previously
        // allocated by `malloc`.
        let non_null_ptr = unsafe { T::read_begin(pool, handle) };
        non_null_ptr.as_ptr().cast()
    }
    extern "C" fn obj_read_end_(pool: *mut c_void, handle: usize, handle_mem: *mut c_void) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };

        // SAFETY: handle_mem is guaranteed to be non-null by the caller (zswap)
        let handle_mem_ptr = unsafe { NonNull::new_unchecked(handle_mem.cast()) };

        // SAFETY: the caller (zswap) guarantees that `handle` is a valid handle previously
        // allocated by `malloc`.
        unsafe { T::read_end(pool, handle, handle_mem_ptr) }
    }
    extern "C" fn obj_write_(
        pool: *mut c_void,
        handle: usize,
        handle_mem: *mut c_void,
        mem_len: usize,
    ) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };

        // SAFETY: handle_mem is guaranteed to be non-null by the caller (zswap)
        let handle_mem_ptr = unsafe { NonNull::new_unchecked(handle_mem.cast()) };

        // SAFETY: the caller (zswap) guarantees that `handle` is a valid handle previously
        // allocated by `malloc`.
        unsafe {
            T::write(pool, handle, handle_mem_ptr, mem_len);
        }
    }
    extern "C" fn total_pages_(pool: *mut c_void) -> u64 {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };
        T::total_pages(pool)
    }
}

// SAFETY: A call to `unregister` for a given instance of `RegType` is guaranteed to be valid
// because preceding call to `register` never fails for zpool.
unsafe impl<T: ZpoolDriver + 'static> driver::RegistrationOps for Adapter<T> {
    type RegType = bindings::zpool_driver;

    unsafe fn register(
        pdrv: &Opaque<Self::RegType>,
        name: &'static CStr,
        _module: &'static ThisModule,
    ) -> Result {
        // SAFETY: It's safe to set the fields of `struct zpool_driver` on initialization.
        unsafe {
            (*(pdrv.get())).type_ = name.as_char_ptr().cast_mut();
            (*(pdrv.get())).create = Some(Self::create_);
            (*(pdrv.get())).destroy = Some(Self::destroy_);
            (*(pdrv.get())).malloc = Some(Self::malloc_);
            (*(pdrv.get())).free = Some(Self::free_);
            (*(pdrv.get())).obj_read_begin = Some(Self::obj_read_begin_);
            (*(pdrv.get())).obj_read_end = Some(Self::obj_read_end_);
            (*(pdrv.get())).obj_write = Some(Self::obj_write_);
            (*(pdrv.get())).total_pages = Some(Self::total_pages_);

            bindings::zpool_register_driver(pdrv.get());
        }
        Ok(())
    }
    unsafe fn unregister(pdrv: &Opaque<Self::RegType>) {
        // SAFETY: `pdrv` is guaranteed to be a valid `RegType`.
        unsafe { bindings::zpool_unregister_driver(pdrv.get()) };
    }
}

/// Declares a kernel module that exposes a zpool driver (i. e. an implementation of the zpool
/// API).
///
/// # Examples
///
///```ignore
/// kernel::module_zpool_driver! {
///     type: MyDriver,
///     name: "Module name",
///     authors: ["Author name"],
///     description: "Description",
///     license: "GPL",
/// }
///```
#[macro_export]
macro_rules! module_zpool_driver {
($($f:tt)*) => {
    $crate::module_driver!(<T>, $crate::zpool::Adapter<T>, { $($f)* });
};
}
