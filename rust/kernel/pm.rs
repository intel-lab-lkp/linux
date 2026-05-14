// SPDX-License-Identifier: GPL-2.0

//! Rust Runtime Power Management abstraction.
//!
//! C header: [`include/linux/pm_runtime.h`](srctree/include/linux/pm_runtime.h)

use crate::{
    bindings, device,
    error::{to_result, VTABLE_DEFAULT_ERROR},
    macros::paste,
    prelude::*,
    sync::aref::ARef,
    sync::atomic::{
        ordering::{Acquire, Full, Release},
        Atomic,
    },
    sync::Arc,
    sync::{new_mutex, Mutex},
};

use core::marker::PhantomData;

/// Runtime Power Management modes that determine how a particular PM
/// transition is to be carried out.
/// Corresponds to C Runtime PM flag argument bits:
/// - `RPM_ASYNC`
/// - `RPM_NOWAIT`
/// - `RPM_GET_PUT`
/// - `RPM_AUTO`
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct Mode(u32);

impl Mode {
    /// Synchronous PM operations - default.
    #[allow(unused)]
    const SYNC: Mode = Mode(0);
    /// Allow asynchronous PM operations.
    const ASYNC: Mode = Mode(bindings::RPM_ASYNC);
    /// Do not wait for any pending rquests to finish.
    const NOWAIT: Mode = Mode(bindings::RPM_NOWAIT);
    /// Acquire a runtime-PM usage reference.
    const ACQUIRE: Mode = Mode(bindings::RPM_GET_PUT);
    /// Use autosuspend.
    const AUTO: Mode = Mode(bindings::RPM_AUTO);
    /// Additional mode for devices supporting idle states.
    const IDLE: Mode = Mode(1 << 16);

    const fn join(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }

    fn contains(self, other: Self) -> bool {
        (self.0 & other.0) != 0
    }
}

impl core::ops::BitOr for Mode {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Mode {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::Not for Mode {
    type Output = Self;
    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

impl From<Mode> for core::ffi::c_int {
    #[inline]
    fn from(mode: Mode) -> core::ffi::c_int {
        mode.0 as core::ffi::c_int
    }
}

/// Utility macro for combining multiple request modes
macro_rules! mode {
    ($mode:expr $(, $args:expr)+ $(,)?) => {{
        let mut new_mode = $mode;
        $( new_mode = new_mode.join($args); )+
        new_mode
    }};
}

/// Runtime power transition scope.
pub struct Scope<Tag> {
    dev: ARef<device::Device>,
    mode: Mode,
    _tag: PhantomData<Tag>,
}

/// Device resumed without incrementing the device's usage count
pub struct Resume;
/// Device resumed with the devices'a usage count being incremented
pub struct Awake;
/// Device with increased usage reference
pub struct Retain;

/// Resumes the device without acquiring the usage reference.
/// Note: This does not guarantee the device will be kept active
/// for the lifetime of the scope due to potentil pending suspend
/// requests.
///
/// On drop:
/// - If `Mode::IDLE`, calls `__pm_runtime_idle()`:
///   triggers idle notification before attempting to suspend
/// - If `Mode::AUTO`, marks last busy then calls `__pm_runtime_suspend()`.
/// - Otherwise calls `__pm_runtime_suspend()`.
///
/// The guard must be dropped from a context matching the requested transition
/// mode: sync vs async.
#[must_use = "dropping this guard issues the matching runtime PM release request"]
pub struct ResumeScope(Scope<Resume>);

/// Acquires a runtime-PM usage reference and keeps the device powered.
///
/// Requires `Mode::ACQUIRE`. Drop behavior matches `ResumeScope`.
/// The guard must be dropped from a context matching the requested transition
/// mode: sync vs async.
#[must_use = "dropping this guard releases its runtime PM hold"]
pub struct AwakeScope(Scope<Awake>);

/// Prevents the device from getting suspended by holding the usage reference
/// count.
///
/// On drop, calls `pm_runtime_put_noidle()`.
#[must_use = "dropping this guard releases its runtime PM hold"]
pub struct RetainScope(Scope<Retain>);

impl ResumeScope {
    fn new(dev: ARef<device::Device>, mode: Mode) -> Result<Self> {
        if mode.contains(Mode::ACQUIRE) {
            // Mode::ACQUIRE is intended to be used with Awake scope
            // Avoid mixing the modes.
            return Err(EINVAL);
        }

        // Mode::IDLE is internal so strip it of before passing further
        Request::resume(&dev, mode & !Mode::IDLE).map(|()| {
            Self(Scope::<Resume> {
                dev: dev.clone(),
                mode,
                _tag: PhantomData,
            })
        })
    }
}

impl Drop for ResumeScope {
    fn drop(&mut self) {
        let scope_mode = self.0.mode & !Mode::IDLE;

        match self.0.mode {
            mode if mode.contains(Mode::IDLE) => {
                Request::idle(&self.0.dev, scope_mode & Mode::ASYNC);
            }
            mode if mode.contains(Mode::AUTO) => {
                Request::mark_last_busy(&self.0.dev);
                let _ = Request::suspend(&self.0.dev, scope_mode).inspect_err(|_| {
                    dev_err!(self.0.dev.as_ref(), "Failed to suspend device\n");
                });
            }
            _ => {
                let _ = Request::suspend(&self.0.dev, scope_mode).inspect_err(|_| {
                    dev_err!(self.0.dev.as_ref(), "Failed to suspend device\n");
                });
            }
        }
    }
}

impl AwakeScope {
    fn new(dev: ARef<device::Device>, mode: Mode) -> Result<Self> {
        if !mode.contains(Mode::ACQUIRE) {
            return Err(EINVAL);
        }
        // Mode::IDLE is internal so strip it of before passing further
        Request::resume(&dev, mode & !Mode::IDLE)
            .inspect_err(|_| Request::put_noidle(&dev))
            .map(|()| {
                Self(Scope::<Awake> {
                    dev: dev.clone(),
                    mode,
                    _tag: PhantomData,
                })
            })
    }
}

impl Drop for AwakeScope {
    fn drop(&mut self) {
        let scope_mode = self.0.mode & !Mode::IDLE;
        match self.0.mode {
            mode if mode.contains(Mode::IDLE) => {
                Request::idle(&self.0.dev, scope_mode);
            }
            mode if mode.contains(Mode::AUTO) => {
                Request::mark_last_busy(&self.0.dev);
                let _ = Request::suspend(&self.0.dev, scope_mode).inspect_err(|_| {
                    dev_err!(self.0.dev.as_ref(), "Failed to suspend device\n");
                });
            }
            _ => {
                let _ = Request::suspend(&self.0.dev, scope_mode).inspect_err(|_| {
                    dev_err!(self.0.dev.as_ref(), "Failed to suspend device\n");
                });
            }
        }
    }
}

impl RetainScope {
    fn new(dev: ARef<device::Device>) -> Result<Self> {
        Request::get_noresume(&dev);
        Ok(Self(Scope::<Retain> {
            dev,
            mode: Mode(0),
            _tag: PhantomData,
        }))
    }
}

impl Drop for RetainScope {
    fn drop(&mut self) {
        Request::put_noidle(&self.0.dev);
    }
}

struct Request;

#[cfg(CONFIG_PM)]
impl Request {
    #[inline(always)]
    fn resume(dev: &ARef<device::Device>, mode: Mode) -> Result {
        // SAFETY: `dev` is a valid `&ARef<Device>`, so the underlying `Device` is
        // guaranteed to be alive and `as_raw()` yields a valid pointer for the
        // duration of this call.
        to_result(unsafe { bindings::__pm_runtime_resume(dev.as_raw(), mode.into()) })
    }

    #[inline(always)]
    fn idle(dev: &ARef<device::Device>, mode: Mode) {
        // SAFETY: `dev` is a valid `&ARef<Device>`, so the underlying `Device` is
        // guaranteed to be alive and `as_raw()` yields a valid pointer for the
        // duration of this call.
            unsafe {
            bindings::__pm_runtime_idle(dev.as_raw(), mode.into());
        }
    }

    #[inline(always)]
    fn mark_last_busy(dev: &ARef<device::Device>) {
        // SAFETY: `dev` is a valid `&ARef<Device>`, so the underlying `Device` is
        // guaranteed to be alive and `as_raw()` yields a valid pointer for the
        // duration of this call.
        unsafe {
            bindings::pm_runtime_mark_last_busy(dev.as_raw());
        }
    }

    #[inline(always)]
    fn suspend(dev: &ARef<device::Device>, mode: Mode) -> Result {
        // SAFETY: `dev` is a valid `&ARef<Device>`, so the underlying `Device` is
        // guaranteed to be alive and `as_raw()` yields a valid pointer for the
        // duration of this call.
        to_result(unsafe { bindings::__pm_runtime_suspend(dev.as_raw(), mode.into()) })
    }

    #[inline(always)]
    fn runtime_enable(dev: &ARef<device::Device>) -> Result {
        // SAFETY: `dev` is a valid `&ARef<Device>`, so the underlying `Device` is
        // guaranteed to be alive and `as_raw()` yields a valid pointer for the
        // duration of this call.
        to_result(unsafe { bindings::devm_pm_runtime_enable(dev.as_raw()) })
    }
}

#[cfg(not(CONFIG_PM))]
impl Request {
    #[inline(always)]
    fn resume(_dev: &ARef<device::Device>, _mode: Mode) -> Result {
        Err(Error::from_errno(-(bindings::ENOSYS as i32)))
    }

    #[inline(always)]
    fn idle(_dev: &ARef<device::Device>, _mode: Mode) {}

    #[inline(always)]
    fn mark_last_busy(_dev: &ARef<device::Device>) {}

    #[inline(always)]
    fn suspend(_dev: &ARef<device::Device>, _mode: Mode) -> Result {
        Err(Error::from_errno(-(bindings::ENOSYS as i32)))
    }

    #[inline(always)]
    fn runtime_enable(_dev: &ARef<device::Device>) -> Result {
        Ok(())
    }
}

impl Request {
    #[inline(always)]
    fn get_noresume(dev: &ARef<device::Device>) {
        // SAFETY: `dev` is a valid `&ARef<Device>`, so the underlying `Device` is
        // guaranteed to be alive and `as_raw()` yields a valid pointer for the
        // duration of this call.
        unsafe { bindings::pm_runtime_get_noresume(dev.as_raw()) };
    }

    #[inline(always)]
    fn put_noidle(dev: &ARef<device::Device>) {
        // SAFETY: `dev` is a valid `&ARef<Device>`, so the underlying `Device` is
        // guaranteed to be alive and `as_raw()` yields a valid pointer for the
        // duration of this call.
        unsafe { bindings::pm_runtime_put_noidle(dev.as_raw()) };
    }
}

macro_rules! runtime_state {
    (
        $(#[$enum_meta:meta])*
        $name:ident : $repr:ty {
            $(
                $(#[$variant_meta:meta])*
                $v:ident
            ),+$(,)?
        }
    ) => {
        #[repr($repr)]
        #[derive(Copy, Clone, Debug, Eq, PartialEq)]
        $(#[$enum_meta])*
        pub enum $name {
            $(
                $(#[$variant_meta])*
                $v
            ),+
        }

        impl From<$name> for $repr {
            fn from(v: $name) -> Self { v as $repr }
        }

        impl TryFrom<$repr> for $name {
            type Error = crate::error::Error;

            fn try_from(v: $repr) -> Result<Self> {
                match v {
                    $(x if x == $name::$v as $repr => Ok($name::$v), )+
                    _ => Err(EINVAL),
                }
            }
        }
    }
}

runtime_state!(
    ///  Runtime PM state tracked by [`PMContext`].
    ///
    /// This state mirrors the transitions expected by the runtime PM
    /// callbacks and is used to validate those independently
    /// from the PM core's internal `RPM_*` state.
    RuntimePMState: u32 {
        /// Device has entered the runtime idle notification path.
        Idle,
        /// Device is runtime active.
        Active,
        /// Runtime suspend callback is in progress.
        Suspending,
        /// Device is runtime suspended.
        Suspended,
        /// Runtime resume callback is in progress.
        Resuming,
        /// Runtime PM state has not been initialized.
        Unknown,
    }
);

/// Generated callbacks require the device to be bound and driver data to be
/// installed.
macro_rules! define_pm_callback {
    // Bare callback with no associate transition nor fallible descriptor
    (@parse_desc $name:ident) => { define_pm_callback!(@default $name); };
    // Callback with associated state transition
    (@parse_desc $name:ident ($from:ident, $pre:ident, $post:ident)) => {
        define_pm_callback!(@default $name, $from, $pre, $post);
    };

    (@default $name:ident $(, $from:ident, $pre:ident, $post:ident)? ) => {
        paste!(
            /// # Safety
            ///
            /// `dev` must be a valid `struct device *` provided by the PM core.
            unsafe extern "C" fn [<$name _callback>]<T:PMOps>(
                dev: *mut bindings::device
            ) -> core::ffi::c_int {

                let dev: &device::Device<device::CoreInternal> =
                     // SAFETY: `dev` is provided by the PM core to a runtime PM callback and
                     // is valid for the duration of the callback.
                    unsafe { device::Device::from_raw(dev) };

                // SAFETY: PM callbacks are only invoked while the device is bound
                // See [`pm_runtime_remove`](srctree/drivers/base/power/runtime.c)
                let bound_dev = unsafe { dev.as_bound() };

                bound_dev.drvdata::<T::DriverDataType>()
                    .and_then(|data| {
                        let pm_ctx = T::get_pmcontext(data.get_ref())?;
                        let pm_dev = match <T as PMOps>::DeviceType::try_from(dev) {
                            Ok(pm_dev) => Ok(pm_dev),
                            Err(_)  => Err(ENODEV),
                        }?;

                        define_pm_callback!(@transition verify, pm_ctx $(, $from, $pre)?)?;
                        match T::$name(pm_dev, pm_ctx.current_payload()) {
                            Ok(new_state) => {
                                pm_ctx.update_payload(new_state);
                                // This should not really fail
                                let _ =
                                    define_pm_callback!(@transition unchecked, pm_ctx $(,$pre, $post)?);
                                Ok(0)
                            },
                            Err((old_state, e)) => {
                                pm_ctx.update_payload(old_state);
                                let _ = define_pm_callback!(@transition unchecked, pm_ctx $(,$pre, $from)?);
                                Err(e)
                            }
                        }
                    }).map_err(|e| e.to_errno()).err().unwrap_or(0)
            }
        );

    };

   (@transition verify, $ctx:ident, $prev:ident, $new:ident) => {
        // Mark the transition to requested state - if viable
        $ctx.record_transition(
            RuntimePMState::$prev,
            RuntimePMState::$new,
        ).map_err(|_| EINVAL)
    };

    (@transition unchecked, $ctx:ident, $prev:ident, $new:ident) => {
        {
            $ctx.set_state(RuntimePMState::$new);
            Result::<(), crate::error::Error>::Ok(())
        }
    };

    (@transition verify, $ctx:ident) => {
        Result::<(), crate::error::Error>::Ok(())
    };

    (@transition unchecked, $ctx:ident) => {
        Result::<(), crate::error::Error>::Ok(())
    };
}

/// SAFETY:
/// bindings::dev_pm_ops is #[repr(C)], implements Default
/// and the struct itself is all nullable function pointers.
/// There is no padding and all zero bit-pattern is valid
///
const PMOPS_NONE: bindings::dev_pm_ops =
    unsafe { core::mem::MaybeUninit::<bindings::dev_pm_ops>::zeroed().assume_init() };

/// Runtime PM callbacks implemented by a driver.
///
/// The generated `dev_pm_ops` callbacks obtain the driver's private data,
/// retrieve the associated `PMContext`, validate the expected PM
/// transition, and then call the corresponding trait method.
macro_rules! define_pm_ops {
    ($($name:ident $( : $desc:tt )? ),+ $(,)?) => {
        $( define_pm_callback!( @parse_desc $name $( $desc )?); )+
        define_pm_ops!(@common $( $name ), +);
    };

    (@common $( $name:ident),+ ) => {
        /// Runtime PM callbacks implemented by a driver
        ///
        /// The PM core will call into the generated C tcallbacks, which in
        /// turn invoke these methods.
        /// Runtime payload ownership is transferred to the callback.
        /// On success, the callback returns the payload to store
        /// for the new PM state. On failure, it returns the payload to restore
        /// together with the error reported to the PM core.
        #[vtable]
        pub trait PMOps: Sized
        {
            /// Type of a bus device
            type DeviceType<'a>:
                TryFrom<&'a device::Device<device::CoreInternal>>
                + AsRef<device::Device>
                where
                    Self: 'a;
            /// Type of data associated with the `DeviceType' drvier.
            type DriverDataType: 'static;
            /// Type allowing access to associated PMContext
            type PMContextRef<'a>:
                core::ops::Deref<Target = PMContext<Self, Active>>
                + 'a
                where
                    Self: 'a;

            /// Type of the data associated with a PM transitions:
            /// owned by the PMContext, though the transition is managed
            /// by the driver.
            type RuntimePayloadType;

            /// Function that needs to be implemented by the driver
            fn get_pmcontext(_: &Self::DriverDataType) -> Result<Self::PMContextRef<'_>>;

            $(
                #[allow(missing_docs)]
                // Callback-specific docs are provided by the generated `dev_pm_ops`
                // contract on `PMOps`.

                fn $name(
                    _dev:  Self::DeviceType<'_>,
                    _data: Option<Arc<Self::RuntimePayloadType>>
                ) -> Result<
                    Option<Arc<Self::RuntimePayloadType>>,
                    (Option<Arc<Self::RuntimePayloadType>>, $crate::error::Error)
                > {
                    build_error!(VTABLE_DEFAULT_ERROR)
                }
            )+
        }
        paste!(
            impl<T:PMOps> PMContext<T> {
                #[allow(missing_docs)]
                pub const PM_OPS: bindings::dev_pm_ops = bindings::dev_pm_ops {
                    $( [<$name>]: if T::[<HAS_ $name:upper>] {
                        Some([<$name _callback>]::<T>)
                    } else {
                        None
                    }, )+
                    ..PMOPS_NONE
                };
            }
        );
    }
}

/// Marker type for a PM context that is not yet enabled.
pub struct Dormant;
/// Marker type for a PM context that is enabled (runtime PM enabled).
/// It does not by itself mean that the device is currently runtime-active;
/// use `active()` or `suspended()` to query the runtime state.
pub struct Active;

#[pin_data]
struct PMRuntimeState<T: PMOps> {
    current_state: Atomic<u32>,
    #[pin]
    data: Mutex<Option<Arc<T::RuntimePayloadType>>>,
}

impl<T: PMOps> PMRuntimeState<T> {
    fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            current_state: Atomic::new(RuntimePMState::Unknown.into()),
            data <- new_mutex!(None)
        })
    }
}

/// Runtime PM context tied to a device.
pub struct PMContext<T: PMOps, State = Dormant> {
    dev: ARef<device::Device>,
    /// Optional driver-selected runtime PM request profiles.
    ///
    /// Set of runtime PM predefined profiles that can be used by the driver
    /// when requesting a PM transition. This might be useful when a driver
    /// has several different PM usage patterns.
    /// See [crate::module::Profile] for more details.
    pub profiles: Option<KVec<Profile>>,
    state: Pin<KBox<PMRuntimeState<T>>>,

    _marker: PhantomData<State>,
}

define_pm_ops!(
        // PM state change : (expected current state, tranition state, final expected state)
        runtime_suspend : (Active, Suspending, Suspended),
        runtime_resume  : (Suspended, Resuming, Active),
);

impl<T: PMOps, State> PMContext<T, State> {
    /// Takes ownership of the stored runtime PM payload.
    #[inline]
    fn current_payload(&self) -> Option<Arc<<T as PMOps>::RuntimePayloadType>> {
        self.state.data.lock().take()
    }

    /// Stores the runtime PM payload for the next transition.
    #[inline]
    fn update_payload(&self, data: Option<Arc<<T as PMOps>::RuntimePayloadType>>) {
        *self.state.data.lock() = data;
    }

    /// Calls `f` with a reference to the currently stored runtime PM payload.
    ///
    /// The payload is driver-defined and represents the data associated with
    /// the current runtime PM state.
    ///
    pub fn with_payload(
        &self,
        f: impl FnOnce(&Option<Arc<<T as PMOps>::RuntimePayloadType>>) -> Result,
    ) -> Result {
        let state = self.state.data.lock().clone();
        f(&state)
    }
}

impl<T: PMOps> PMContext<T, Dormant> {
    /// Creates a dormant PM context for a device.
    ///
    /// `profiles` is an optional driver-defined [`Profile`] preset that are later
    /// selected by the driver when requesting runtime PM transitions.
    #[inline]
    pub fn new(
        dev: &device::Device,
        profiles: Option<KVec<Profile>>,
    ) -> Result<Self> {
        Ok(Self {
            dev: dev.into(),
            profiles,
            state: KBox::pin_init(PMRuntimeState::new(), GFP_KERNEL)?,
            _marker: PhantomData,
        })
    }

    /// Initialize the RPM state.
    /// The caller is responsible for configuring the runtime payload
    /// approprietaly for chosen state.
    /// Intendend to be called prior to enabling full pm runtime,
    /// i.e. during probe when the device's private data is not yet set.
    pub fn init_state(
        &self,
        runtime_state: RuntimePMState,
        data: Option<Arc<T::RuntimePayloadType>>,
    ) -> Result {
        match runtime_state {
            RuntimePMState::Active => {
                // SAFETY: `self.dev` is a valid `&ARef<Device>`, so the underlying `Device` is
                // guaranteed to be alive and `as_raw()` yields a valid pointer for the
                // duration of this call.
                to_result(unsafe { bindings::pm_runtime_set_active(self.dev.as_raw()) })
            }
            RuntimePMState::Suspended => {
                // SAFETY: `self.dev` is a valid `&ARef<Device>`, so the underlying `Device` is
                // guaranteed to be alive and `as_raw()` yields a valid pointer for the
                // duration of this call.
                to_result(unsafe { bindings::pm_runtime_set_suspended(self.dev.as_raw()) })
            }
            _ => return Err(EINVAL),
        }?;

        self.update_payload(data);
        self.state
            .current_state
            .store(runtime_state.into(), Release);
        Ok(())
    }

    /// Enables runtime PM and returns an active PMContext.
    #[inline]
    pub fn enable(self) -> Result<PMContext<T, Active>> {
        Request::runtime_enable(&self.dev)?;

        let PMContext {
            dev,
            profiles,
            state,
            ..
        } = self;

        // In case the PM state has not been initialized upfront
        let _ = state.current_state.cmpxchg(
            RuntimePMState::Unknown.into(),
            RuntimePMState::Suspended.into(),
            Release,
        );
        Ok(PMContext {
            dev,
            profiles,
            state,
            _marker: PhantomData,
        })
    }
}

/// Runtime PM request profile.
///
/// `PMContext` can store a driver-provided set of profiles in
/// [`PMContext::profiles`].
///
/// # Example:
///
/// ```ignore
/// enum PMProfile {
///     RegisterAccess = 0,
///     SubmitJob = 1,
/// }
///
/// let profiles = KVec::from_iter([
///     // Synchronous resume/suspend. Useful for short register accesses where
///     // the caller needs the device powered before continuing.
///     Profile::new(),
///
///     // Resume synchronously, then allow autosuspend when the scope is
///     // dropped.
///     Profile::new().auto(),
///
/// ], GFP_KERNEL)?;
///
/// let pm = PMContext::new(dev, Some(profiles))?;
/// pm.apply_config(
///     &[PMConfig::AutoSuspend(true),PMConfig::AutoSuspendDelay(300)]
/// )?;
///
/// let pm = pm.enable()?;
///
/// let profile = pm.profiles
///     .as_ref()
///     .ok_or(EINVAL)?[PMProfile::SubmitJob as usize];
///
/// pm.with_get(profile, || {
///     submit_job_to_device()?;
///     Ok(())
/// })?;
///
/// ```
pub struct Profile(Mode);

impl Profile {
    /// Creates a profile with default SYNC mode set.
    pub const fn new() -> Self {
        Self(Mode::SYNC)
    }
    /// /Enables async PM operations for this profile.
    pub const fn r#async(self) -> Self {
        Self(mode!(self.0, Mode::ASYNC))
    }
    /// Use autosuspend
    pub const fn auto(self) -> Self {
        Self(mode!(self.0, Mode::AUTO))
    }
    /// Requests idle handling for this profile.
    pub const fn idle(self) -> Self {
        Self(mode!(self.0, Mode::IDLE))
    }
    /// Do not wait for concurrent requests to finish.
    pub const fn nowait(self) -> Self {
        Self(mode!(self.0, Mode::NOWAIT))
    }
}

impl Default for Profile {
     fn default() -> Self {
         Self::new()
     }
}

impl<T: PMOps> PMContext<T, Active> {
    #[inline(always)]
    fn record_transition(
        &self,
        expected_state: RuntimePMState,
        new_state: RuntimePMState,
    ) -> Result {
        self.state
            .current_state
            .cmpxchg(expected_state.into(), new_state.into(), Full)
            .map(|_| ())
            .map_err(|_| EINVAL)
    }

    #[inline(always)]
    fn set_state(&self, new_state: RuntimePMState) {
        self.state.current_state.store(new_state.into(), Release);
    }

    /// Returns whether the runtime PM state is active.
    ///
    /// This reflects the state tracked by [`PMContext`], not a direct query of
    /// the PM core's internal `RPM_*` state.
    #[inline(always)]
    pub fn active(&self) -> bool {
        self.state
            .current_state
            .load(Acquire)
            .try_into()
            .unwrap_or(RuntimePMState::Unknown)
            == RuntimePMState::Active
    }

    /// Returns whether the runtime PM state is suspended.
    ///
    /// This reflects the state tracked by [`PMContext`], not a direct query of
    /// the PM core's internal `RPM_*` state.
    #[inline(always)]
    pub fn suspended(&self) -> bool {
        self.state
            .current_state
            .load(Acquire)
            .try_into()
            .unwrap_or(RuntimePMState::Unknown)
            == RuntimePMState::Suspended
    }

    /// Creates a `ResumeScope` for the given profile.
    #[inline]
    pub fn resume(&self, profile: Profile) -> Result<ResumeScope> {
        ResumeScope::new(self.dev.clone(), profile.0)
    }

    /// Creates an `AwakeScope` for the given profile.
    #[inline]
    pub fn get(&self, profile: Profile) -> Result<AwakeScope> {
        AwakeScope::new(self.dev.clone(), profile.0 | Mode::ACQUIRE)
    }

    /// Creates a `RetainScope` for this device.
    pub fn hold(&self) -> Result<RetainScope> {
        RetainScope::new(self.dev.clone())
    }
    /// Runs a closure while holding a `ResumeScope`.
    pub fn with_resume<R>(&self, profile: Profile, f: impl FnOnce() -> Result<R>) -> Result<R> {
        if profile.0.contains(Mode::ASYNC) {
            return Err(EINVAL);
        }
        let _scope = self.resume(profile)?;
        f()
    }
    /// Runs a closure while holding an `AwakeScope`.
    pub fn with_get<R>(&self, profile: Profile, f: impl FnOnce() -> Result<R>) -> Result<R> {
        if profile.0.contains(Mode::ASYNC) {
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
}

/// Configuration knobs for runtime PM.
pub enum PMConfig {
    /// Ignore child devices when suspending.
    IgnoreChildren(bool),
    /// Disable runtime PM callbacks.
    NoCallbacks,
    /// Mark device as IRQ-safe for runtime PM.
    IrqSafe,
    /// Enable or disable autosuspend.
    AutoSuspend(bool),
    /// Set autosuspend delay (milliseconds).
    AutoSuspendDelay(u32),
}

impl<T: PMOps, State> PMContext<T, State> {
    /// Applies runtime PM configuration options.
    ///
    /// Options are applied in the order provided. The currently supported
    /// options do not report per-option failures.
    ///
    /// This may be called before or after runtime PM is enabled, depending on
    /// the option and the driver's setup sequence.
    pub fn apply_config(&self, opts: &[PMConfig]) -> Result {
        #[cfg(not(CONFIG_PM))]
        let _ = opts;
        #[cfg(CONFIG_PM)]
        for opt in opts {
            match opt {
                // SAFETY: `self.dev` is a valid `&ARef<Device>`, so the underlying `Device` is
                // guaranteed to be alive and `as_raw()` yields a valid pointer for the
                // duration of this call.
                PMConfig::IgnoreChildren(v) => unsafe {
                    bindings::pm_suspend_ignore_children(self.dev.as_raw(), *v)
                },
                // SAFETY: `self.dev` is a valid `&ARef<Device>`, so the underlying `Device` is
                // guaranteed to be alive and `as_raw()` yields a valid pointer for the
                // duration of this call.
                PMConfig::NoCallbacks => unsafe {
                    bindings::pm_runtime_no_callbacks(self.dev.as_raw())
                },
                // SAFETY: `self.dev` is a valid `&ARef<Device>`, so the underlying `Device` is
                // guaranteed to be alive and `as_raw()` yields a valid pointer for the
                // duration of this call.
                PMConfig::IrqSafe => unsafe { bindings::pm_runtime_irq_safe(self.dev.as_raw()) },
                // SAFETY: `self.dev` is a valid `&ARef<Device>`, so the underlying `Device` is
                // guaranteed to be alive and `as_raw()` yields a valid pointer for the
                // duration of this call.
                PMConfig::AutoSuspend(v) => unsafe {
                    bindings::__pm_runtime_use_autosuspend(self.dev.as_raw(), *v);
                },
                // SAFETY: `self.dev` is a valid `&ARef<Device>`, so the underlying `Device` is
                // guaranteed to be alive and `as_raw()` yields a valid pointer for the
                // duration of this call.
                PMConfig::AutoSuspendDelay(v) => unsafe {
                    bindings::pm_runtime_set_autosuspend_delay(self.dev.as_raw(), *v as i32);
                    bindings::__pm_runtime_use_autosuspend(self.dev.as_raw(), true);
                },
            }
        }
        Ok(())
    }
}
