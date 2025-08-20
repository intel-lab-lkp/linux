use crate::{
    bindings,
    error::Result,
    kernel::alloc::Flags,
    str::CStr,
    types::{ForeignOwnable, Opaque},
};
use core::ffi::{c_int, c_uchar, c_void};
use core::ptr::null_mut;
use kernel::alloc::NumaNode;
use kernel::ThisModule;

/// zpool API
pub trait Zpool {
    /// Opaque Rust representation of `struct zpool`.
    type Pool: ForeignOwnable;

    /// Create a pool.
    fn create(name: *const c_uchar, gfp: Flags) -> Result<Self::Pool>;

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
        -> *mut c_void;

    /// Finish reading from a previously allocated `handle`. `handle_mem` must be the pointer
    /// previously returned by `read_begin`.
    fn read_end(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: usize,
        handle_mem: *mut c_void,
    );

    /// Write to the object represented by a previously allocated `handle`. `handle_mem` points
    /// to the memory to copy data from, and `mem_len` defines the length of the data block to
    /// be copied.
    fn write(
        pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>,
        handle: usize,
        handle_mem: *mut c_void,
        mem_len: usize,
    );

    /// Get the number of pages used by the `pool`.
    fn total_pages(pool: <Self::Pool as ForeignOwnable>::Borrowed<'_>) -> u64;
}

/// Zpool driver registration trait.
pub trait Registration {
    /// Register a zpool driver.
    fn register(&self, name: &'static CStr, module: &'static ThisModule) -> Result;

    /// Pool creation callback.
    extern "C" fn _create(name: *const c_uchar, gfp: u32) -> *mut c_void;

    /// Pool destruction callback.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `pool` is a valid pointer to `struct zpool`.
    unsafe extern "C" fn _destroy(pool: *mut c_void);

    /// Callback for object allocation in the pool.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `pool` is a valid pointer to `struct zpool` and that `handle`
    /// is a valid pointer to usize.
    unsafe extern "C" fn _malloc(
        pool: *mut c_void,
        size: usize,
        gfp: u32,
        handle: *mut usize,
        nid: c_int,
    ) -> c_int;

    /// Callback for object release.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `pool` is a valid pointer to `struct zpool`.
    unsafe extern "C" fn _free(pool: *mut c_void, handle: usize);

    /// Callback to prepare the object for reading.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `pool` is a valid pointer to `struct zpool`.
    unsafe extern "C" fn _obj_read_begin(
        pool: *mut c_void,
        handle: usize,
        local_copy: *mut c_void,
    ) -> *mut c_void;

    /// Callback to signal the end of reading from an object.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `pool` is a valid pointer to `struct zpool`.
    unsafe extern "C" fn _obj_read_end(pool: *mut c_void, handle: usize, handle_mem: *mut c_void);

    /// Callback for writing to an object.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `pool` is a valid pointer to `struct zpool`.
    unsafe extern "C" fn _obj_write(
        pool: *mut c_void,
        handle: usize,
        handle_mem: *mut c_void,
        mem_len: usize,
    );

    /// Callback to return the number of pages in the pool.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `pool` is a valid pointer to `struct zpool`.
    unsafe extern "C" fn _total_pages(pool: *mut c_void) -> u64;
}

/// Zpool driver structure.
pub struct ZpoolDriver<T: Zpool> {
    inner: Opaque<bindings::zpool_driver>,

    /// Zpool callback functions that a zpool driver must provide
    pub callbacks: T,
}

impl<T: Zpool> Clone for ZpoolDriver<T> {
    fn clone(&self) -> Self {
        todo!()
    }
}

// SAFETY: zpool driver must ensure that ZpoolDriver's `callbacks` are thread safe
unsafe impl<T: Zpool> Sync for ZpoolDriver<T> {}

impl<T: Zpool> ZpoolDriver<T> {
    /// create an instance of a zpool driver
    pub const fn new(t: T) -> Self {
        Self {
            inner: Opaque::uninit(),
            callbacks: t,
        }
    }
}

impl<T: Zpool> Registration for ZpoolDriver<T> {
    extern "C" fn _create(name: *const c_uchar, gfp: u32) -> *mut c_void {
        let pool = T::create(name, Flags::new(gfp));
        match pool {
            Err(_) => null_mut(),
            Ok(p) => T::Pool::into_foreign(p),
        }
    }
    unsafe extern "C" fn _destroy(pool: *mut c_void) {
        // SAFETY: The pointer originates from an `into_foreign` call.
        T::destroy(unsafe { T::Pool::from_foreign(pool) })
    }
    unsafe extern "C" fn _malloc(
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
    unsafe extern "C" fn _free(pool: *mut c_void, handle: usize) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };
        T::free(pool, handle)
    }
    unsafe extern "C" fn _obj_read_begin(
        pool: *mut c_void,
        handle: usize,
        _local_copy: *mut c_void,
    ) -> *mut c_void {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };
        T::read_begin(pool, handle)
    }
    unsafe extern "C" fn _obj_read_end(pool: *mut c_void, handle: usize, handle_mem: *mut c_void) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };
        T::read_end(pool, handle, handle_mem)
    }
    unsafe extern "C" fn _obj_write(
        pool: *mut c_void,
        handle: usize,
        handle_mem: *mut c_void,
        mem_len: usize,
    ) {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };
        T::write(pool, handle, handle_mem, mem_len);
    }
    unsafe extern "C" fn _total_pages(pool: *mut c_void) -> u64 {
        // SAFETY: The pointer originates from an `into_foreign` call. If `pool` is passed to
        // `from_foreign`, then that happens in `_destroy` which will not be called during this
        // method.
        let pool = unsafe { T::Pool::borrow(pool) };
        T::total_pages(pool)
    }

    fn register(&self, name: &'static CStr, module: &'static ThisModule) -> Result {
        // SAFETY: `ZpoolDriver::new()` ensures that `self.inner` is a valid pointer
        unsafe {
            (*(self.inner.get())).create = Some(Self::_create);
            (*(self.inner.get())).destroy = Some(Self::_destroy);
            (*(self.inner.get())).malloc = Some(Self::_malloc);
            (*(self.inner.get())).free = Some(Self::_free);
            (*(self.inner.get())).obj_read_begin = Some(Self::_obj_read_begin);
            (*(self.inner.get())).obj_read_end = Some(Self::_obj_read_end);
            (*(self.inner.get())).obj_write = Some(Self::_obj_write);
            (*(self.inner.get())).total_pages = Some(Self::_total_pages);

            (*(self.inner.get())).owner = module.0;
            (*(self.inner.get())).type_ = name.as_char_ptr().cast_mut();

            bindings::zpool_register_driver(self.inner.get());
        }
        Ok(())
    }
}
