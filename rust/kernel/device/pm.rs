// SPDX-License-Identifier: GPL-2.0

//! Rust Runtime Power Management abstraction.
//!
//! C header: [`include/linux/pm_runtime.h`](srctree/include/linux/pm_runtime.h)

use crate::{
    bindings,
    bits::bit_u32,
    device::{
        Device,
        Bound,
        Core,
        AsBusDevice, //
    },
    driver,
    error::{
        to_result,
        VTABLE_DEFAULT_ERROR, //
    },
    prelude::*,
    sync::atomic::{
        ordering,
        Atomic,
        AtomicFlag,
        AtomicType, //
    },
    sync::Srcu,
    sync::srcu::new_srcu, //
};

use core::{
    cell::UnsafeCell,
    marker::PhantomData,
    mem::ManuallyDrop,
    ptr::NonNull, //
};

kernel::impl_flags! {
    /// Runtime Power Management modes that determine how a particular PM
    /// transition is to be carried out.
    /// Corresponds to C Runtime PM flag argument bits:
    /// - `RPM_ASYNC`
    /// - `RPM_NOWAIT`
    /// - `RPM_GET_PUT`
    /// - `RPM_AUTO`
    /// Synchronous mode is represented as `Mode::empty()`,
    /// which is the default mode.
    #[derive(Clone, Copy, PartialEq, Eq, Default, Debug)]
    pub struct Mode(u32);

    /// Single RPM mode
    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    pub enum ModeFlag {
        /// Allow asynchronous PM operations.
        Async = bindings::RPM_ASYNC,
        /// Do not wait for any pending requests to finish.
        Nowait = bindings::RPM_NOWAIT,
        /// Acquire a runtime-PM usage reference.
        Acquire = bindings::RPM_GET_PUT,
        /// Use autosuspend.
        Auto = bindings::RPM_AUTO,
        /// Additional mode for devices supporting idle states.
        /// No counterpart.
        Idle = bit_u32(16),
    }
}

impl From<Mode> for c_int {
    #[inline]
    fn from(mode: Mode) -> c_int {
        mode.0 as c_int
    }
}

/// Device's runtime power management status
#[derive(Copy,Clone)]
#[repr(i32)]
pub enum RuntimePMState {
    /// The device is expected to be runtime active and in its normal operating state
    Active = bindings::rpm_status_RPM_ACTIVE,
    /// The device is expected to be suspended, unavailable for normal operations
    Suspended = bindings::rpm_status_RPM_SUSPENDED,
}

/// Device's scoped runtime-resumed state
pub struct Scope<'a, Tag> {
    dev: &'a Device<Bound>,
    mode: Mode,
    _tag: PhantomData<Tag>,
}

/// Device resumed without incrementing the device's usage count
struct Resume;
/// Device resumed with the device's usage count being incremented
struct Awake;
/// Device with increased usage reference count
struct Retain;

/// Resumes the device without acquiring the usage reference.
/// Note: This does not guarantee the device will be kept active
/// for the lifetime of the scope due to potential pending/incoming
/// suspend requests.
/// Successfully obtaining the scope guard does not guarantee the device
/// has been resumed: subject to the requested mode:
/// `ModeFlag::Async` and `ModeFlag::Nowait`
///
/// On drop:
/// - If `ModeFlag::Idle`, calls `__pm_runtime_idle()`:
///   triggers idle notification before attempting to suspend
/// - If `ModeFlag::Auto`, marks last busy then calls `__pm_runtime_suspend()`.
/// - Otherwise calls `__pm_runtime_suspend()`.
///
/// The guard must be dropped from a context matching the requested transition
/// mode: sync vs async.
#[must_use = "dropping this guard issues the matching runtime PM release request"]
pub struct ResumeScope<'a>(Scope<'a, Resume>);

/// Acquires a runtime-PM usage reference and keeps the device powered.
/// Successfully obtaining the scope guard does not guarantee the device
/// has been resumed: subject to the requested mode:
/// `ModeFlag::Async` and `ModeFlag::Nowait`.
///
/// Requires `ModeFlag::Acquire`.
/// Drop behavior matches `ResumeScope`.
///
/// The guard must be dropped from a context matching the requested transition
/// mode: sync vs async.
#[must_use = "dropping this guard releases its runtime PM hold"]
pub struct AwakeScope<'a>(Scope<'a, Awake>);

/// Prevents the device from getting suspended by holding the usage reference
/// count.
///
/// On drop, calls `pm_runtime_put_noidle()`.
#[must_use = "dropping this guard releases its runtime PM hold"]
pub struct RetainScope<'a>(Scope<'a, Retain>);

impl<'a> ResumeScope<'a> {
    fn new(dev: &'a Device<Bound>, mode: Mode) -> Result<Self> {
        if mode.contains(ModeFlag::Acquire) {
            // ModeFlag::Acquire is intended to be used with Awake scope
            // Avoid mixing the modes.
            return Err(EINVAL);
        }

        // ModeFlag::Idle is internal so strip it off before passing further
        match Request::resume(dev, mode & !ModeFlag::Idle) {
            Ok(()) => {}
            //  if ModeFlag::Async is set, -EINPROGRESS signals ongoing resume,
            //  with ModeFlag::Nowait -> ongoing suspend with delayed resume
            //  being queued
            Err(e) if e == EINPROGRESS && mode.contains_any(ModeFlag::Async| ModeFlag::Nowait) => {}
            Err(e) => return Err(e),
        }

        Ok(Self(Scope::<Resume> {
                dev,
                mode,
                _tag: PhantomData,
        }))
    }

    fn release_inner(&self) -> Result {
        if !cfg!(CONFIG_PM) {
            return Ok(())
        }

        let mode = self.0.mode;
        let scope_mode = mode & !ModeFlag::Idle;

        if mode.contains(ModeFlag::Auto) {
            Request::mark_last_busy(self.0.dev);
        }

        match self.0.mode {
            mode if mode.contains(ModeFlag::Idle) => Request::idle(self.0.dev, scope_mode),
            _ => Request::suspend(self.0.dev, scope_mode),
        }
    }

    /// Explicitly release the scope
    ///
    /// This should be used in favor of regular drop when error handling is
    /// required.
    pub fn release(self) -> Result {
        let result = self.release_inner();
        core::mem::forget(self);
        result
    }
}

impl<'a> Drop for ResumeScope<'a> {
    fn drop(&mut self) {
        let _ = self.release_inner();
    }
}

impl<'a> AwakeScope<'a> {
    fn new(dev: &'a Device<Bound>, mode: Mode) -> Result<Self> {
        if !mode.contains(ModeFlag::Acquire) {
            return Err(EINVAL);
        }
        // ModeFlag::Idle is internal so strip it off before passing further
        match Request::resume(dev, mode & !ModeFlag::Idle) {
            Ok(()) => {}
            // For async/nowait requests, `EINPROGRESS` means the resume is
            // either in flight or pending.
            Err(e) if e == EINPROGRESS && mode.contains_any(ModeFlag::Async | ModeFlag::Nowait) => {}
            Err(e) => {
                Request::put_noidle(dev);
                return Err(e);
            }
        }

        Ok(Self(Scope::<Awake> {
            dev,
            mode,
            _tag: PhantomData,
        }))
    }

    fn release_inner(&self) -> Result {
        if !cfg!(CONFIG_PM) {
            return Ok(())
        }

        let mode = self.0.mode;
        let scope_mode = mode & !ModeFlag::Idle;

        if mode.contains(ModeFlag::Auto) {
            Request::mark_last_busy(self.0.dev);
        }

        match self.0.mode {
            mode if mode.contains(ModeFlag::Idle) => Request::idle(self.0.dev, scope_mode),
            _ => Request::suspend(self.0.dev, scope_mode),
        }
    }

    /// Explicitly release the scope
    ///
    /// This should be used in favor of regular drop when error handling is
    /// required.
    pub fn release(self) -> Result {
        let result = self.release_inner();
        core::mem::forget(self);
        result
    }
}

impl<'a> Drop for AwakeScope<'a> {
    fn drop(&mut self) {
        let _ = self.release_inner();
    }
}

impl<'a> RetainScope<'a> {
    fn new(dev: &'a Device<Bound>) -> Result<Self> {
        Request::get_noresume(dev);
        Ok(Self(Scope::<Retain> {
            dev,
            mode: Mode::empty(),
            _tag: PhantomData,
        }))
    }

    fn try_new(dev: &'a Device<Bound>) -> Result<Self> {
        Request::get_if_active(dev)?;
        Ok(Self(Scope::<Retain> {
            dev,
            mode: Mode(u32::from(Mode::empty())),
            _tag: PhantomData,
        }))
    }

    fn release_inner(&self) {
        Request::put_noidle(self.0.dev);
    }

    /// Explicitly release the scope
    ///
    /// This should be used in favor of regular drop when error handling is
    /// required.
    pub fn release(self) -> Result {
        self.release_inner();
        core::mem::forget(self);
        Ok(())
    }
}

impl<'a> Drop for RetainScope<'a> {
    fn drop(&mut self) {
        self.release_inner();
    }
}

/// Private entry points to the C runtime PM API.
///
/// Provides a centralized location for both, the C calls themsleves,
/// as well as for their safety requirements.
///
/// # Safety
///
/// Each method takes `&Device<Bound>`. `Device` guarantees that `as_raw()`
/// points to a valid `struct device` during the call. `Bound` guarantees that
/// the driver remains attached while the PM core may invoke callbacks using
/// driver-owned data. For requests completed later, [`Registration`] keeps
/// callback data alive and drains pending callbacks before dropping it.
struct Request;

impl Request {
    #[inline]
    fn active(dev: &Device<Bound>) -> bool {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe { bindings::pm_runtime_active(dev.as_raw()) }
    }

    #[inline]
    fn suspended(dev: &Device<Bound>) -> bool {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe { bindings::pm_runtime_suspended(dev.as_raw()) }
    }

    #[inline]
    fn resume(dev: &Device<Bound>, mode: Mode) -> Result {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        to_result(unsafe { bindings::__pm_runtime_resume(dev.as_raw(), mode.into()) })
    }

    #[inline]
    fn suspend(dev: &Device<Bound>, mode: Mode) -> Result {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        to_result(unsafe { bindings::__pm_runtime_suspend(dev.as_raw(), mode.into()) })
    }

    #[inline]
    fn get_noresume(dev: &Device<Bound>) {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe { bindings::pm_runtime_get_noresume(dev.as_raw()) };
    }

    #[inline]
    fn get_if_active(dev: &Device<Bound>) -> Result {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        match unsafe { bindings::pm_runtime_get_if_active(dev.as_raw()) } {
            ret if ret < 0 => Err(Error::from_errno(ret)),
            0 => Err(EAGAIN),
            _ => Ok(()),
        }
    }

    #[inline]
    fn idle(dev: &Device<Bound>, mode: Mode) -> Result {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        to_result(unsafe { bindings::__pm_runtime_idle(dev.as_raw(), mode.into()) })
    }

    #[inline]
    fn put_noidle(dev: &Device<Bound>) {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe { bindings::pm_runtime_put_noidle(dev.as_raw()) };
    }

    #[inline]
    fn mark_active(dev: &Device<Bound>) -> Result {
        to_result(
            // SAFETY: `dev` satisfies safety requirements documented on `Request`.
            unsafe { bindings::pm_runtime_set_active(dev.as_raw()) },
        )
    }

    #[inline]
    fn mark_suspended(dev: &Device<Bound>) -> Result {
        to_result(
            // SAFETY: `dev` satisfies safety requirements documented on `Request`.
            unsafe { bindings::pm_runtime_set_suspended(dev.as_raw()) },
        )
    }

    #[inline]
    fn mark_last_busy(dev: &Device<Bound>) {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe {
            bindings::pm_runtime_mark_last_busy(dev.as_raw());
        }
    }

    #[inline]
    fn runtime_enable(dev: &Device<Bound>) {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe { bindings::pm_runtime_enable(dev.as_raw()) }
    }

    #[inline]
    fn runtime_disable(dev: &Device<Bound>) {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe { bindings::__pm_runtime_disable(dev.as_raw(), true) };
    }

    #[inline]
    fn barrier(dev: &Device<Bound>) {
        // SAFETY: `dev` satisfies safety requirements documented on `Request`.
        unsafe {
            bindings::pm_runtime_barrier(dev.as_raw());
        }
    }
}

/// Common runtime PM callback entry point.
///
/// Invokes a runtime PM callback with its registration data and current payload.
///
/// # Safety
///
/// The generated extern "C" callbacks call into this helper with the raw
/// `struct device *` provided by the PM core. As such, it is guaranteed that
/// the `struct device` remains valid throughout the calls.
#[cfg(CONFIG_PM)]
unsafe fn with_registration_data<T, F>(dev: *mut bindings::device, cb: F) -> Result
where
    T: PMOps,
    F: for <'a>FnOnce(
        &'a <T as PMOps>::DeviceType,
        &'a <T as PMOps>::RegistrationData<'a>,
        RuntimeStatePayload<T::ActivePayload<'a>, T::SuspendedPayload<'a>>
     ) -> PMCallbackResult<RuntimeStatePayload<T::ActivePayload<'a>, T::SuspendedPayload<'a>>,
                           RuntimeStatePayload<T::ActivePayload<'a>, T::SuspendedPayload<'a>>>,
{
    // SAFETY: `dev` is provided by the PM core and remains valid
    // for the duration of the callback.
    let dev: &Device = unsafe { Device::from_raw(dev) };
    // SAFETY: The runtime PM callback can only be triggered for a bound device.
    let dev: &Device<Bound> =  unsafe { dev.as_bound() };
    let pm_dev: &T::DeviceType =
        // SAFETY: The generated `dev_pm_ops` for `T` is installed on devices whose
        // bus-specific type is `T::DeviceType`. Therefore the base `Device<Bound>`
        // passed by the PM core is embedded in a valid `T::DeviceType`; the
        // `AsBusDevice` implementation supplies the correct offset for this cast.
        unsafe { T::DeviceType::from_device(dev) };
    // SAFETY:  The `RegistrationData` must have been successfully installed
    // for a device and it is guaranteed that it reamins valid at least until
    // the last PM callback finishes.
    let ptr = unsafe { (*(*dev.as_raw()).p).rust.pm_registration_data };
    // SAFETY: `Registration::new` stores a pointer to its initialized, pinned
    // `RegistrationData<T>` before enabling runtime PM. These callbacks
    // belong to the same `T`. Registration teardown disables runtime PM and
    // drains callbacks before clearing the pointer and dropping the data, so
    // it remains valid for this callback
    let reg_data = unsafe { &*ptr.cast::<RegistrationData<'_, T>>() };

    let _guard = reg_data.payload.acquire()?;
    // SAFETY: This callback alone acquired `in_flight`; it has exclusive
    // access to the payload slot until `_guard` is dropped.
    let slot = unsafe { &mut *reg_data.payload.inner.get() };
    // SAFETY: `_guard` gives this callback exclusive access to `slot`, which
    // contains an initialized payload. Both callback outcomes provide a payload
    // that is written back into `slot` before `_guard` is released.
    let payload = unsafe { ManuallyDrop::take(slot) };

    let (next, result) = match cb(pm_dev, &reg_data.data, payload) {
                Ok(next) => (next, Ok(())),
                Err((current, err)) => (current, Err(err)),
    };

    *slot = ManuallyDrop::new(next);
    result
}

/// Runtime resume PM callback.
///
/// # Safety
///
/// `dev` is a valid `struct device *` supplied by the PM core for a device
/// whose runtime PM callbacks and PM registration data were both created
/// for `T`.
#[cfg(CONFIG_PM)]
unsafe extern "C" fn runtime_resume_callback<T>(dev: *mut bindings::device) -> c_int
where
    T: PMOps,
{
    // SAFETY: The callback's safety contract guarantees that `dev` remains
    // valid for this call and has PM registration data created for `T`.
    // These are the preconditions of `with_registration_data`.
    unsafe {
        with_registration_data::<T, _>(dev,
            |pm_dev, data, payload| {
                match payload {
                    RuntimeStatePayload::Suspended(payload_data) => {
                        T::runtime_resume(pm_dev, data, payload_data)
                            .map(RuntimeStatePayload::Active)
                            .map_err(|(prev, err)| (RuntimeStatePayload::Suspended(prev), err))
                    },
                    other => Err((other, EINVAL)),
                }
        })
        .map(|()| 0)
        .unwrap_or_else(|e| e.to_errno())
    }
}
/// Runtime suspend PM callback.
///
/// # Safety
///
/// `dev` is a valid `struct device *` supplied by the PM core for a device
/// whose runtime PM callbacks and PM registration data were both created
/// for `T`.
#[cfg(CONFIG_PM)]
unsafe extern "C" fn runtime_suspend_callback<T>(dev: *mut bindings::device) -> c_int
where
    T: PMOps,
{
    // SAFETY: The callback's safety contract guarantees that `dev` remains
    // valid for this call and has PM registration data created for `T`.
    // These are the preconditions of `with_registration_data`.
    unsafe {
        with_registration_data::<T, _>(dev,
            |pm_dev, data, payload| {
                match payload {
                    RuntimeStatePayload::Active(payload_data) => {
                        T::runtime_suspend(pm_dev, data, payload_data)
                            .map(RuntimeStatePayload::Suspended)
                            .map_err(|(prev, err)| (RuntimeStatePayload::Active(prev), err))
                    },
                    other => Err((other, EINVAL)),
                }
        })
        .map(|()| 0)
        .unwrap_or_else(|e| e.to_errno())
    }
}

/// Default empty Runtime PM ops
pub const PMOPS_NONE: bindings::dev_pm_ops =
    // SAFETY:
    // bindings::dev_pm_ops is #[repr(C)], implements Default
    // and the struct itself is all nullable function pointers.
    // There is no padding and all zero bit-pattern is valid
    //
    unsafe { core::mem::MaybeUninit::<bindings::dev_pm_ops>::zeroed().assume_init() };

/// Typed reference to a driver's runtime PM ops.
///
/// Instances of this type are generated from static [`Ops`]
/// and are used by bus registration code to install the matching
/// C `dev_pm_ops` pointer into the driver object.
/// The `A` parameter identifies the bus adapter that is allowed
/// to install the table.
pub struct AdapterOpsRef<'a, A> {
    #[cfg_attr(not(CONFIG_PM), allow(unused))]
    ops: &'a bindings::dev_pm_ops,
    _p: PhantomData<fn() -> *const A>,
}

/// Conveniance type alias
pub type OpsRef<T> = AdapterOpsRef<'static, <T as PMOps>::Adapter>;

// SAFETY: `AdapterOpsRef` only contains a shared reference to an immutable
// `dev_pm_ops` table and a type marker. Sharing it between threads cannot
// introduce mutation or aliasing of Rust-owned state.
unsafe impl<A> Sync for AdapterOpsRef<'_, A> {}

impl<A> AdapterOpsRef<'_, A> {
    #[cfg_attr(not(CONFIG_PM), allow(unused))]
    #[inline]
    pub(crate) const fn as_raw(&self) -> *const bindings::dev_pm_ops {
        self.ops
    }
}

/// Runtime PM ops for a driver.
///
/// Intended to be constructed by bus-specific helpers, allowing
/// the bus to ensure that the callback device type matches the device
/// supplied by the C driver core.
pub struct Ops<T: PMOps> {
    raw: bindings::dev_pm_ops,
    _p: PhantomData<fn() -> T>,
}

impl<T: PMOps> Ops<T> {
    /// Creates a typed runtime PM ops.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that `T::Adapter` is the bus adapter that
    /// will install this ops into the C driver object for `T`, and
    /// that `T::DeviceType` is the bus device type corresponding to
    /// that adapter. Safe bus-specific constructors should wrap this
    /// function with those bounds.
    pub(crate) const unsafe fn new_unchecked(raw: bindings::dev_pm_ops) -> Self {
        const_assert!(
            T::HAS_RUNTIME_RESUME == T::HAS_RUNTIME_SUSPEND,
            "runtime suspend and resume callbacks should be implemented together"
        );
        Self {
            raw,
            _p: PhantomData,
        }
    }

    /// Provides a typed reference to this runtime PM ops
    ///
    /// Bus registration code uses the returned value to install
    /// the ops into the corresponding C driver object.
    /// The returned reference is tied to `T::Adapter`.
    pub const fn pm_ops(&'static self) -> OpsRef<T> {
        AdapterOpsRef {
            ops: &self.raw,
            _p: PhantomData,
        }
    }

    /// Returns the raw C runtime PM operations table.
    #[inline]
    pub const fn as_raw(&'static self) -> *const bindings::dev_pm_ops {
        &self.raw
    }
}

/// Result type returned by runtime PM callbacks.
pub type PMCallbackResult<New, Current> = Result<New, (Current, Error)>;

/// Runtime PM callbacks implemented by a driver.
///
/// Defines the [`PMOps`] trait and its corresponding [`bindings::dev_pm_ops`].
///
/// The Adapter associated type identifies the bus adapter that installs
/// the generated PM ops.
/// Implementations should normally be enabled through a
/// bus-specific constructor for [`Ops`], which constrains
/// [`Self::DeviceType`] to the correct bound device type for that bus.
#[vtable]
pub trait PMOps: Sized {
    /// The bus adapter that installs the generated PM ops.
    type Adapter: driver::DriverLayout + 'static;

    /// Type of the bound bus device passed to runtime PM callbacks.
    type DeviceType: AsBusDevice<Bound>;

    /// Data associated with the `Registration`, tied to its lifetime.
    type RegistrationData<'a>: Send + Sync + 'a;

    /// Driver's specific data associated with suspended PM device state
    type SuspendedPayload<'a>: Send + 'a;
    /// Driver's specific data associated with active PM device states
    type ActivePayload<'a>: Send + 'a;

    /// Runtime resume callback.
    fn runtime_resume<'a>(
        _dev: &'a Self::DeviceType,
        _reg_data: &'a Self::RegistrationData<'a>,
        _payload: Self::SuspendedPayload<'a>,
    ) -> PMCallbackResult<Self::ActivePayload<'a>, Self::SuspendedPayload<'a>> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Runtime suspend callback.
    fn runtime_suspend<'a>(
        _dev: &'a Self::DeviceType,
        _reg_data: &'a Self::RegistrationData<'a>,
        _payload: Self::ActivePayload<'a>,
    ) -> PMCallbackResult<Self::SuspendedPayload<'a>, Self::ActivePayload<'a>> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// RAII guard for ongoing runtime PM payload transition.
///
/// For most of the callbacks this guard is not necessarily needed as
/// the callbacks themselves are being serialized by the runtime PM C code.
/// Still, some like runtime_idle are exempt from that.
#[cfg_attr(not(CONFIG_PM), allow(unused))]
struct PayloadGuard<'a> {
    busy: &'a AtomicFlag,
}

impl Drop for PayloadGuard<'_> {
    fn drop(&mut self) {
        self.busy.store(false, ordering::Release);
    }
}

/// Payload associated with the given runtime power state of a device.
#[cfg_attr(not(CONFIG_PM), allow(unused))]
pub enum RuntimeStatePayload<Active, Suspended> {
    /// Payload associated with the runtime active PM state of a device
    Active(Active),
    /// Payload associated with the runtime suspended PM state of a device
    Suspended(Suspended),
}

#[cfg_attr(not(CONFIG_PM), allow(unused))]
struct Payload<'a, T:PMOps> {
    in_flight: AtomicFlag,
    inner: UnsafeCell<ManuallyDrop<RuntimeStatePayload<T::ActivePayload<'a>, T::SuspendedPayload<'a>>>>,
}

#[cfg_attr(not(CONFIG_PM), allow(unused))]
impl<'a, T:PMOps> Payload<'a, T> {
    /// Attempts to acquire exclusive access to the runtime PM payload.
    ///
    /// Returns `EBUSY` if another runtime PM callback is already transitioning the
    /// payload.
    fn acquire(&self) -> Result<PayloadGuard<'_>> {
        self.in_flight
            .cmpxchg(false, true, ordering::Acquire)
            .map_err(|_| EBUSY)?;
        Ok(PayloadGuard {
            busy: &self.in_flight,
        })
    }
}

impl<T:PMOps> Drop for Payload<'_, T> {
    fn drop(&mut self) {
        // SAFETY: teardown drained PM callbacks and the slot holds a `P`
        unsafe {
            ManuallyDrop::drop(self.inner.get_mut());
        }
    }
}

// SAFETY: Although Payload's `inner` is an `UnsafeCell`, it is only accessed
// after `in_flight` has been acquired. The AtomicFlag flag serializes all mutable
// access to the payload, and `PayloadGuard` clears the flag when the access ends.
unsafe impl<T:PMOps> Sync for Payload<'_, T> {}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
enum PMContextState {
    /// The PM registration is alive, but runtime PM has not been enabled.
    Disabled = 0,
    /// Enabling runtime PM has been initiated but has not completed just yet
    Enabling = 1,
    /// The runtime PM has been enabled. Intended to be balanced by
    /// `disable()` or `Registration::drop()`
    Enabled = 2,
    /// Disabling runtime PM has been initiated but has not completed just yet
    Disabling = 3,
    /// The owning `Registration` is being dropped or has been dropped.
    /// Stale cloned `PMContext`s may still exist, but once this state is
    /// visible, runtime PM cannot be re-enabled.
    Closing = 4,
}

// SAFETY: `PMContextState` has the same size and alignment as `i32`, and all
// values written through `Atomic<PMContextState>` are valid `PMContextState`
// discriminants.
unsafe impl AtomicType for PMContextState {
    type Repr = i32;
}

#[pin_data]
struct PMContextInner<'a, T: PMOps> {
    dev: &'a Device<Bound>,
    /// State owned by the runtime PM registration
    state: Atomic<PMContextState>,
    /// A synchronization point for all pending callbacks to finish.
    #[pin]
    guard: Srcu,
    /// Optional driver-selected runtime PM request PMProfiles.
    ///
    /// Set of runtime PM predefined PMProfiles that can be used by the driver
    /// when requesting a PM transition. This might be useful when a driver
    /// has several different PM usage patterns.
    /// See [PMProfile] for more details.
    profiles: KVec<PMProfile>,
    /// Set of PM config options applied for associated device.
    configs: KVec<PMConfig>,
    _marker: PhantomData<fn() -> T>,
}

/// Runtime PM context tied to a device.
///
/// Needs to be dropped from sleepable context.
#[pin_data]
pub struct PMContext<'r, 'a, T: PMOps> {
    inner: &'r PMContextInner<'a, T>,
}

impl<'r, 'a, T: PMOps> PMContext<'r, 'a, T> {
    /// Driver-provided runtime PM operations.
    ///
    /// A driver implements this trait to handle runtime PM
    /// transitions for its device type.
    ///
    /// Each callback receives the device, its registration data
    /// and the current payload.
    /// On success, it returns the payload to keep for the next
    /// transition. On failure, it returns the payload together
    /// with the error so the previous, or otherwise sane state
    /// can be preserved.
    #[cfg(CONFIG_PM)]
    pub(crate) const PM_OPS: bindings::dev_pm_ops = bindings::dev_pm_ops {
        runtime_resume: if T::HAS_RUNTIME_RESUME {
            Some(runtime_resume_callback::<T>)
        } else {
            None
        },
        runtime_suspend: if T::HAS_RUNTIME_SUSPEND {
            Some(runtime_suspend_callback::<T>)
        } else {
            None
        },
        ..PMOPS_NONE
    };

    #[cfg(not(CONFIG_PM))]
    pub(crate) const PM_OPS: bindings::dev_pm_ops = PMOPS_NONE;

    #[inline]
    fn state_transition(&self, expected: PMContextState, new: PMContextState) -> Result
    {
        self.inner.state.cmpxchg(expected, new, ordering::Full)
        .map(|_| ())
        .map_err(|actual| match (expected, actual) {
            (_, PMContextState::Closing) => ENODEV,
            (PMContextState::Disabled, PMContextState::Disabling)
            | (PMContextState::Enabled, PMContextState::Enabling) => EBUSY,
            _ => EINVAL,
        })
   }

    /// Enable runtime PM
    pub fn enable(&self, state: RuntimePMState) -> Result {
        let _transition_guard = self.inner.guard.read_lock();
        self.state_transition(PMContextState::Disabled, PMContextState::Enabling)?;
        Self::apply_config(self.inner.dev, &self.inner.configs);

        match state {
                RuntimePMState::Active => Request::mark_active(self.inner.dev),
                RuntimePMState::Suspended => Request::mark_suspended(self.inner.dev),
        }
        .inspect_err(|_| {
            Self::reset_config(self.inner.dev, &self.inner.configs);
            let _ = self.state_transition(
                    PMContextState::Enabling,
                    PMContextState::Disabled,
                );
        })?;

        Request::runtime_enable(self.inner.dev);
        self.state_transition(
                PMContextState::Enabling,
                PMContextState::Enabled,
        ).inspect_err(|e| {
            if *e == ENODEV {
                Request::runtime_disable(self.inner.dev);
                Self::reset_config(self.inner.dev, &self.inner.configs);
            }
        })?;
        Ok(())
    }

    /// Disable runtime PM
    pub fn disable(&self) -> Result {
        let _transition_guard = self.inner.guard.read_lock();
        self.state_transition(
                PMContextState::Enabled,
                PMContextState::Disabling,
        )?;

        Request::runtime_disable(self.inner.dev);
        Self::reset_config(self.inner.dev, &self.inner.configs);

        let _  = self.state_transition(
             PMContextState::Disabling,
             PMContextState::Disabled
        );
        Ok(())
    }

    /// Returns whether the runtime PM state is active.
    #[inline]
    pub fn active(&self) -> bool {
        Request::active(self.inner.dev)
    }

    /// Returns whether the runtime PM state is suspended.
    #[inline]
    pub fn suspended(&self) -> bool {
        Request::suspended(self.inner.dev)
    }

    /// Creates a `ResumeScope` for the given PMProfile.
    /// Note that for ASYNC/NOWAIT request this does not guarantee
    /// the device has been resumed at the time this function returns.
    #[inline]
    pub fn resume(&self, profile: PMProfile) -> Result<ResumeScope<'r>> {
        ResumeScope::new(self.inner.dev, profile.0)
    }

    /// Creates an `AwakeScope` for the given PMProfile.
    /// Note that for ASYNC/NOWAIT request this does not guarantee
    /// the device has been resumed at the time this function returns.
    #[inline]
    pub fn get(&self, profile: PMProfile) -> Result<AwakeScope<'r>> {
        AwakeScope::new(self.inner.dev, profile.0 | ModeFlag::Acquire)
    }

    /// Creates a `RetainScope` for this device.
    pub fn hold(&self) -> Result<RetainScope<'r>> {
        RetainScope::new(self.inner.dev)
    }

    /// Creates a `RetainScope` for an active device.
    pub fn try_hold_active(&self) -> Result<RetainScope<'r>> {
        RetainScope::try_new(self.inner.dev)
    }

    /// Runs a closure while holding a `ResumeScope`.
    pub fn with_resume<R>(&self, profile: PMProfile, f: impl FnOnce() -> Result<R>) -> Result<R> {
        if profile.0.contains_any(ModeFlag::Async| ModeFlag::Nowait) {
            return Err(EINVAL);
        }
        let _scope = self.resume(profile)?;
        f()
    }
    /// Runs a closure while holding an `AwakeScope`.
    pub fn with_get<R>(&self, profile: PMProfile, f: impl FnOnce() -> Result<R>) -> Result<R> {
        if profile.0.contains_any(ModeFlag::Async | ModeFlag::Nowait) {
            return Err(EINVAL);
        }
        let _scope = self.get(profile)?;
        f()
    }

    /// Runs a closure while holding a `RetainScope`.
    pub fn with_hold<R>(&self, f: impl FnOnce() -> Result<R>) -> Result<R> {
        let _scope = self.hold()?;
        f()
    }

    /// Applies runtime PM configuration options.
    ///
    /// Options are applied in the order provided. The currently supported
    /// options do not report per-option failures.
    fn apply_config(dev: &Device<Bound>, opts: &[PMConfig]) {
        for opt in opts {
            match opt {
                // SAFETY: The `Device<Bound>` reference guarantees that `dev.as_raw()`
                // yields a valid pointer to `struct device` for the duration of this call.
                PMConfig::IgnoreChildren(v) => unsafe {
                    bindings::pm_suspend_ignore_children(dev.as_raw(), *v)
                },
                // SAFETY: The `Device<Bound>` reference guarantees that `dev.as_raw()`
                // yields a valid pointer to `struct device` for the duration of this call.
                PMConfig::NoCallbacks => unsafe { bindings::pm_runtime_no_callbacks(dev.as_raw()) },
                // SAFETY: The `Device<Bound>` reference guarantees that `dev.as_raw()`
                // yields a valid pointer to `struct device` for the duration of this call.
                PMConfig::IrqSafe => unsafe {
                    // Do not attempt to call that more than once
                    if !bindings::pm_runtime_is_irq_safe(dev.as_raw()) {
                        bindings::pm_runtime_irq_safe(dev.as_raw());
                    }
                },
                // SAFETY: The `Device<Bound>` reference guarantees that `dev.as_raw()`
                // yields a valid pointer to `struct device` for the duration of this call.
                PMConfig::AutoSuspend(v) => unsafe {
                    bindings::__pm_runtime_use_autosuspend(dev.as_raw(), *v);
                },
                // SAFETY: The `Device<Bound>` reference guarantees that `dev.as_raw()`
                // yields a valid pointer to `struct device` for the duration of this call.
                PMConfig::AutoSuspendDelay(v) => unsafe {
                    bindings::pm_runtime_set_autosuspend_delay(dev.as_raw(), *v);
                    bindings::__pm_runtime_use_autosuspend(dev.as_raw(), true);
                },
            }
        }
    }

    fn reset_config(dev: &Device<Bound>, opts: &[PMConfig]) {
        if opts.iter().any(|opt| {
            matches!(opt, PMConfig::AutoSuspend(_) | PMConfig::AutoSuspendDelay(_))
        }) {
             // SAFETY: `dev` remains bound and valid for this call
             unsafe { bindings::__pm_runtime_use_autosuspend(dev.as_raw(), false) };
        }

        if opts.iter().any(|opt| {
            matches!(opt, PMConfig::IgnoreChildren(_))
        }) {
             // SAFETY: `dev` remains bound and valid for this call
             unsafe { bindings::pm_suspend_ignore_children(dev.as_raw(), false) };
        }
    }

    /// Get a borrowed reference to PM profiles associated with the PM context
    pub fn profiles(&self) -> &[PMProfile] {
        &self.inner.profiles
    }

    /// Get a borrowed reference to PM configs associated with the PM context
    pub fn configs(&self) -> &[PMConfig] {
        &self.inner.configs
    }
}

/// Runtime PM request profile.
#[derive(Copy, Clone)]
pub struct PMProfile(Mode);

impl PMProfile {
    /// Creates a PMProfile with default SYNC mode set.
    pub const fn new() -> Self {
        Self(Mode::empty())
    }
    /// Enables async PM operations for this PMProfile.
    pub const fn r#async(self) -> Self {
        Self(Mode(self.0 .0 | ModeFlag::Async as u32))
    }
    /// Use autosuspend
    pub const fn auto(self) -> Self {
        Self(Mode(self.0 .0 | ModeFlag::Auto as u32))
    }
    /// Requests idle handling for this PMProfile.
    pub const fn idle(self) -> Self {
        Self(Mode(self.0 .0 | ModeFlag::Idle as u32))
    }
    /// Do not wait for concurrent requests to finish.
    pub const fn nowait(self) -> Self {
        Self(Mode(self.0 .0 | ModeFlag::Nowait as u32))
    }
}

impl Default for PMProfile {
    fn default() -> Self {
        Self::new()
    }
}

/// Configuration knobs for runtime PM.
pub enum PMConfig {
    /// Ignore child devices when suspending.
    IgnoreChildren(bool),
    /// Disable runtime PM callbacks.
    NoCallbacks,
    /// Mark device as IRQ-safe for runtime PM.
    /// Runtime PM callbacks run with local interrupts
    /// disabled and must not sleep
    IrqSafe,
    /// Enable or disable autosuspend.
    AutoSuspend(bool),
    /// Set autosuspend delay (milliseconds).
    AutoSuspendDelay(i32),
}

/// Runtime PM data stored within the `struct device_private` during
/// runtime PM registration.
///
#[repr(C)]
#[pin_data]
struct RegistrationData<'a, T: PMOps> {
    payload: Payload<'a, T>,
    #[cfg_attr(not(CONFIG_PM), allow(dead_code))]
    #[pin]
    data: T::RegistrationData<'a>,
    _marker: PhantomData<&'a mut T>,
}

/// Runtime PM registration for a device.
///
/// A `Registration` installs the `RegistrationData` alonw with the runtime
/// payload, used by the generated [`PMOps`] callbacks and owns
/// the corresponding teardown.
///
/// Dropping the registration disables runtime PM, waits for in-flight runtime PM
/// callbacks to complete, and then removes the stored registration data.
#[pin_data(PinnedDrop)]
pub struct Registration<'a, T>
where
    T:PMOps,
{
    ctx: PMContext<'inner, 'a,  T>,
    #[pin]
    inner: PMContextInner<'a, T>,
    #[pin]
    data: RegistrationData<'a, T>,
}

impl<'a, T: PMOps> Registration<'a, T> {
    /// Creates a runtime PM registration for `dev`.
    ///
    /// The provided profiles and configuration are stored in the associated
    /// [`PMContext`]. The `initial_payload` is expected to reflect current
    /// power state of the device.
    ///
    /// The device must use the callback represented by `ops`, generated
    /// for the same bus adapter and driver pair `(D, T)`.
    ///
    /// # Safety
    ///
    /// The returned [`Registration`] object cannot be mem::forgotten,
    /// or it's [`Drop`] implementation cannot be skipped in any way.
    ///
    pub unsafe fn new<E>(
        dev: &'a Device<Core<'_>>,
        ops: &'static Ops<T>,
        profiles: Option<KVec<PMProfile>>,
        configs: Option<KVec<PMConfig>>,
        data: impl PinInit<<T as PMOps>::RegistrationData<'a>, E> + 'a,
        initial_payload: RuntimeStatePayload<T::ActivePayload<'a>, T::SuspendedPayload<'a>>,
    ) -> impl PinInit<Self, Error> + 'a
    where
        Error: From<E>,
    {
         let dev: &'a Device<Bound> = dev;

       try_pin_init!(Self {
           _: {
               // SAFETY: For the duration of this call, `dev` is a valid `Device<Core>`,
               // and so is its raw `struct device` pointer.
               let pm =  unsafe { (*(*dev.as_raw()).driver).pm };
               if  pm != ops.as_raw() && (cfg!(CONFIG_PM) || !pm.is_null()) {
                   return Err(EINVAL);
               }
               // SAFETY: Construction has exclusive access to this slot.
               if !unsafe {
                    (*(*dev.as_raw()).p).rust.pm_registration_data
               }.is_null() {
                   return Err(EBUSY);
               }
           },
           inner <- try_pin_init!(PMContextInner {
               dev,
               state: Atomic::new(PMContextState::Disabled),
               guard <- new_srcu!("rpm"),
               profiles: profiles.unwrap_or_default(),
               configs: configs.unwrap_or_default(),
               _marker: PhantomData,
           }),
           ctx : PMContext { inner: inner.get_ref(), },
           data <- try_pin_init!(RegistrationData {
                data <- data,
                payload: Payload {in_flight: AtomicFlag::new(false), inner: UnsafeCell::new(ManuallyDrop::new(initial_payload))},
                _marker: PhantomData
            }? E),
            _: {
                let ptr: NonNull<RegistrationData<'a, T>> =
                    NonNull::from(data.as_ref().get_ref());
                // SAFETY: The device is not yet registered, and neither is
                // the runtime pm beingenabled - as such, there is no
                // cuncurency here
                unsafe {
                    (*(*dev.as_raw()).p).rust.pm_registration_data =
                        ptr.cast::<core::ffi::c_void>().as_ptr();
                };

            }
       })

   }
    /// Returns the runtime PM context associated with this registration.
    pub fn get_ctx(&self) -> PMContext<'_, 'a,  T> {
        PMContext{ inner: &self.inner }
    }
}

#[pinned_drop]
impl<'a, T: PMOps> PinnedDrop for Registration<'a, T> {
    fn drop(self: Pin<&mut Self>) {
        // `self.inner.dev` is the device this registration was
        // created for. Runtime PM is disabled first, and `pm_runtime_barrier`
        // waits for pending runtime PM work/callbacks before the callback data
        // is removed below.
        // Furthermore, prevent any stale `PMContext`s from re-enabling runtime
        // PM after teardown starts.
        let prev = self.inner.state.xchg(PMContextState::Closing, ordering::Full);

        self.inner.guard.synchronize();

        if prev == PMContextState::Enabled {
            Request::runtime_disable(self.inner.dev);
            PMContext::<T>::reset_config(self.inner.dev, &self.inner.configs);
        }

        Request::barrier(self.inner.dev);

        // SAFETY: The pointer  was stored by `Registration::new`.
        // Runtime PM has been disabled and drained above, so generated
        // callbacks can no longer borrow this data.
        unsafe {
            (*(*self.inner.dev.as_raw()).p).rust.pm_registration_data = core::ptr::null_mut();
        }
    }
}
