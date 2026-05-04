// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM device.
//!
//! C header: [`include/drm/drm_device.h`](srctree/include/drm/drm_device.h)

use crate::{
    alloc::allocator::Kmalloc,
    bindings,
    device::{
        self,
        AsBusDevice as _, //
    },
    drm::{
        self,
        driver::AllocImpl,
        private::Sealed, //
    },
    error::from_err_ptr,
    prelude::*,
    sync::aref::{
        ARef,
        AlwaysRefCounted, //
    },
    types::{
        ForLt,
        NotThreadSafe,
        Opaque, //
    },
    workqueue::{
        HasDelayedWork,
        HasWork,
        Work,
        WorkItem, //
    }, //
};
use core::{
    alloc::Layout,
    cell::UnsafeCell,
    marker::PhantomData,
    mem,
    ops::Deref,
    ptr::{
        self,
        NonNull, //
    },
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

/// A trait implemented by all possible contexts a [`Device`] can be used in.
///
/// Setting up a new [`Device`] is a multi-stage process. Each step of the process that a user
/// interacts with in Rust has a respective [`DeviceContext`] typestate. For example,
/// `Device<T, Registered>` would be a [`Device`] that reached the [`Registered`] [`DeviceContext`].
///
/// Each stage of this process is described below:
///
/// ```text
///        1                     2                         3
/// +--------------+   +------------------+   +-----------------------+
/// |Device created| → |Device initialized| → |Registered w/ userspace|
/// +--------------+   +------------------+   +-----------------------+
///    (Uninit)                                      (Registered)
/// ```
///
/// 1. The [`Device`] is in the [`Uninit`] context and is not guaranteed to be initialized or
///    registered with userspace. Only a limited subset of DRM core functionality is available.
/// 2. The [`Device`] is guaranteed to be fully initialized, but is not guaranteed to be registered
///    with userspace. All DRM core functionality which doesn't interact with userspace is
///    available. We currently don't have a context for representing this.
/// 3. The [`Device`] is guaranteed to be fully initialized, and is guaranteed to have been
///    registered with userspace at some point - thus putting it in the [`Registered`] context.
///
/// An important caveat of [`DeviceContext`] which must be kept in mind: when used as a typestate
/// for a reference type, it can only guarantee that a [`Device`] reached a particular stage in the
/// initialization process _at the time the reference was taken_. No guarantee is made in regards to
/// what stage of the process the [`Device`] is currently in. This means for instance that a
/// `&Device<T, Uninit>` may actually be registered with userspace, it just wasn't known to be
/// registered at the time the reference was taken.
pub trait DeviceContext: Sealed + Send + Sync {}

/// The [`DeviceContext`] of a [`Device`] that was registered with userspace at some point.
///
/// This represents a [`Device`] which is guaranteed to have been registered with userspace at
/// some point in time. Such a DRM device is guaranteed to have been fully-initialized.
///
/// Note: A device in this context is not guaranteed to remain registered with userspace for its
/// entire lifetime, as this is impossible to guarantee at compile-time.
///
/// # Invariants
///
/// A [`Device`] in this [`DeviceContext`] is guaranteed to have been registered with userspace
/// at some point in time.
pub struct Registered;

impl Sealed for Registered {}
impl DeviceContext for Registered {}

/// The [`DeviceContext`] of a [`Device`] that may be unregistered and partly uninitialized.
///
/// A [`Device`] in this context is only guaranteed to be partly initialized, and may or may not
/// be registered with userspace. Thus operations which depend on the [`Device`] being fully
/// initialized, or which depend on the [`Device`] being registered with userspace are not
/// available through this [`DeviceContext`].
///
/// A [`Device`] in this context can be used to create a
/// [`Registration`](drm::driver::Registration).
pub struct Uninit;

impl Sealed for Uninit {}
impl DeviceContext for Uninit {}

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
    const fn compute_features() -> u32 {
        let mut features = drm::driver::FEAT_GEM;

        if T::FEAT_RENDER {
            features |= drm::driver::FEAT_RENDER;
        }

        features
    }

    const VTABLE: bindings::drm_driver = drm_legacy_fields! {
        load: None,
        open: Some(drm::File::<T::File>::open_callback),
        postclose: Some(drm::File::<T::File>::postclose_callback),
        unload: None,
        release: Some(Device::<T>::release),
        master_set: None,
        master_drop: None,
        debugfs_init: None,

        // Ignore the Uninit DeviceContext below. It is only provided because it is required by the
        // compiler, and it is not actually used by these functions.
        gem_create_object: T::Object::<Uninit>::ALLOC_OPS.gem_create_object,
        prime_handle_to_fd: T::Object::<Uninit>::ALLOC_OPS.prime_handle_to_fd,
        prime_fd_to_handle: T::Object::<Uninit>::ALLOC_OPS.prime_fd_to_handle,
        gem_prime_import: T::Object::<Uninit>::ALLOC_OPS.gem_prime_import,
        gem_prime_import_sg_table: T::Object::<Uninit>::ALLOC_OPS.gem_prime_import_sg_table,
        dumb_create: T::Object::<Uninit>::ALLOC_OPS.dumb_create,
        dumb_map_offset: T::Object::<Uninit>::ALLOC_OPS.dumb_map_offset,

        show_fdinfo: None,
        fbdev_probe: None,

        major: T::INFO.major,
        minor: T::INFO.minor,
        patchlevel: T::INFO.patchlevel,
        name: crate::str::as_char_ptr_in_const_context(T::INFO.name).cast_mut(),
        desc: crate::str::as_char_ptr_in_const_context(T::INFO.desc).cast_mut(),

        driver_features: Self::compute_features(),
        ioctls: T::IOCTLS.as_ptr(),
        num_ioctls: T::IOCTLS.len() as i32,
        fops: &Self::GEM_FOPS,
    };

    const GEM_FOPS: bindings::file_operations = drm::gem::create_fops();

    /// Create a new `UnregisteredDevice` for a `drm::Driver`.
    ///
    /// This can be used to create a [`Registration`](kernel::drm::Registration).
    pub fn new(
        dev: &T::ParentDevice<device::Bound>,
        data: impl PinInit<T::Data, Error>,
    ) -> Result<Self> {
        // `__drm_dev_alloc` uses `kmalloc()` to allocate memory, hence ensure a `kmalloc()`
        // compatible `Layout`.
        let layout = Kmalloc::aligned_layout(Layout::new::<Device<T, Uninit>>());

        // Use a temporary vtable without a `release` callback until `data` is initialized, so
        // init failure can release the DRM device without dropping uninitialized fields.
        let alloc_vtable = bindings::drm_driver {
            release: None,
            ..Self::VTABLE
        };

        // SAFETY:
        // - `alloc_vtable` reference remains valid until no longer used,
        // - `dev` is valid by its type invarants,
        let raw_drm: *mut Device<T, Uninit> = unsafe {
            bindings::__drm_dev_alloc(
                dev.as_ref().as_raw(),
                &alloc_vtable,
                layout.size(),
                mem::offset_of!(Device<T, Uninit>, dev),
            )
        }
        .cast();
        let raw_drm = NonNull::new(from_err_ptr(raw_drm)?).ok_or(ENOMEM)?;

        // SAFETY: `raw_drm` is a valid pointer to `Self`, given that `__drm_dev_alloc` was
        // successful.
        let drm_dev = unsafe { Device::into_drm_device(raw_drm) };

        // SAFETY: `raw_drm` is a valid pointer to `Self`.
        let raw_data = unsafe { ptr::addr_of_mut!((*raw_drm.as_ptr()).data) };

        // SAFETY:
        // - `raw_data` is a valid pointer to uninitialized memory.
        // - `raw_data` will not move until it is dropped.
        unsafe { data.__pinned_init(raw_data) }.inspect_err(|_| {
            // SAFETY: `__drm_dev_alloc()` was successful, hence `drm_dev` must be valid and the
            // refcount must be non-zero.
            unsafe { bindings::drm_dev_put(drm_dev) };
        })?;

        // SAFETY: `drm_dev` is still private to this function.
        unsafe { (*drm_dev).driver = const { &Self::VTABLE } };

        // SAFETY: `raw_drm` is valid; no concurrent access before registration.
        unsafe { (*raw_drm.as_ptr()).registration_data = UnsafeCell::new(NonNull::dangling()) };

        // SAFETY: The reference count is one, and now we take ownership of that reference as a
        // `drm::Device`.
        // INVARIANT: We just created the device above, but have yet to call `drm_dev_register`.
        // `Self` cannot be copied or sent to another thread - ensuring that `drm_dev_register`
        // won't be called during its lifetime and that the device is unregistered.
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
/// behavior of such a device is still well-defined. Additionally, a device with the registration
/// state [`Uninit`] simply does not have a guaranteed registration state at compile time, and could
/// be either registered or unregistered. Since there is no way to guarantee a long-lived reference
/// to an unregistered device would remain unregistered, we do not provide a [`DeviceContext`] for
/// this.
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
    pub(super) registration_data: UnsafeCell<NonNull<<T::RegistrationData as ForLt>::Of<'static>>>,
    _ctx: PhantomData<C>,
}

impl<T: drm::Driver, C: DeviceContext> Device<T, C> {
    pub(crate) fn as_raw(&self) -> *mut bindings::drm_device {
        self.dev.get()
    }

    /// Returns a reference to the registration data with lifetime shortened from `'static`.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    ///
    /// - The parent bus device is bound (e.g. by holding an active `drm_dev_enter()` critical
    ///   section via [`UnbindGuard`]).
    /// - The returned reference is not exposed to code that can choose a concrete lifetime for
    ///   it, as that would be unsound for types that are invariant over their lifetime parameter
    ///   (e.g. it must be passed through an HRTB-bounded closure).
    unsafe fn registration_data_unchecked(&self) -> &<T::RegistrationData as ForLt>::Of<'_> {
        // SAFETY: Caller guarantees the parent bus device is bound, hence
        // the pointer is valid.
        let static_ref = unsafe { (*self.registration_data.get()).as_ref() };

        // SAFETY: Caller guarantees the reference is only used behind an HRTB, making the
        // round-trip from `'static` sound regardless of variance.
        unsafe { T::RegistrationData::cast_ref_unchecked(static_ref) }
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

impl<T: drm::Driver> Device<T, Registered> {
    /// Guard against the parent bus device being unbound.
    ///
    /// Returns an [`UnbindGuard`] if the device has not been unplugged, [`None`] otherwise.
    ///
    /// The returned guard dereferences to the parent bus device in the [`device::Bound`] context
    /// (see [`Driver::ParentDevice`](drm::Driver::ParentDevice)).
    ///
    /// While [`UnbindGuard`] is held the parent device is guaranteed to be bound.
    #[must_use]
    pub fn unbind_guard(&self) -> Option<UnbindGuard<'_, T>> {
        let mut idx: i32 = 0;
        // SAFETY: `self.as_raw()` is a valid pointer to a `struct drm_device` by the type
        // invariants of `Device<T, Registered>`.
        if unsafe { bindings::drm_dev_enter(self.as_raw(), &mut idx) } {
            Some(UnbindGuard { dev: self, idx })
        } else {
            None
        }
    }

    /// Execute a closure while the parent bus device is guaranteed to be bound.
    ///
    /// Acquires the [`UnbindGuard`] and, if the device has not been unplugged, calls `f` with the
    /// parent bus device. Returns `None` if the device has been unplugged.
    pub fn with_unbind_guard<R>(
        &self,
        f: impl FnOnce(&T::ParentDevice<device::Bound>) -> R,
    ) -> Option<R> {
        let guard = self.unbind_guard()?;
        Some(f(&guard))
    }
}

/// A guard preventing the parent bus device from being unbound.
///
/// The guard dereferences to [`Driver::ParentDevice<Bound>`](drm::Driver::ParentDevice), providing
/// access to the parent bus device with the guarantee that it is bound for the entire duration of
/// the critical section.
///
/// Internally this is backed by a `drm_dev_enter()` / `drm_dev_exit()` SRCU critical section.
///
/// See [`Device::unbind_guard`] for details on the safety argument.
///
/// # Invariants
///
/// - `idx` is the SRCU read lock index returned by a successful `drm_dev_enter()` call.
/// - The parent bus device of `dev` is bound for the lifetime of this guard.
#[must_use]
pub struct UnbindGuard<'a, T: drm::Driver> {
    dev: &'a Device<T, Registered>,
    idx: i32,
}

impl<T: drm::Driver> UnbindGuard<'_, T> {
    /// Access the parent device and registration data through a closure, with both lifetimes
    /// tied to the closure scope.
    ///
    /// The data is owned by [`Registration`](drm::Registration) and is guaranteed to remain valid
    /// for the duration of this guard, since [`Registration`](drm::Registration)'s `drop` calls
    /// `drm_dev_unplug()` which waits for all `drm_dev_enter()` critical sections to complete.
    pub fn registration_data_with<R, F>(&self, f: F) -> R
    where
        F: for<'a> FnOnce(
            &'a T::ParentDevice<device::Bound>,
            &'a <T::RegistrationData as ForLt>::Of<'a>,
        ) -> R,
    {
        // SAFETY: We hold an active `drm_dev_enter()` critical section, so the parent bus
        // device is guaranteed to be bound. The closure's HRTB `for<'a>` prevents the caller
        // from smuggling in references with a concrete short lifetime, satisfying the lifetime
        // requirement of `registration_data_unchecked`.
        f(&**self, unsafe { self.dev.registration_data_unchecked() })
    }
}

impl<T: drm::Driver> Deref for UnbindGuard<'_, T> {
    type Target = T::ParentDevice<device::Bound>;

    #[inline]
    fn deref(&self) -> &Self::Target {
        // SAFETY:
        // - The parent `struct device` is embedded in a `T::ParentDevice`, as guaranteed by
        //   `UnregisteredDevice::new` taking a `&T::ParentDevice<device::Bound>`.
        // - By the type invariants of `UnbindGuard`, the parent device is bound for the lifetime
        //   of this guard.
        unsafe { T::ParentDevice::from_device(self.dev.as_ref().as_bound()) }
    }
}

impl<T: drm::Driver> Drop for UnbindGuard<'_, T> {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: `self.idx` was returned by a successful `drm_dev_enter()` call, as guaranteed
        // by the type invariants of `UnbindGuard`.
        unsafe { bindings::drm_dev_exit(self.idx) };
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

impl<T, C, const ID: u64> WorkItem<ID> for Device<T, C>
where
    T: drm::Driver,
    T::Data: WorkItem<ID, Pointer = ARef<Self>>,
    T::Data: HasWork<Self, ID>,
    C: DeviceContext,
{
    type Pointer = ARef<Self>;

    fn run(ptr: ARef<Self>) {
        T::Data::run(ptr);
    }
}

// SAFETY:
//
// - `raw_get_work` and `work_container_of` return valid pointers by relying on
// `T::Data::raw_get_work` and `container_of`. In particular, `T::Data` is
// stored inline in `drm::Device`, so the `container_of` call is valid.
//
// - The two methods are true inverses of each other: given `ptr: *mut
// Device<T, C>`, `raw_get_work` will return a `*mut Work<Device<T, C>, ID>` through
// `T::Data::raw_get_work` and given a `ptr: *mut Work<Device<T, C>, ID>`,
// `work_container_of` will return a `*mut Device<T, C>` through `container_of`.
unsafe impl<T, C, const ID: u64> HasWork<Self, ID> for Device<T, C>
where
    T: drm::Driver,
    T::Data: HasWork<Self, ID>,
    C: DeviceContext,
{
    unsafe fn raw_get_work(ptr: *mut Self) -> *mut Work<Self, ID> {
        // SAFETY: The caller promises that `ptr` points to a valid `Device<T, C>`.
        let data_ptr = unsafe { &raw mut (*ptr).data };

        // SAFETY: `data_ptr` is a valid pointer to `T::Data`.
        unsafe { T::Data::raw_get_work(data_ptr) }
    }

    unsafe fn work_container_of(ptr: *mut Work<Self, ID>) -> *mut Self {
        // SAFETY: The caller promises that `ptr` points at a `Work` field in
        // `T::Data`.
        let data_ptr = unsafe { T::Data::work_container_of(ptr) };

        // SAFETY: `T::Data` is stored as the `data` field in `Device<T, C>`.
        unsafe { crate::container_of!(data_ptr, Self, data) }
    }
}

// SAFETY: Our `HasWork<T, ID>` implementation returns a `work_struct` that is
// stored in the `work` field of a `delayed_work` with the same access rules as
// the `work_struct` owing to the bound on `T::Data: HasDelayedWork<Device<T, C>,
// ID>`, which requires that `T::Data::raw_get_work` return a `work_struct` that
// is inside a `delayed_work`.
unsafe impl<T, C, const ID: u64> HasDelayedWork<Self, ID> for Device<T, C>
where
    T: drm::Driver,
    T::Data: HasDelayedWork<Self, ID>,
    C: DeviceContext,
{
}
