use crate::{
    bindings,
    error::Result,
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
/// A zpool driver implementation which does nothing but prints pool name on its creation and
/// destruction, and panics if zswap tries to actually read from a pool's alleged object.
///
/// ```
/// use core::ptr::NonNull;
/// use kernel::alloc::{Flags, KBox, NumaNode};
/// use kernel::zpool::*;
///
/// struct MyZpool {
///     name: &'static CStr,
/// }
///
/// struct MyZpoolDriver;
///
/// impl ZpoolDriver for MyZpoolDriver {
///     type Pool = KBox<MyZpool>;
///
///     fn create(name: &'static CStr, gfp: Flags) -> Result<KBox<MyZpool>> {
///         let myPool = MyZpool { name };
///         let mut pool = KBox::new(myPool, gfp)?;
///
///         pr_info!("Created pool {}\n", pool.name);
///         Ok(pool)
///     }
///     fn destroy(p: KBox<MyZpool>) {
///         let pool = KBox::into_inner(p);
///         pr_info!("Removed pool {}\n", pool.name);
///     }
///     fn malloc(_pool: &mut MyZpool, _size: usize, _gfp: Flags, _nid: NumaNode) -> Result<usize> {
///         Ok(0) // TODO
///     }
///     fn free(_pool: &MyZpool, _handle: usize) {
///         // TODO
///     }
///     fn read_begin(_pool: &MyZpool, _handle: usize) -> NonNull<u8> {
///         panic!("read_begin not implemented\n"); // TODO
///     }
///     fn read_end(_pool: &MyZpool, _handle: usize, _handle_mem: NonNull<u8>) {}
///     fn write(_pool: &MyZpool, _handle: usize, _handle_mem: NonNull<u8>, _mem_len: usize) {}
///     fn total_pages(_pool: &MyZpool) -> u64 { 0 }
/// }
/// ```
pub trait ZpoolDriver {
    /// Opaque Rust representation of `struct zpool`.
    type Pool: ForeignOwnable;

    /// Create a pool.
    fn create(name: &'static CStr, gfp: Flags) -> Result<Self::Pool>;

    /// Destroy the pool.
    fn destroy(pool: Self::Pool);

    /// Allocate an object of size `size` using GFP flags `gfp` from the pool `pool`, wuth the
    /// preferred NUMA node `nid`. If the allocation is successful, an opaque handle is returned.
    fn malloc(
        pool: <Self::Pool as ForeignOwnable>::BorrowedMut<'_>,
        size: usize,
        gfp: Flags,
        nid: NumaNode,
    ) -> Result<usize>;

    /// Free a previously allocated from the `pool` object, represented by `handle`.
    fn free(pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>, handle: usize);

    /// Make all the necessary preparations for the caller to be able to read from the object
    /// represented by `handle` and return a valid pointer to the `handle` memory to be read.
    fn read_begin(pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>, handle: usize)
        -> NonNull<u8>;

    /// Finish reading from a previously allocated `handle`. `handle_mem` must be the pointer
    /// previously returned by `read_begin`.
    fn read_end(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: usize,
        handle_mem: NonNull<u8>,
    );

    /// Write to the object represented by a previously allocated `handle`. `handle_mem` points
    /// to the memory to copy data from, and `mem_len` defines the length of the data block to
    /// be copied.
    fn write(
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
        let pool = unsafe { T::create(CStr::from_char_ptr(name), Flags::new(gfp)) };
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
        let real_nid = match nid {
            bindings::NUMA_NO_NODE => Ok(NumaNode::NO_NODE),
            _ => NumaNode::new(nid),
        };
        if real_nid.is_err() {
            return -(bindings::EINVAL as i32);
        }

        let result = T::malloc(pool, size, Flags::new(gfp), real_nid.unwrap());
        match result {
            Err(_) => -(bindings::ENOMEM as i32),
            Ok(h) => {
                // SAFETY: handle is guaranteed to be a valid pointer by zpool
                unsafe { *handle = h };
                0
            }
        }
    }
    extern "C" fn free_(pool: *mut c_void, handle: usize) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };
        T::free(pool, handle)
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
        let non_null_ptr = T::read_begin(pool, handle);
        non_null_ptr.as_ptr().cast()
    }
    extern "C" fn obj_read_end_(pool: *mut c_void, handle: usize, handle_mem: *mut c_void) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };

        // SAFETY: handle_mem is guaranteed to be non-null by zpool
        let handle_mem_ptr = unsafe { NonNull::new_unchecked(handle_mem.cast()) };
        T::read_end(pool, handle, handle_mem_ptr)
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

        // SAFETY: handle_mem is guaranteed to be non-null by zpool
        let handle_mem_ptr = unsafe { NonNull::new_unchecked(handle_mem.cast()) };
        T::write(pool, handle, handle_mem_ptr, mem_len);
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

/// Declares a kernel module that exposes a zpool driver (i. e. an implementation of the zpool API)
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


