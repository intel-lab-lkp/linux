// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM device.
//!
//! C header: [`include/drm/drm_device.h`](srctree/include/drm/drm_device.h)

use crate::{
    alloc::allocator::Kmalloc,
    bindings, device, drm,
    drm::{driver::AllocImpl, private::Sealed},
    error::from_err_ptr,
    error::Result,
    prelude::*,
    sync::aref::{ARef, AlwaysRefCounted},
    types::{NotThreadSafe, Opaque},
};
use core::{
    alloc::Layout,
    marker::PhantomData,
    mem::{self},
    ops::Deref,
    ptr::{self, NonNull},
};

#[cfg(CONFIG_DRM_LEGACY)]
macro_rules! drm_legacy_fields {
    ( $($field:ident: $val:expr),* $(,)? ) => {
        bindings::drm_driver {
            $( $field: $val ),*,
            firstopen: None,
            preclose: None,
            dma_ioctl: None,
            dma_quiescent: None,
            context_dtor: None,
            irq_handler: None,
            irq_preinstall: None,
            irq_postinstall: None,
            irq_uninstall: None,
            get_vblank_counter: None,
            enable_vblank: None,
            disable_vblank: None,
            dev_priv_size: 0,
        }
    }
}

#[cfg(not(CONFIG_DRM_LEGACY))]
macro_rules! drm_legacy_fields {
    ( $($field:ident: $val:expr),* $(,)? ) => {
        bindings::drm_driver {
            $( $field: $val ),*
        }
    }
}

macro_rules! drm_dev_ctx {
    (
        $( #[$attrs:meta] )*
        $name:ident
    ) => {
        $( #[$attrs] )*
        pub struct $name;

        impl DeviceContext for $name {}
        impl Sealed for $name {}

        // SAFETY: All registration states are free of side-effects (e.g. no Drop) and are ZSTs,
        // thus they are always thread-safe.
        unsafe impl Send for $name {}
        // SAFETY: All registration states are free of side-effects (e.g. no Drop) and are ZSTs,
        // thus they are always thread-safe.
        unsafe impl Sync for $name {}
    };
}

/// A trait implemented by all possible contexts a [`Device`] can be used in.
pub trait DeviceContext: Sealed + Send + Sync {}

drm_dev_ctx! {
    /// The [`DeviceContext`] of a [`Device`] that was registered with userspace at some point.
    ///
    /// This represents a [`Device`] which is guaranteed to have been registered with userspace at
    /// some point in time. Such a DRM device is guaranteed to have been fully-initialized.
    ///
    /// Note: A device in this context is not guaranteed to remain registered with userspace for its
    /// entire lifetime, as this is impossible to guarantee at compile-time. However, any
    /// userspace-dependent operations performed with an unregistered device in this [`DeviceContext`]
    /// are guaranteed to be no-ops.
    ///
    /// # Invariants
    ///
    /// A [`Device`] in this [`DeviceContext`] is guaranteed to have called `drm_dev_register` once.
    Registered
}

drm_dev_ctx! {
    /// The [`DeviceContext`] of a [`Device`] that may be unregistered and partly uninitialized.
    ///
    /// A [`Device`] in this context is only guaranteed to be partly initialized, and may or may not
    /// be registered with userspace. Thus operations which depend on the [`Device`] being fully
    /// initialized, or which depend on the [`Device`] being registered with userspace are not
    /// available through this [`DeviceContext`].
    ///
    /// A [`Device`] in this context can be used to create a
    /// [`Registration`](drm::driver::Registration).
    Uninit
}

/// A [`Device`] which is known at compile-time to be unregistered with userspace.
///
/// This type allows performing operations which are only safe to do before userspace registration,
/// and can be used to create a [`Registration`](drm::driver::Registration) once the driver is ready
/// to register the device with userspace.
///
/// Since DRM device initialization must be single-threaded, this object is not thread-safe.
///
/// # Invariants
///
/// The device in `self.0` is guaranteed to be a newly created [`Device`] that has not yet been
/// registered with userspace until this type is dropped.
pub struct UnregisteredDevice<T: drm::Driver>(ARef<Device<T, Uninit>>, NotThreadSafe);

impl<T: drm::Driver> Deref for UnregisteredDevice<T> {
    type Target = Device<T, Uninit>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl<T: drm::Driver> UnregisteredDevice<T> {
    const VTABLE: bindings::drm_driver = drm_legacy_fields! {
        load: None,
        open: Some(drm::File::<T::File>::open_callback),
        postclose: Some(drm::File::<T::File>::postclose_callback),
        unload: None,
        release: Some(Device::<T>::release),
        master_set: None,
        master_drop: None,
        debugfs_init: None,
        gem_create_object: T::Object::ALLOC_OPS.gem_create_object,
        prime_handle_to_fd: T::Object::ALLOC_OPS.prime_handle_to_fd,
        prime_fd_to_handle: T::Object::ALLOC_OPS.prime_fd_to_handle,
        gem_prime_import: T::Object::ALLOC_OPS.gem_prime_import,
        gem_prime_import_sg_table: T::Object::ALLOC_OPS.gem_prime_import_sg_table,
        dumb_create: T::Object::ALLOC_OPS.dumb_create,
        dumb_map_offset: T::Object::ALLOC_OPS.dumb_map_offset,
        show_fdinfo: None,
        fbdev_probe: None,

        major: T::INFO.major,
        minor: T::INFO.minor,
        patchlevel: T::INFO.patchlevel,
        name: crate::str::as_char_ptr_in_const_context(T::INFO.name).cast_mut(),
        desc: crate::str::as_char_ptr_in_const_context(T::INFO.desc).cast_mut(),

        driver_features: drm::driver::FEAT_GEM,
        ioctls: T::IOCTLS.as_ptr(),
        num_ioctls: T::IOCTLS.len() as i32,
        fops: &Self::GEM_FOPS,
    };

    const GEM_FOPS: bindings::file_operations = drm::gem::create_fops();

    /// Create a new `UnregisteredDevice` for a `drm::Driver`.
    ///
    /// This can be used to create a [`Registration`](kernel::drm::Registration).
    pub fn new(dev: &device::Device, data: impl PinInit<T::Data, Error>) -> Result<Self> {
        // `__drm_dev_alloc` uses `kmalloc()` to allocate memory, hence ensure a `kmalloc()`
        // compatible `Layout`.
        let layout = Kmalloc::aligned_layout(Layout::new::<Self>());

        // SAFETY:
        // - `VTABLE`, as a `const` is pinned to the read-only section of the compilation,
        // - `dev` is valid by its type invarants,
        let raw_drm: *mut Device<T, Uninit> = unsafe {
            bindings::__drm_dev_alloc(
                dev.as_raw(),
                &Self::VTABLE,
                layout.size(),
                mem::offset_of!(Device<T, Uninit>, dev),
            )
        }
        .cast();
        let raw_drm = NonNull::new(from_err_ptr(raw_drm)?).ok_or(ENOMEM)?;

        // SAFETY: `raw_drm` is a valid pointer to `Self`.
        let raw_data = unsafe { ptr::addr_of_mut!((*raw_drm.as_ptr()).data) };

        // SAFETY:
        // - `raw_data` is a valid pointer to uninitialized memory.
        // - `raw_data` will not move until it is dropped.
        unsafe { data.__pinned_init(raw_data) }.inspect_err(|_| {
            // SAFETY: `raw_drm` is a valid pointer to `Self`, given that `__drm_dev_alloc` was
            // successful.
            let drm_dev = unsafe { Device::into_drm_device(raw_drm) };

            // SAFETY: `__drm_dev_alloc()` was successful, hence `drm_dev` must be valid and the
            // refcount must be non-zero.
            unsafe { bindings::drm_dev_put(drm_dev) };
        })?;

        // SAFETY: The reference count is one, and now we take ownership of that reference as a
        // `drm::Device`.
        // INVARIANT: We just created the device above, but have yet to call `drm_dev_register`.
        // `Self` cannot be copied or sent to another thread - ensuring that `drm_dev_register`
        // won't be called during its lifetime and that the device is unregistered
        Ok(Self(unsafe { ARef::from_raw(raw_drm) }, NotThreadSafe))
    }
}

/// A typed DRM device with a specific [`drm::Driver`] implementation and [`DeviceContext`].
///
/// Since DRM devices can be used before being fully initialized and registered with userspace, `C`
/// represents the furthest [`DeviceContext`] we can guarantee that this [`Device`] has reached.
///
/// Keep in mind: this means that an unregistered device can still have the registration state
/// [`Registered`] as long as it was registered with userspace once in the past, and that the
/// behavior of such a device is still well-defined. In such a situation, the behavior of any
/// functions which interact with userspace will simply be no-ops. Additionally, a device with the
/// registration state [`Uninit`] simply does not have a guaranteed registration state at compile
/// time, and could be either registered or unregistered. Since there is no way to guarantee a
/// long-lived reference to an unregistered device would remain unregistered, we do not provide a
/// [`DeviceContext`] for this.
///
/// # Invariants
///
/// * `self.dev` is a valid instance of a `struct device`.
/// * The data layout of `Self` remains the same across all implementations of `C`.
/// * Any invariants for `C` also apply.
#[repr(C)]
pub struct Device<T: drm::Driver, C: DeviceContext = Registered> {
    dev: Opaque<bindings::drm_device>,
    data: T::Data,
    _ctx: PhantomData<C>,
}

impl<T: drm::Driver, C: DeviceContext> Device<T, C> {
    pub(crate) fn as_raw(&self) -> *mut bindings::drm_device {
        self.dev.get()
    }

    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `struct device` embedded in `Self`.
    unsafe fn from_drm_device(ptr: *const bindings::drm_device) -> *mut Self {
        // SAFETY: By the safety requirements of this function `ptr` is a valid pointer to a
        // `struct drm_device` embedded in `Self`.
        unsafe { crate::container_of!(Opaque::cast_from(ptr), Self, dev) }.cast_mut()
    }

    /// # Safety
    ///
    /// `ptr` must be a valid pointer to `Self`.
    unsafe fn into_drm_device(ptr: NonNull<Self>) -> *mut bindings::drm_device {
        // SAFETY: By the safety requirements of this function, `ptr` is a valid pointer to `Self`.
        unsafe { &raw mut (*ptr.as_ptr()).dev }.cast()
    }

    /// Not intended to be called externally, except via declare_drm_ioctls!()
    ///
    /// # Safety
    ///
    /// * Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count,
    ///   i.e. it must be ensured that the reference count of the C `struct drm_device` `ptr` points
    ///   to can't drop to zero, for the duration of this function call and the entire duration when
    ///   the returned reference exists.
    /// * Additionally, callers must ensure that the `struct device`, `ptr` is pointing to, is
    ///   embedded in `Self`.
    /// * Callers promise that any type invariants of `C` will be upheld.
    #[doc(hidden)]
    pub unsafe fn from_raw<'a>(ptr: *const bindings::drm_device) -> &'a Self {
        // SAFETY: By the safety requirements of this function `ptr` is a valid pointer to a
        // `struct drm_device` embedded in `Self`.
        let ptr = unsafe { Self::from_drm_device(ptr) };

        // SAFETY: `ptr` is valid by the safety requirements of this function.
        unsafe { &*ptr.cast() }
    }

    extern "C" fn release(ptr: *mut bindings::drm_device) {
        // SAFETY: `ptr` is a valid pointer to a `struct drm_device` and embedded in `Self`.
        let this = unsafe { Self::from_drm_device(ptr) };

        // SAFETY:
        // - When `release` runs it is guaranteed that there is no further access to `this`.
        // - `this` is valid for dropping.
        unsafe { core::ptr::drop_in_place(this) };
    }

    /// Change the [`DeviceContext`] for a [`Device`].
    ///
    /// # Safety
    ///
    /// The caller promises that `self` fulfills all of the guarantees provided by the given
    /// [`DeviceContext`].
    pub(crate) unsafe fn assume_ctx<NewCtx: DeviceContext>(&self) -> &Device<T, NewCtx> {
        // SAFETY: The data layout is identical via our type invariants.
        unsafe { mem::transmute(self) }
    }
}

impl<T: drm::Driver, C: DeviceContext> Deref for Device<T, C> {
    type Target = T::Data;

    fn deref(&self) -> &Self::Target {
        &self.data
    }
}

// SAFETY: DRM device objects are always reference counted and the get/put functions
// satisfy the requirements.
unsafe impl<T: drm::Driver, C: DeviceContext> AlwaysRefCounted for Device<T, C> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::drm_dev_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: `obj` is a valid pointer to `Self`.
        let drm_dev = unsafe { Self::into_drm_device(obj) };

        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::drm_dev_put(drm_dev) };
    }
}

impl<T: drm::Driver, C: DeviceContext> AsRef<device::Device> for Device<T, C> {
    fn as_ref(&self) -> &device::Device {
        // SAFETY: `bindings::drm_device::dev` is valid as long as the DRM device itself is valid,
        // which is guaranteed by the type invariant.
        unsafe { device::Device::from_raw((*self.as_raw()).dev) }
    }
}

// SAFETY: A `drm::Device` can be released from any thread.
unsafe impl<T: drm::Driver, C: DeviceContext> Send for Device<T, C> {}

// SAFETY: A `drm::Device` can be shared among threads because all immutable methods are protected
// by the synchronization in `struct drm_device`.
unsafe impl<T: drm::Driver, C: DeviceContext> Sync for Device<T, C> {}
