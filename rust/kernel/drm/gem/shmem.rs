// SPDX-License-Identifier: GPL-2.0

//! DRM GEM shmem helper objects
//!
//! C header: [`include/linux/drm/drm_gem_shmem_helper.h`](srctree/include/drm/drm_gem_shmem_helper.h)

// TODO:
// - There are a number of spots here that manually acquire/release the DMA reservation lock using
//   dma_resv_(un)lock(). In the future we should add support for ww mutex, expose a method to
//   acquire a reference to the WwMutex, and then use that directly instead of the C functions here.

use crate::{
    container_of,
    device::{
        self,
        Bound, //
    },
    devres::*,
    drm::{
        driver,
        gem,
        private::Sealed,
        Device, //
    },
    error::{
        from_err_ptr,
        to_result, //
    },
    prelude::*,
    scatterlist,
    sync::{
        aref::ARef,
        new_mutex,
        Mutex,
        SetOnce, //
    },
    types::Opaque, //
};
use core::{
    mem,
    ops::{
        Deref,
        DerefMut, //
    },
    ptr::{
        self,
        NonNull, //
    },
};
use gem::{
    BaseObjectPrivate,
    DriverObject,
    IntoGEMObject, //
};

/// A struct for controlling the creation of shmem-backed GEM objects.
///
/// This is used with [`Object::new()`] to control various properties that can only be set when
/// initially creating a shmem-backed GEM object.
#[derive(Default)]
pub struct ObjectConfig<'a, T: DriverObject> {
    /// Whether to set the write-combine map flag.
    pub map_wc: bool,

    /// Reuse the DMA reservation from another GEM object.
    ///
    /// The newly created [`Object`] will hold an owned refcount to `parent_resv_obj` if specified.
    pub parent_resv_obj: Option<&'a Object<T>>,
}

/// A shmem-backed GEM object.
///
/// # Invariants
///
/// `obj` contains a valid initialized `struct drm_gem_shmem_object` for the lifetime of this
/// object.
#[repr(C)]
#[pin_data]
pub struct Object<T: DriverObject> {
    #[pin]
    obj: Opaque<bindings::drm_gem_shmem_object>,
    /// Parent object that owns this object's DMA reservation object.
    parent_resv_obj: Option<ARef<Object<T>>>,
    /// Devres object for unmapping any SGTable on driver-unbind.
    /// TODO: Drop the mutex once we can use Init with SetOnce.
    #[pin]
    sgt_res: Mutex<SetOnce<Devres<SGTableMap<T>>>>,
    #[pin]
    inner: T,
}

super::impl_aref_for_gem_obj!(impl<T> for Object<T> where T: DriverObject);

// SAFETY: All GEM objects are thread-safe.
unsafe impl<T: DriverObject> Send for Object<T> {}

// SAFETY: All GEM objects are thread-safe.
unsafe impl<T: DriverObject> Sync for Object<T> {}

impl<T: DriverObject> Object<T> {
    /// `drm_gem_object_funcs` vtable suitable for GEM shmem objects.
    const VTABLE: bindings::drm_gem_object_funcs = bindings::drm_gem_object_funcs {
        free: Some(Self::free_callback),
        open: Some(super::open_callback::<T>),
        close: Some(super::close_callback::<T>),
        print_info: Some(bindings::drm_gem_shmem_object_print_info),
        export: None,
        pin: Some(bindings::drm_gem_shmem_object_pin),
        unpin: Some(bindings::drm_gem_shmem_object_unpin),
        get_sg_table: Some(bindings::drm_gem_shmem_object_get_sg_table),
        vmap: Some(bindings::drm_gem_shmem_object_vmap),
        vunmap: Some(bindings::drm_gem_shmem_object_vunmap),
        mmap: Some(bindings::drm_gem_shmem_object_mmap),
        status: None,
        rss: None,
        #[allow(unused_unsafe, reason = "Safe since Rust 1.82.0")]
        // SAFETY: `drm_gem_shmem_vm_ops` is a valid, static const on the C side.
        vm_ops: unsafe { &raw const bindings::drm_gem_shmem_vm_ops },
        evict: None,
    };

    /// Return a raw pointer to the embedded drm_gem_shmem_object.
    fn as_raw_shmem(&self) -> *mut bindings::drm_gem_shmem_object {
        self.obj.get()
    }

    /// Create a new shmem-backed DRM object of the given size.
    ///
    /// Additional config options can be specified using `config`.
    pub fn new(
        dev: &Device<T::Driver>,
        size: usize,
        config: ObjectConfig<'_, T>,
        args: T::Args,
    ) -> Result<ARef<Self>> {
        let new: Pin<KBox<Self>> = KBox::try_pin_init(
            try_pin_init!(Self {
                obj <- Opaque::init_zeroed(),
                parent_resv_obj: config.parent_resv_obj.map(|p| p.into()),
                sgt_res <- new_mutex!(SetOnce::new()),
                inner <- T::new(dev, size, args),
            }),
            GFP_KERNEL,
        )?;

        // SAFETY: `obj.as_raw()` is guaranteed to be valid by the initialization above.
        unsafe { (*new.as_raw()).funcs = &Self::VTABLE };

        // SAFETY: The arguments are all valid via the type invariants.
        to_result(unsafe { bindings::drm_gem_shmem_init(dev.as_raw(), new.as_raw_shmem(), size) })?;

        // SAFETY: We never move out of `self`.
        let new = KBox::into_raw(unsafe { Pin::into_inner_unchecked(new) });

        // SAFETY: We're taking over the owned refcount from `drm_gem_shmem_init`.
        let obj = unsafe { ARef::from_raw(NonNull::new_unchecked(new)) };

        // Start filling out values from `config`
        if let Some(parent_resv) = config.parent_resv_obj {
            // SAFETY: We have yet to expose the new gem object outside of this function, so it is
            // safe to modify this field.
            unsafe { (*obj.obj.get()).base.resv = parent_resv.raw_dma_resv() };
        }

        // SAFETY: We have yet to expose this object outside of this function, so we're guaranteed
        // to have exclusive access - thus making this safe to hold a mutable reference to.
        let shmem = unsafe { &mut *obj.as_raw_shmem() };
        shmem.set_map_wc(config.map_wc);

        Ok(obj)
    }

    /// Returns the `Device` that owns this GEM object.
    pub fn dev(&self) -> &Device<T::Driver> {
        // SAFETY: `dev` will have been initialized in `Self::new()` by `drm_gem_shmem_init()`.
        unsafe { Device::from_raw((*self.as_raw()).dev) }
    }

    extern "C" fn free_callback(obj: *mut bindings::drm_gem_object) {
        // SAFETY:
        // - DRM always passes a valid gem object here
        // - We used drm_gem_shmem_create() in our create_gem_object callback, so we know that
        //   `obj` is contained within a drm_gem_shmem_object
        let base = unsafe { container_of!(obj, bindings::drm_gem_shmem_object, base) };

        // SAFETY:
        // - We verified above that `obj` is valid, which makes `this` valid
        // - This function is set in AllocOps, so we know that `this` is contained within a
        //   `Object<T>`
        let this = unsafe { container_of!(Opaque::cast_from(base), Self, obj) }.cast_mut();

        // SAFETY: We are in free_callback(), which means that we have exclusive access to `this`.
        let mut sgt_guard = unsafe { (*this).sgt_res.lock() };

        // drm_gem_shmem_release() will clear any existing sgt, so we need to clear sgt_res before
        // calling it to prevent a double-free.
        drop(mem::take(sgt_guard.deref_mut()));

        // Drop the lock we acquired, we don't need it anymore and it will be acquired again when we
        // perform the final drop in this function.
        drop(sgt_guard);

        // SAFETY:
        // - We're in free_callback - so this function is safe to call.
        // - We won't be using the gem resources on `this` after this call.
        unsafe { bindings::drm_gem_shmem_release(base) };

        // SAFETY: We're recovering the Kbox<> we created in gem_create_object()
        let _ = unsafe { KBox::from_raw(this) };
    }

    // If necessary, create an SGTable for the gem object and register a Devres for it to ensure
    // that it is unmapped on driver unbind.
    fn get_sg_table<'a>(
        &'a self,
        dev: &'a device::Device<Bound>,
    ) -> Result<&'a Devres<SGTableMap<T>>> {
        if dev.as_raw() != self.dev().as_ref().as_raw() {
            return Err(EINVAL);
        }

        let sgt_res = self.sgt_res.lock();
        let sgt_map = if let Some(devres) = sgt_res.as_ref() {
            devres
        } else {
            // INVARIANT: We store this Devres in the object itself and don't move it, ensuring that
            // the object it points to remains valid for the lifetime of the SGTableMap.
            sgt_res.populate(Devres::new(dev, SGTableMap::new(self))?);

            // SAFETY: We just populated `sgt_res` above, making this safe to unwrap.
            unsafe { sgt_res.as_ref().unwrap_unchecked() }
        };

        // SAFETY:
        // We only write to `sgt_res` in two places:
        // - The above code.
        // - `free_callback()`, which can't be called as long as `self` is alive.
        // Therefore, it's safe to hold a reference to the contents of `sgt_res` without holding
        // the lock for the lifetime of 'a, making this lifetime extension safe.
        Ok(unsafe { mem::transmute::<&_, &'a _>(sgt_map) })
    }

    /// Creates (if necessary) and returns an immutable reference to a scatter-gather table of DMA
    /// pages for this object.
    ///
    /// This will pin the object in memory. It is expected that `dev` should be a pointer to the
    /// same [`device::Device`] which `self` belongs to, otherwise this function will return
    /// `Err(EINVAL)`.
    #[inline]
    pub fn sg_table<'a>(
        &'a self,
        dev: &'a device::Device<Bound>,
    ) -> Result<&'a scatterlist::SGTable> {
        let sgt = self.get_sg_table(dev)?;

        Ok(sgt.access(dev)?.deref())
    }

    /// Creates (if necessary) and returns an owned reference to a scatter-gather table of DMA pages
    /// for this object.
    ///
    /// This is the same as [`sg_table`], except that it instead returns an
    /// [`shmem::SGTable`] which holds a reference to the associated gem object, instead of a
    /// reference to an [`scatterlist::SGTable`].
    ///
    /// See [`sg_table`] for more info.
    ///
    /// [`shmem::SGTable`]: SGTable
    /// [`sg_table`]: Self::sg_table
    pub fn owned_sg_table(&self, dev: &device::Device<Bound>) -> Result<SGTable<T>> {
        self.get_sg_table(dev)?;

        // INVARIANT: We just ensured above that `self.sgt_res` is initialized with
        // `Devres<SGTableMap<T>>`.
        Ok(SGTable(self.into()))
    }
}

impl<T: DriverObject> Deref for Object<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

impl<T: DriverObject> DerefMut for Object<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.inner
    }
}

impl<T: DriverObject> Sealed for Object<T> {}

impl<T: DriverObject> gem::IntoGEMObject for Object<T> {
    fn as_raw(&self) -> *mut bindings::drm_gem_object {
        // SAFETY:
        // - Our immutable reference is proof that this is safe to dereference.
        // - `obj` is always a valid drm_gem_shmem_object via our type invariants.
        unsafe { &raw mut (*self.obj.get()).base }
    }

    unsafe fn from_raw<'a>(obj: *mut bindings::drm_gem_object) -> &'a Object<T> {
        // SAFETY: The safety contract of from_gem_obj() guarantees that `obj` is contained within
        // `Self`
        unsafe {
            let obj = Opaque::cast_from(container_of!(obj, bindings::drm_gem_shmem_object, base));

            &*container_of!(obj, Object<T>, obj)
        }
    }
}

impl<T: DriverObject> driver::AllocImpl for Object<T> {
    type Driver = T::Driver;

    const ALLOC_OPS: driver::AllocOps = driver::AllocOps {
        gem_create_object: None,
        prime_handle_to_fd: None,
        prime_fd_to_handle: None,
        gem_prime_import: None,
        gem_prime_import_sg_table: Some(bindings::drm_gem_shmem_prime_import_sg_table),
        dumb_create: Some(bindings::drm_gem_shmem_dumb_create),
        dumb_map_offset: None,
    };
}

/// Private helper-type for holding the `dma_resv` object for a GEM shmem object.
///
/// When this is dropped, the `dma_resv` lock is dropped as well.
///
// TODO: This should be replace with a WwMutex equivalent once we have such bindings in the kernel.
struct DmaResvGuard<'a, T: DriverObject>(&'a Object<T>);

impl<'a, T: DriverObject> DmaResvGuard<'a, T> {
    #[inline(always)]
    fn new(obj: &'a Object<T>) -> Self {
        // SAFETY: This lock is initialized throughout the lifetime of `object`.
        unsafe { bindings::dma_resv_lock(obj.raw_dma_resv(), ptr::null_mut()) };

        Self(obj)
    }
}

impl<'a, T: DriverObject> Drop for DmaResvGuard<'a, T> {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: We are releasing the lock grabbed during the creation of this object.
        unsafe { bindings::dma_resv_unlock(self.0.raw_dma_resv()) };
    }
}

/// A reference to a GEM object that is known to have a mapped [`SGTable`].
///
/// This is used by the Rust bindings with [`Devres`] in order to ensure that mappings for SGTables
/// on GEM shmem objects are revoked on driver-unbind.
///
/// # Invariants
///
/// - `self.obj` always points to a valid GEM object.
/// - This object is proof that `self.obj.owner.sgt` has an initialized and valid
///   [`scatterlist::SGTable`].
pub struct SGTableMap<T: DriverObject> {
    obj: NonNull<Object<T>>,
}

impl<T: DriverObject> Deref for SGTableMap<T> {
    type Target = scatterlist::SGTable;

    fn deref(&self) -> &Self::Target {
        // SAFETY:
        // - The NonNull is guaranteed to be valid via our type invariants.
        // - The sgt field is guaranteed to be initialized and valid via our type invariants.
        unsafe { scatterlist::SGTable::from_raw((*self.obj.as_ref().as_raw_shmem()).sgt) }
    }
}

impl<T: DriverObject> Drop for SGTableMap<T> {
    fn drop(&mut self) {
        // SAFETY: `obj` is always valid via our type invariants
        let obj = unsafe { self.obj.as_ref() };
        let _lock = DmaResvGuard::new(obj);

        // SAFETY: We acquired the lock needed for calling this function above
        unsafe { bindings::__drm_gem_shmem_free_sgt_locked(obj.as_raw_shmem()) };
    }
}

impl<T: DriverObject> SGTableMap<T> {
    fn new(obj: &Object<T>) -> impl Init<Self, Error> {
        // INVARIANT:
        // - We call drm_gem_shmem_get_pages_sgt_locked below and check whether or not it
        //   succeeds, fulfilling the invariant of SGTableMap that the object's `sgt` field is
        //   initialized.
        // SAFETY:
        // - `obj` is fully initialized, making this function safe to call.
        from_err_ptr(unsafe { bindings::drm_gem_shmem_get_pages_sgt(obj.as_raw_shmem()) })?;

        Ok(Self { obj: obj.into() })
    }
}

// SAFETY: The NonNull in SGTableMap is guaranteed valid by our type invariants, and the GEM object
// it points to is guaranteed to be thread-safe.
unsafe impl<T: DriverObject> Send for SGTableMap<T> {}
// SAFETY: The NonNull in SGTableMap is guaranteed valid by our type invariants, and the GEM object
// it points to is guaranteed to be thread-safe.
unsafe impl<T: DriverObject> Sync for SGTableMap<T> {}

/// An owned reference to a scatter-gather table of DMA address spans for a GEM shmem object.
///
/// This object holds an owned reference to the underlying GEM shmem object, ensuring that the
/// [`scatterlist::SGTable`] referenced by this type remains valid for the lifetime of this object.
///
/// # Invariants
///
/// - This type is proof that `self.0.sgt_res` is initialized with a `Devres<SGTableMap<T>>`.
/// - This object is only exposed in situations where we know the underlying `SGTable` will not be
///   modified for the lifetime of this object. Thus, it is safe to send/access this type across
///   threads.
pub struct SGTable<T: DriverObject>(ARef<Object<T>>);

// SAFETY: This object is thread-safe via our type invariants.
unsafe impl<T: DriverObject> Send for SGTable<T> {}
// SAFETY: This object is thread-safe via our type invariants.
unsafe impl<T: DriverObject> Sync for SGTable<T> {}

impl<T: DriverObject> Deref for SGTable<T> {
    type Target = Devres<SGTableMap<T>>;

    #[inline(always)]
    fn deref(&self) -> &Self::Target {
        // TODO: This is Bad, but we'll replace this and do better once we can pass an Init to
        // SetOnce.
        // SAFETY:
        // - We only write to this location in two places:
        //   - Object::<T>::get_sg_table()
        //   - Object::<T>::free_callback() (which gets called by gem shmem helpers after the last
        //     gem object ref drop).
        //   Our type is proof that get_sg_table() has been called previously, and also proof that
        //   `free_callback()` has not been called - meaning that we can assume no races will occur
        //   for the lifetime of this object and we can safely access the Mutex contents without
        //   actually locking it.
        // - Since we proved get_sg_table() has been called, we know that sgt_res is already
        //   populated and thus unwrap_unchecked() is safe to call.
        unsafe { (*self.0.sgt_res.data.get()).as_ref().unwrap_unchecked() }
    }
}
