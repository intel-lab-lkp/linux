// SPDX-License-Identifier: GPL-2.0

//! RTC (Real-Time Clock) device support.
//!
//! C headers: [`include/linux/rtc.h`](srctree/include/linux/rtc.h).
//!
//! Reference: <https://www.kernel.org/doc/html/latest/driver-api/rtc.html>
use crate::{
    acpi,
    amba,
    bindings,
    bitmap::Bitmap,
    device::{self},
    driver,
    error::{
        code,
        from_result,
        to_result,
        VTABLE_DEFAULT_ERROR, //
    },
    i2c,
    of,
    platform,
    prelude::*,
    seq_file::SeqFile,
    sync::aref::AlwaysRefCounted,
    types::{
        ForeignOwnable,
        Opaque, //
    },
    ThisModule, //
};

use core::{
    marker::PhantomData,
    ptr::NonNull, //
};

/// RTC time structure.
///
/// Wraps the C `struct rtc_time` from `include/uapi/linux/rtc.h`.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct RtcTime(pub bindings::rtc_time);

impl RtcTime {
    /// Creates a new `RtcTime` from a time64_t value (seconds since 1970-01-01 00:00:00 UTC).
    pub fn from_time64(time: i64) -> Self {
        // SAFETY: `rtc_time` is a C struct with only integer fields, so it's safe to
        // zero-initialize.
        let mut tm = Self(unsafe { core::mem::zeroed() });
        // SAFETY: `rtc_time64_to_tm` is a pure function that only writes to the provided
        // `struct rtc_time` pointer. The pointer is valid because `tm.0` is a valid mutable
        // reference, and the function does not retain any references to it.
        unsafe {
            bindings::rtc_time64_to_tm(time, &mut tm.0);
        }
        tm
    }

    /// Converts this `RtcTime` to a time64_t value (seconds since 1970-01-01 00:00:00 UTC).
    pub fn to_time64(&self) -> i64 {
        // SAFETY: `rtc_tm_to_time64` is a pure function that only reads from the provided
        // `struct rtc_time` pointer. The pointer is valid because `self.0` is a valid reference,
        // and the function does not retain any references to it.
        unsafe { bindings::rtc_tm_to_time64(core::ptr::from_ref(&self.0).cast_mut()) }
    }

    /// Sets this `RtcTime` from a time64_t value (seconds since 1970-01-01 00:00:00 UTC).
    pub fn set_from_time64(&mut self, time: i64) {
        // SAFETY: `rtc_time64_to_tm` is a pure function that only writes to the provided
        // `struct rtc_time` pointer. The pointer is valid because `self.0` is a valid mutable
        // reference, and the function does not retain any references to it.
        unsafe {
            bindings::rtc_time64_to_tm(time, &mut self.0);
        }
    }

    /// Converts a time64_t value to an RTC time structure.
    #[inline]
    pub fn time64_to_tm(time: i64, tm: &mut Self) {
        tm.set_from_time64(time);
    }

    /// Converts an RTC time structure to a time64_t value (seconds since 1970-01-01 00:00:00 UTC).
    #[inline]
    pub fn tm_to_time64(tm: &Self) -> i64 {
        tm.to_time64()
    }

    /// Calculates the day of the year (0-365) from the date components.
    #[inline]
    pub fn year_days(&self) -> i32 {
        // SAFETY: `rtc_year_days` is a pure function that only performs calculations based on
        // the provided parameters. The values should be from valid RTC time structures and
        // non-negative, but the function itself is safe to call with any values.
        unsafe {
            bindings::rtc_year_days(
                self.0.tm_mday as u32,
                self.0.tm_mon as u32,
                self.0.tm_year as u32,
            )
        }
    }

    /// Returns the seconds field (0-59).
    #[inline]
    pub fn tm_sec(&self) -> i32 {
        self.0.tm_sec
    }

    /// Sets the seconds field (0-59).
    #[inline]
    pub fn set_tm_sec(&mut self, sec: i32) {
        self.0.tm_sec = sec;
    }

    /// Returns the minutes field (0-59).
    #[inline]
    pub fn tm_min(&self) -> i32 {
        self.0.tm_min
    }

    /// Sets the minutes field (0-59).
    #[inline]
    pub fn set_tm_min(&mut self, min: i32) {
        self.0.tm_min = min;
    }

    /// Returns the hours field (0-23).
    #[inline]
    pub fn tm_hour(&self) -> i32 {
        self.0.tm_hour
    }

    /// Sets the hours field (0-23).
    #[inline]
    pub fn set_tm_hour(&mut self, hour: i32) {
        self.0.tm_hour = hour;
    }

    /// Returns the day of the month (1-31).
    #[inline]
    pub fn tm_mday(&self) -> i32 {
        self.0.tm_mday
    }

    /// Sets the day of the month (1-31).
    #[inline]
    pub fn set_tm_mday(&mut self, mday: i32) {
        self.0.tm_mday = mday;
    }

    /// Returns the month (0-11, where 0 is January).
    #[inline]
    pub fn tm_mon(&self) -> i32 {
        self.0.tm_mon
    }

    /// Sets the month (0-11, where 0 is January).
    #[inline]
    pub fn set_tm_mon(&mut self, mon: i32) {
        self.0.tm_mon = mon;
    }

    /// Returns the year (years since 1900).
    #[inline]
    pub fn tm_year(&self) -> i32 {
        self.0.tm_year
    }

    /// Sets the year (years since 1900).
    #[inline]
    pub fn set_tm_year(&mut self, year: i32) {
        self.0.tm_year = year;
    }

    /// Returns the day of the week (0-6, where 0 is Sunday).
    #[inline]
    pub fn tm_wday(&self) -> i32 {
        self.0.tm_wday
    }

    /// Sets the day of the week (0-6, where 0 is Sunday).
    #[inline]
    pub fn set_tm_wday(&mut self, wday: i32) {
        self.0.tm_wday = wday;
    }

    /// Returns the day of the year (0-365).
    #[inline]
    pub fn tm_yday(&self) -> i32 {
        self.0.tm_yday
    }

    /// Sets the day of the year (0-365).
    #[inline]
    pub fn set_tm_yday(&mut self, yday: i32) {
        self.0.tm_yday = yday;
    }

    /// Returns the daylight saving time flag.
    #[inline]
    pub fn tm_isdst(&self) -> i32 {
        self.0.tm_isdst
    }

    /// Sets the daylight saving time flag.
    #[inline]
    pub fn set_tm_isdst(&mut self, isdst: i32) {
        self.0.tm_isdst = isdst;
    }
}

impl Default for RtcTime {
    fn default() -> Self {
        // SAFETY: `rtc_time` is a C struct with only integer fields, so it's safe to
        // zero-initialize.
        Self(unsafe { core::mem::zeroed() })
    }
}

/// RTC wake alarm structure.
///
/// Wraps the C `struct rtc_wkalrm` from `include/uapi/linux/rtc.h`.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct RtcWkAlrm(pub bindings::rtc_wkalrm);

impl Default for RtcWkAlrm {
    fn default() -> Self {
        // SAFETY: `rtc_wkalrm` is a C struct with only integer fields and a nested `rtc_time`
        // struct (which also has only integer fields), so it's safe to zero-initialize.
        Self(unsafe { core::mem::zeroed() })
    }
}

impl RtcWkAlrm {
    /// Returns a reference to the alarm time.
    #[inline]
    pub fn get_time(&self) -> &RtcTime {
        // SAFETY: `RtcTime` is `#[repr(transparent)]` over `bindings::rtc_time`, so the memory
        // layout is identical. We can safely reinterpret the reference.
        unsafe { &*(&raw const self.0.time).cast::<RtcTime>() }
    }

    /// Returns a mutable reference to the alarm time.
    #[inline]
    pub fn get_time_mut(&mut self) -> &mut RtcTime {
        // SAFETY: `RtcTime` is `#[repr(transparent)]` over `bindings::rtc_time`, so the memory
        // layout is identical. We can safely reinterpret the reference.
        unsafe { &mut *(&raw mut self.0.time).cast::<RtcTime>() }
    }

    /// Sets the alarm time from a `RtcTime` value.
    #[inline]
    pub fn set_time(&mut self, time: RtcTime) {
        self.0.time = time.0;
    }

    /// Returns the enabled field.
    #[inline]
    pub fn enabled(&self) -> u8 {
        self.0.enabled
    }

    /// Sets the `enabled` field.
    #[inline]
    pub fn set_enabled(&mut self, enabled: u8) {
        self.0.enabled = enabled;
    }

    /// Returns the pending field.
    #[inline]
    pub fn pending(&self) -> u8 {
        self.0.pending
    }

    /// Sets the `pending` field.
    #[inline]
    pub fn set_pending(&mut self, pending: u8) {
        self.0.pending = pending;
    }
}

/// RTC parameter structure.
///
/// Wraps the C `struct rtc_param` from `include/uapi/linux/rtc.h`.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct RtcParam(pub bindings::rtc_param);

impl Default for RtcParam {
    fn default() -> Self {
        // SAFETY: FFI type is valid to be zero-initialized.
        Self(unsafe { core::mem::zeroed() })
    }
}

/// A Rust wrapper for the C `struct rtc_device`.
///
/// This type provides safe access to RTC device operations. The underlying `rtc_device`
/// is managed by the kernel and remains valid for the lifetime of the `RtcDevice`.
///
/// # Invariants
///
/// A [`RtcDevice`] instance holds a pointer to a valid [`struct rtc_device`] that is
/// registered and managed by the kernel.
///
/// # Examples
///
/// ```rust
/// # use kernel::{
/// #     prelude::*,
/// #     rtc::{
/// #         DriverGeneric,
/// #         PlatformBus,
/// #         RtcDevice, //
/// #     }, //
/// # };
/// // Example IdInfo type with range fields
/// struct DeviceInfo {
///     range_min: i64,
///     range_max: u64,
/// }
///
/// // In your DriverGeneric implementation:
/// // fn init_rtcdevice(
/// //     rtc: &RtcDevice,
/// //     _driver: Pin<&Self>,
/// //     id_info: Option<&DeviceInfo>,
/// // ) -> Result {
/// //     // Set the time range for the RTC device based on device variant
/// //     if let Some(info) = id_info {
/// //         rtc.set_range_min(info.range_min);
/// //         rtc.set_range_max(info.range_max);
/// //     } else {
/// //         rtc.set_range_min(0);
/// //         rtc.set_range_max(u64::MAX);
/// //     }
/// //     Ok(())
/// // }
/// ```
///
/// [`struct rtc_device`]: https://docs.kernel.org/driver-api/rtc.html
#[repr(transparent)]
pub struct RtcDevice(Opaque<bindings::rtc_device>);

impl RtcDevice {
    /// Obtain the raw [`struct rtc_device`] pointer.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::rtc_device {
        self.0.get()
    }

    /// Returns a bound reference to the parent [`device::Device`].
    pub fn bound_parent_device(&self) -> &device::Device<device::Bound> {
        // SAFETY: `device()` returns a valid device, and RTC devices always have a parent.
        let parent = unsafe { self.device().parent().unwrap_unchecked() };
        // SAFETY: A bound RTC device always has a bound parent device.
        unsafe { parent.as_bound() }
    }

    /// Returns a reference to the [`device::Device`] that the RTC device is bound to.
    pub fn device(&self) -> &device::Device {
        // SAFETY: `self.as_raw()` is a valid pointer to a `struct rtc_device`, and `dev` is a valid
        // field within it.
        unsafe { device::Device::from_raw(&raw mut (*self.as_raw()).dev) }
    }

    /// Set the minimum time range for the RTC device.
    #[inline]
    pub fn set_range_min(&self, min: i64) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only writing to the `range_min` field.
        unsafe {
            (*self.as_raw()).range_min = min;
        }
    }

    /// Set the maximum time range for the RTC device.
    #[inline]
    pub fn set_range_max(&self, max: u64) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only writing to the `range_max` field.
        unsafe {
            (*self.as_raw()).range_max = max;
        }
    }

    /// Get the minimum time range for the RTC device.
    #[inline]
    pub fn range_min(&self) -> i64 {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only reading the `range_min` field.
        unsafe { (*self.as_raw()).range_min }
    }

    /// Get the maximum time range for the RTC device.
    #[inline]
    pub fn range_max(&self) -> u64 {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only reading the `range_max` field.
        unsafe { (*self.as_raw()).range_max }
    }

    /// Notify the RTC framework that an interrupt has occurred.
    ///
    /// Should be called from interrupt handlers. Schedules work to handle the interrupt
    /// in process context.
    #[inline]
    pub fn update_irq(&self, num: usize, events: usize) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`. The rtc_update_irq function handles NULL/ERR checks internally.
        unsafe {
            bindings::rtc_update_irq(self.as_raw(), num, events);
        }
    }

    /// Clear a feature bit in the RTC device.
    #[inline]
    pub fn clear_feature(&self, feature: u32) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and features is a valid bitmap array with RTC_FEATURE_CNT bits.
        let features_bitmap = unsafe {
            Bitmap::from_raw_mut(
                (*self.as_raw()).features.as_mut_ptr().cast::<usize>(),
                bindings::RTC_FEATURE_CNT as usize,
            )
        };
        features_bitmap.clear_bit(feature as usize);
    }
}

impl<Ctx: device::DeviceContext> AsRef<device::Device<Ctx>> for RtcDevice {
    fn as_ref(&self) -> &device::Device<Ctx> {
        let raw = self.as_raw();
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct rtc_device`.
        let dev = unsafe { &raw mut (*raw).dev };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(dev) }
    }
}

// SAFETY: Instances of `RtcDevice` are always reference-counted via the underlying `device`.
// The `struct rtc_device` contains a `struct device dev` as its first field, and the
// reference counting is managed through `get_device`/`put_device` on the `dev` field.
unsafe impl AlwaysRefCounted for RtcDevice {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        // `self.device()` returns a reference to the `dev` field of `struct rtc_device`.
        unsafe { bindings::get_device(self.device().as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        let rtc: *mut bindings::rtc_device = obj.cast().as_ptr();

        // SAFETY: By the type invariant of `Self`, `rtc` is a pointer to a valid
        // `struct rtc_device`. The `dev` field is the first field of `struct rtc_device`,
        // so we can safely access it.
        let dev = unsafe { &raw mut (*rtc).dev };

        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::put_device(dev) };
    }
}

// SAFETY: `RtcDevice` is reference-counted and can be released from any thread.
unsafe impl Send for RtcDevice {}

// SAFETY: `RtcDevice` can be shared among threads because all immutable methods are
// protected by the synchronization in `struct rtc_device` (via `ops_lock` mutex).
unsafe impl Sync for RtcDevice {}

/// The RTC driver trait.
///
/// RTC drivers are registered as platform drivers, so they use platform device matching
/// (OF and ACPI device IDs).
///
/// # Examples
///
///```
/// # use kernel::{
/// #     acpi,
/// #     bindings,
/// #     c_str,
/// #     device::Core,
/// #     of,
/// #     platform,
/// #     rtc, //
/// # };
/// use kernel::prelude::*;
///
/// struct MyRtcDriver;
///
/// kernel::of_device_table!(
///     OF_TABLE,
///     MODULE_OF_TABLE,
///     <MyRtcDriver as rtc::DriverGeneric<rtc::PlatformBus>>::IdInfo,
///     [
///         (of::DeviceId::new(c_str!("test,rtc")), ()),
///     ]
/// );
///
/// kernel::acpi_device_table!(
///     ACPI_TABLE,
///     MODULE_ACPI_TABLE,
///     <MyRtcDriver as rtc::DriverGeneric<rtc::PlatformBus>>::IdInfo,
///     [
///         (acpi::DeviceId::new(c_str!("RSTRTC00")), ()),
///     ]
/// );
///
/// impl rtc::DriverGeneric<rtc::PlatformBus> for MyRtcDriver {
///     type IdInfo = ();
///
///     fn probe(
///         _pdev: &platform::Device<Core>,
///         _info: Option<&Self::IdInfo>,
///     ) -> impl PinInit<Self, Error> {
///         Err(ENODEV)
///     }
/// }
///
/// impl rtc::PlatformIdInfos for MyRtcDriver {
///     const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
///     const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);
/// }
///```
/// Trait for different bus types that RTC drivers can use.
///
/// This trait abstracts the differences between different bus types (platform, amba, etc.)
/// and provides the necessary types and methods for driver registration.
pub trait Bus: Send + Sync + Sized + 'static {
    /// The device type for this bus.
    type Device;

    /// The driver registration type for this bus (e.g., `platform_driver`, `amba_driver`).
    type RegType: Default;

    /// The raw C device pointer type for probe callbacks.
    type RawDevice;

    /// The probe function signature for this bus type.
    /// Different buses have different probe function signatures:
    /// - Platform: `unsafe extern "C" fn(*mut platform_device) -> i32`
    /// - AMBA: `unsafe extern "C" fn(*mut amba_device, *const amba_id) -> i32`
    type ProbeFn;

    /// The remove function signature for this bus type.
    /// Currently, all buses use the same remove function signature:
    /// `unsafe extern "C" fn(*mut RawDevice)`
    type RemoveFn;

    /// Get device ID info from a device.
    fn id_info<T: DriverGeneric<Self>>(dev: &device::Device) -> Option<&'static T::IdInfo>
    where
        Self: Sized;

    /// Convert raw device pointer to typed device reference.
    ///
    /// The safety requirements are satisfied by the caller, which ensures that `raw` is a valid
    /// pointer to the appropriate device type. The returned reference is valid for the lifetime of
    /// the raw pointer.
    fn from_raw<'a>(raw: *mut Self::RawDevice) -> &'a Self::Device;

    /// Get the underlying device from a bus device.
    fn as_device(dev: &Self::Device) -> &device::Device;

    /// Register the driver.
    ///
    /// This is the unified interface for driver registration. It delegates to
    /// `register_bus_driver` which is implemented by each specific bus type.
    ///
    /// The safety requirements are satisfied by the caller (typically `Adapter::register`),
    /// which is marked as `unsafe` and ensures that `reg` is a valid pointer to the driver
    /// registration structure. The implementation itself only calls non-unsafe functions.
    fn register<T: DriverGeneric<Self> + RtcOperations + 'static>(
        reg: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result
    where
        Self: BusRegistration<T>;

    /// Unregister the driver.
    ///
    /// This is the unified interface for driver unregistration. It delegates to
    /// `unregister_bus_driver` which is implemented by each specific bus type.
    ///
    /// The safety requirements are satisfied by the caller (typically `Adapter::unregister`),
    /// which is marked as `unsafe` and ensures that `reg` is a valid pointer that was previously
    /// registered with `register`. The implementation itself only calls non-unsafe functions.
    fn unregister<T: DriverGeneric<Self> + RtcOperations + 'static>(reg: &Opaque<Self::RegType>)
    where
        Self: BusRegistration<T>;
}

/// Trait for bus-specific driver registration.
///
/// Each bus type (Platform, AMBA, etc.) implements this trait to handle
/// bus-specific registration requirements, including device tables and callbacks.
/// Different bus types may use different device table types:
/// - Platform bus: `of_table` and `acpi_table`
/// - AMBA bus: `id_table`
pub trait BusRegistration<T: DriverGeneric<Self>>: Bus {
    /// Register the bus driver with device tables and callbacks.
    ///
    /// This method is implemented by each specific bus type (PlatformBus, AmbaBus, etc.)
    /// to handle bus-specific registration requirements.
    ///
    /// The safety requirements are satisfied by the caller (`Bus::register`), which is
    /// marked as `unsafe` and ensures that `reg` is a valid pointer to the driver
    /// registration structure, and that `probe_fn` and `remove_fn` are valid callback
    /// functions.
    fn register_bus_driver(
        reg: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
        probe_fn: Self::ProbeFn,
        remove_fn: Self::RemoveFn,
    ) -> Result;

    /// Unregister the bus driver.
    ///
    /// This method is implemented by each specific bus type (PlatformBus, AmbaBus, etc.)
    /// to handle bus-specific unregistration.
    ///
    /// The safety requirements are satisfied by the caller (`Bus::unregister`), which is
    /// marked as `unsafe` and ensures that `reg` is a valid pointer that was previously
    /// registered with `register_bus_driver`.
    fn unregister_bus_driver(reg: &Opaque<Self::RegType>);

    /// Get the probe function for the driver.
    ///
    /// This method is implemented by each bus type to provide the probe function
    /// with the appropriate trait bounds.
    fn get_probe_fn() -> Self::ProbeFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static;

    /// Get the remove function for the driver.
    ///
    /// This method is implemented by each bus type to provide the remove function
    /// with the appropriate trait bounds.
    fn get_remove_fn() -> Self::RemoveFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static;
}

/// Trait for Platform bus driver device tables.
///
/// Platform bus drivers should implement this trait to provide OF and ACPI device tables.
/// This trait requires the driver to also implement `DriverGeneric<PlatformBus>`.
pub trait PlatformIdInfos: DriverGeneric<PlatformBus> {
    /// The table of OF device ids supported by the driver.
    const OF_ID_TABLE: Option<of::IdTable<<Self as DriverGeneric<PlatformBus>>::IdInfo>>;

    /// The table of ACPI device ids supported by the driver.
    const ACPI_ID_TABLE: Option<acpi::IdTable<<Self as DriverGeneric<PlatformBus>>::IdInfo>>;
}

/// Macro to generate `impl Bus` for different bus types.
///
/// This macro reduces code duplication across PlatformBus, AmbaBus, and I2cBus.
macro_rules! impl_bus {
    (
        $bus:ident,
        Device = $device:ty,
        RegType = $reg_type:ty,
        RawDevice = $raw_device:ty,
        ProbeFn = $probe_fn:ty,
        RemoveFn = $remove_fn:ty,
        from_raw_device = $from_raw_device:ty
    ) => {
        impl Bus for $bus {
            type Device = $device;
            type RegType = $reg_type;
            type RawDevice = $raw_device;
            type ProbeFn = $probe_fn;
            type RemoveFn = $remove_fn;

            fn register<T: DriverGeneric<Self> + RtcOperations + 'static>(
                reg: &Opaque<Self::RegType>,
                name: &'static CStr,
                module: &'static ThisModule,
            ) -> Result
            where
                Self: BusRegistration<T>,
            {
                // Get callbacks from Adapter
                // We use BusRegistration::get_probe_fn and get_remove_fn to get the callbacks
                // These methods require the appropriate trait bounds
                // The trait bound is satisfied because BusRegistration<T> requires the
                // appropriate trait
                let probe_fn = <Self as BusRegistration<T>>::get_probe_fn();
                let remove_fn = <Self as BusRegistration<T>>::get_remove_fn();

                // Delegate to bus-specific registration.
                // Each bus type (PlatformBus, AmbaBus, etc.) will extract the appropriate
                // device tables from T in its register_bus_driver implementation.
                Self::register_bus_driver(reg, name, module, probe_fn, remove_fn)
            }

            fn id_info<T: DriverGeneric<Self>>(
                _dev: &device::Device,
            ) -> Option<&'static T::IdInfo> {
                // ID info is extracted in `Adapter::probe_callback` instead.
                None
            }

            fn from_raw<'a>(raw: *mut Self::RawDevice) -> &'a Self::Device {
                // SAFETY: Caller guarantees `raw` is valid.
                // `Device` is `#[repr(transparent)]`, so `CoreInternal` and `Core` have the
                // same layout.
                unsafe { &*raw.cast::<$from_raw_device>() }
            }

            fn as_device(dev: &Self::Device) -> &device::Device {
                dev.as_ref()
            }

            fn unregister<T: DriverGeneric<Self> + RtcOperations + 'static>(
                reg: &Opaque<Self::RegType>,
            ) where
                Self: BusRegistration<T>,
            {
                // Delegate to bus-specific unregistration
                Self::unregister_bus_driver(reg)
            }
        }
    };
}

/// Platform bus implementation.
pub struct PlatformBus;

impl_bus!(
    PlatformBus,
    Device = platform::Device<device::Core>,
    RegType = bindings::platform_driver,
    RawDevice = bindings::platform_device,
    ProbeFn = unsafe extern "C" fn(*mut bindings::platform_device) -> c_int,
    RemoveFn = unsafe extern "C" fn(*mut bindings::platform_device),
    from_raw_device = platform::Device<device::Core>
);

impl<T: DriverGeneric<PlatformBus> + PlatformIdInfos> BusRegistration<T> for PlatformBus {
    fn get_probe_fn() -> Self::ProbeFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static,
    {
        <Adapter<T, Self>>::probe_callback
    }

    fn get_remove_fn() -> Self::RemoveFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static,
    {
        <Adapter<T, Self>>::remove_callback
    }

    fn register_bus_driver(
        reg: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
        probe_fn: Self::ProbeFn,
        remove_fn: Self::RemoveFn,
    ) -> Result {
        // Get device tables from the driver (platform-specific: OF and ACPI)
        let of_table = match <T as PlatformIdInfos>::OF_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        let acpi_table = match <T as PlatformIdInfos>::ACPI_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        // SAFETY: It's safe to set the fields of `struct platform_driver` on initialization.
        unsafe {
            (*reg.get()).driver.name = name.as_char_ptr();
            (*reg.get()).probe = Some(probe_fn);
            (*reg.get()).remove = Some(remove_fn);
            (*reg.get()).driver.of_match_table = of_table;
            (*reg.get()).driver.acpi_match_table = acpi_table;
        }

        // SAFETY: `reg` is guaranteed to be a valid `RegType`.
        to_result(unsafe { bindings::__platform_driver_register(reg.get(), module.0) })
    }

    fn unregister_bus_driver(reg: &Opaque<Self::RegType>) {
        // SAFETY: `reg` is guaranteed to be a valid `RegType` that was previously registered.
        unsafe { bindings::platform_driver_unregister(reg.get()) };
    }
}

/// AMBA bus implementation.
pub struct AmbaBus;

impl_bus!(
    AmbaBus,
    Device = amba::Device<device::Core>,
    RegType = bindings::amba_driver,
    RawDevice = bindings::amba_device,
    ProbeFn = unsafe extern "C" fn(*mut bindings::amba_device, *const bindings::amba_id) -> c_int,
    RemoveFn = unsafe extern "C" fn(*mut bindings::amba_device),
    from_raw_device = amba::Device<device::Core>
);

/// Trait for AMBA bus driver device tables.
///
/// AMBA bus drivers should implement this trait to provide AMBA device ID tables.
/// This trait requires the driver to also implement `DriverGeneric<AmbaBus>`.
pub trait AmbaIdInfos: DriverGeneric<AmbaBus> {
    /// The table of AMBA device ids supported by the driver.
    const ID_TABLE: Option<amba::IdTable<<Self as DriverGeneric<AmbaBus>>::IdInfo>>;
}

impl<T: DriverGeneric<AmbaBus> + AmbaIdInfos> BusRegistration<T> for AmbaBus {
    fn get_probe_fn() -> Self::ProbeFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static,
    {
        <Adapter<T, Self>>::probe_callback
    }

    fn get_remove_fn() -> Self::RemoveFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static,
    {
        <Adapter<T, Self>>::remove_callback
    }

    fn register_bus_driver(
        reg: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
        probe_fn: Self::ProbeFn,
        remove_fn: Self::RemoveFn,
    ) -> Result {
        // Get device table from the driver (AMBA-specific: id_table)
        let id_table = match <T as AmbaIdInfos>::ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        // SAFETY: It's safe to set the fields of `struct amba_driver` on initialization.
        unsafe {
            (*reg.get()).drv.name = name.as_char_ptr();
            (*reg.get()).probe = Some(probe_fn);
            (*reg.get()).remove = Some(remove_fn);
            (*reg.get()).id_table = id_table;
        }

        // SAFETY: `reg` is guaranteed to be a valid `RegType`.
        to_result(unsafe { bindings::__amba_driver_register(reg.get(), module.0) })
    }

    fn unregister_bus_driver(reg: &Opaque<Self::RegType>) {
        // SAFETY: `reg` is guaranteed to be a valid `RegType` that was previously registered.
        unsafe { bindings::amba_driver_unregister(reg.get()) };
    }
}

/// I2C bus implementation.
pub struct I2cBus;

impl_bus!(
    I2cBus,
    Device = i2c::I2cClient<device::Core>,
    RegType = bindings::i2c_driver,
    RawDevice = bindings::i2c_client,
    ProbeFn = unsafe extern "C" fn(*mut bindings::i2c_client) -> c_int,
    RemoveFn = unsafe extern "C" fn(*mut bindings::i2c_client),
    from_raw_device = i2c::I2cClient<device::Core>
);

/// Trait for I2C bus driver device tables.
///
/// I2C bus drivers should implement this trait to provide I2C device ID tables.
/// This trait requires the driver to also implement `DriverGeneric<I2cBus>`.
pub trait I2cIdInfos: DriverGeneric<I2cBus> {
    /// The table of I2C device ids supported by the driver.
    const I2C_ID_TABLE: Option<i2c::IdTable<<Self as DriverGeneric<I2cBus>>::IdInfo>>;
    /// The table of OF device ids supported by the driver.
    const OF_ID_TABLE: Option<of::IdTable<<Self as DriverGeneric<I2cBus>>::IdInfo>> = None;
    /// The table of ACPI device ids supported by the driver.
    const ACPI_ID_TABLE: Option<acpi::IdTable<<Self as DriverGeneric<I2cBus>>::IdInfo>> = None;
}

impl<T: DriverGeneric<I2cBus> + I2cIdInfos> BusRegistration<T> for I2cBus {
    fn get_probe_fn() -> Self::ProbeFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static,
    {
        <Adapter<T, Self>>::probe_callback
    }

    fn get_remove_fn() -> Self::RemoveFn
    where
        T: DriverGeneric<Self> + RtcOperations + 'static,
    {
        <Adapter<T, Self>>::remove_callback
    }

    fn register_bus_driver(
        reg: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
        probe_fn: Self::ProbeFn,
        remove_fn: Self::RemoveFn,
    ) -> Result {
        // Get device tables from the driver (I2C-specific: id_table, of_table, acpi_table)
        let i2c_table = match <T as I2cIdInfos>::I2C_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        let of_table = match <T as I2cIdInfos>::OF_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        let acpi_table = match <T as I2cIdInfos>::ACPI_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        // SAFETY: It's safe to set the fields of `struct i2c_driver` on initialization.
        unsafe {
            (*reg.get()).driver.name = name.as_char_ptr();
            (*reg.get()).probe = Some(probe_fn);
            (*reg.get()).remove = Some(remove_fn);
            (*reg.get()).id_table = i2c_table;
            (*reg.get()).driver.of_match_table = of_table;
            (*reg.get()).driver.acpi_match_table = acpi_table;
        }

        // SAFETY: `reg` is guaranteed to be a valid `RegType`.
        to_result(unsafe { bindings::i2c_register_driver(module.0, reg.get()) })
    }

    fn unregister_bus_driver(reg: &Opaque<Self::RegType>) {
        // SAFETY: `reg` is guaranteed to be a valid `RegType` that was previously registered.
        unsafe { bindings::i2c_del_driver(reg.get()) };
    }
}

/// The RTC driver trait, generic over bus type.
///
/// RTC drivers can be registered on different bus types (platform, amba, etc.).
/// The `B` type parameter specifies which bus type the driver uses.
///
/// For backward compatibility, the `Driver` trait is provided as an alias for
/// `DriverGeneric<PlatformBus>`.
///
/// Note: Device tables (OF, ACPI, etc.) are defined in bus-specific traits
/// (e.g., `PlatformIdInfos` for Platform bus).
pub trait DriverGeneric<B: Bus>: Send {
    /// The type holding driver private data about each device id supported by the driver.
    type IdInfo: 'static;

    /// RTC driver probe.
    ///
    /// Called when a new device is bound to this driver.
    /// Implementers should initialize the driver's private data here.
    /// The RTC device registration is handled automatically by the adapter.
    fn probe(dev: &B::Device, id_info: Option<&Self::IdInfo>) -> impl PinInit<Self, Error>;

    /// Initialize the RTC device after allocation but before registration.
    ///
    /// This method is called by the adapter to initialize the RTC device.
    /// The `id_info` parameter contains the device ID information that was matched
    /// during probe, which can be used to configure device-specific settings.
    /// The `drvdata` parameter provides mutable access to the driver's private data,
    /// which must be obtained via `Device::drvdata_borrow()`.
    /// The `rtc` parameter provides access to the RTC device, from which the parent
    /// bus device can be obtained via `rtc.bound_parent_device()`.
    /// The default implementation does nothing.
    fn init_rtcdevice(
        _rtc: &RtcDevice,
        _drvdata: &mut Self,
        _id_info: Option<&Self::IdInfo>,
    ) -> Result {
        Ok(())
    }

    /// Get the RTC device options.
    ///
    /// Returns the options for creating the RTC device.
    fn rtc_options() -> RtcDeviceOptions {
        RtcDeviceOptions {
            name: crate::c_str!("rtc"),
        }
    }

    /// RTC driver unbind.
    ///
    /// Called when the device is about to be unbound from this driver.
    fn unbind(dev: &B::Device, data: Pin<&Self>) {
        let _ = (dev, data);
    }
}

/// An adapter for the registration of RTC drivers.
///
/// RTC drivers can be registered on different bus types (platform, amba, etc.).
/// The adapter wraps the bus-specific driver registration mechanism while providing
/// RTC-specific semantics.
pub struct Adapter<T: DriverGeneric<B>, B: Bus>(PhantomData<(T, B)>);

// SAFETY: A call to `unregister` for a given instance of `RegType` is guaranteed to be valid if
// a preceding call to `register` has been successful.
unsafe impl<T, B> driver::RegistrationOps for Adapter<T, B>
where
    T: DriverGeneric<B> + RtcOperations + 'static,
    B: Bus + BusRegistration<T>,
{
    type RegType = B::RegType;

    unsafe fn register(
        reg: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result {
        // Delegate to the bus-specific registration.
        // The bus implementation will handle device tables and callbacks internally.
        B::register::<T>(reg, name, module)
    }

    unsafe fn unregister(reg: &Opaque<Self::RegType>) {
        // Delegate to the bus-specific unregistration.
        B::unregister::<T>(reg)
    }
}

// PlatformBus-specific probe callback (single parameter)
impl<T: DriverGeneric<PlatformBus> + PlatformIdInfos + RtcOperations + 'static>
    Adapter<T, PlatformBus>
{
    extern "C" fn probe_callback(raw: *mut bindings::platform_device) -> c_int {
        // SAFETY: The bus only ever calls the probe callback with a valid pointer.
        // `Device` is `#[repr(transparent)]`, so `CoreInternal` and `Core` have the same layout.
        let dev = unsafe { &*raw.cast::<platform::Device<device::Core>>() };
        let dev_ref = PlatformBus::as_device(dev);

        // Get ID info from the bus
        let info = PlatformBus::id_info::<T>(dev_ref);

        Self::probe_impl(dev, info)
    }
}

// AmbaBus-specific probe callback (two parameters)
impl<T: DriverGeneric<AmbaBus> + AmbaIdInfos + RtcOperations + 'static> Adapter<T, AmbaBus> {
    extern "C" fn probe_callback(
        raw: *mut bindings::amba_device,
        id: *const bindings::amba_id,
    ) -> c_int {
        // SAFETY: The bus only ever calls the probe callback with a valid pointer.
        // `Device` is `#[repr(transparent)]`, so `CoreInternal` and `Core` have the same layout.
        let dev = unsafe { &*raw.cast::<amba::Device<device::Core>>() };

        let info = NonNull::new(id.cast_mut()).and_then(|id_ptr| {
            // SAFETY: `DeviceId` is `#[repr(transparent)]` over `amba_id`, so it's safe to cast.
            let device_id = unsafe { &*id_ptr.as_ptr().cast::<amba::DeviceId>() };
            <T as AmbaIdInfos>::ID_TABLE.map(|table| {
                table.info(<amba::DeviceId as crate::device_id::RawDeviceIdIndex>::index(device_id))
            })
        });

        Self::probe_impl(dev, info)
    }
}

// I2cBus-specific probe callback (single parameter)
impl<T: DriverGeneric<I2cBus> + I2cIdInfos + RtcOperations + 'static> Adapter<T, I2cBus> {
    extern "C" fn probe_callback(raw: *mut bindings::i2c_client) -> c_int {
        // SAFETY: The bus only ever calls the probe callback with a valid pointer.
        // `Device` is `#[repr(transparent)]`, so `CoreInternal` and `Core` have the same layout.
        let dev = unsafe { &*raw.cast::<i2c::I2cClient<device::Core>>() };

        // Try I2C ID table first, then fall back to OF/ACPI.
        let info = Self::i2c_id_info(dev).or_else(|| {
            let dev_ref = I2cBus::as_device(dev);
            <Adapter<T, I2cBus> as driver::Adapter>::id_info(dev_ref)
        });

        Self::probe_impl(dev, info)
    }

    /// The [`i2c::IdTable`] of the corresponding driver.
    fn i2c_id_table() -> Option<i2c::IdTable<<Self as driver::Adapter>::IdInfo>> {
        <T as I2cIdInfos>::I2C_ID_TABLE
    }

    /// Returns the driver's private data from the matching entry in the [`i2c::IdTable`], if any.
    ///
    /// If this returns `None`, it means there is no match with an entry in the [`i2c::IdTable`].
    fn i2c_id_info(dev: &i2c::I2cClient) -> Option<&'static <Self as driver::Adapter>::IdInfo> {
        let table = Self::i2c_id_table()?;

        let dev_ref = dev.as_ref();
        let raw_client = dev_ref.as_raw().cast::<bindings::i2c_client>();

        // SAFETY: `table` has static lifetime, hence it's valid for reads.
        // `dev` is guaranteed to be valid while it's alive, and so is `raw_client`.
        let raw_id = unsafe { bindings::i2c_match_id(table.as_ptr(), raw_client) };

        if raw_id.is_null() {
            return None;
        }

        // SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `struct i2c_device_id` and
        // does not add additional invariants, so it's safe to cast.
        let id = unsafe { &*raw_id.cast::<i2c::DeviceId>() };

        Some(table.info(<i2c::DeviceId as crate::device_id::RawDeviceIdIndex>::index(id)))
    }
}

// Common probe implementation
impl<T: DriverGeneric<B> + RtcOperations + 'static, B: Bus> Adapter<T, B> {
    fn probe_impl(dev: &B::Device, info: Option<&T::IdInfo>) -> c_int {
        from_result(|| {
            let data = T::probe(dev, info);
            let dev_ref = B::as_device(dev);

            // Store the driver data in the device first.
            // SAFETY: `Device<Core>` and `Device<CoreInternal>` have the same layout, and
            // `from_ref(dev_ref)` is guaranteed to be valid.
            let dev_internal = unsafe {
                &*core::ptr::from_ref(dev_ref).cast::<device::Device<device::CoreInternal>>()
            };
            dev_internal.set_drvdata(data)?;

            // Allocate RTC device.
            let dev_raw = dev_ref.as_raw();
            // SAFETY: `devm_rtc_allocate_device` returns a pointer to a devm-managed rtc_device.
            let rtc: *mut bindings::rtc_device =
                unsafe { bindings::devm_rtc_allocate_device(dev_raw) };
            if rtc.is_null() {
                return Err(code::ENOMEM);
            }

            // Initialize the RTC device.
            // SAFETY: `rtc` is a valid pointer to a newly allocated rtc_device.
            // `RtcDevice` is `#[repr(transparent)]` over `Opaque<rtc_device>`, so we can safely
            // cast.
            let rtc_device = unsafe { &*rtc.cast() };
            // SAFETY: `dev_internal` is a valid device with driver data set by `set_drvdata` above.
            let drvdata_ptr = unsafe { bindings::dev_get_drvdata(dev_internal.as_raw()) };
            // SAFETY: `drvdata_ptr` is a valid pointer to `T` that was set by `set_drvdata` above.
            let drvdata = unsafe { &mut *drvdata_ptr.cast::<T>() };
            T::init_rtcdevice(rtc_device, drvdata, info)?;

            // Set the RTC device ops.
            // SAFETY: We just allocated the RTC device, so it's safe to set the ops.
            unsafe {
                (*rtc).ops = &RtcVTable::<T>::VTABLE;
            }

            // Register the RTC device.
            // SAFETY: The device will be automatically unregistered when the parent device
            // is removed (devm cleanup). The helper function uses `THIS_MODULE` internally.
            let err = unsafe { bindings::devm_rtc_register_device(rtc) };

            if err != 0 {
                return Err(Error::from_errno(err));
            }

            Ok(0)
        })
    }

    extern "C" fn remove_callback(raw: *mut B::RawDevice) {
        // The bus only ever calls the remove callback with a valid pointer.
        let dev = B::from_raw(raw);

        let dev_ref = B::as_device(dev);
        // SAFETY: `Device<Core>` and `Device<CoreInternal>` have the same layout.
        let dev_internal = unsafe {
            &*core::ptr::from_ref(dev_ref).cast::<device::Device<device::CoreInternal>>()
        };
        // SAFETY: Driver data has been set during probe.
        let data = unsafe { dev_internal.drvdata_obtain::<T>() };

        T::unbind(dev, data.as_ref());
    }
}

// Implementation for PlatformBus
impl<T: DriverGeneric<PlatformBus> + PlatformIdInfos + RtcOperations + 'static> driver::Adapter
    for Adapter<T, PlatformBus>
{
    type IdInfo = T::IdInfo;

    fn of_id_table() -> Option<of::IdTable<Self::IdInfo>> {
        <T as PlatformIdInfos>::OF_ID_TABLE
    }

    fn acpi_id_table() -> Option<acpi::IdTable<Self::IdInfo>> {
        <T as PlatformIdInfos>::ACPI_ID_TABLE
    }
}

// Implementation for I2cBus
impl<T: DriverGeneric<I2cBus> + I2cIdInfos + RtcOperations + 'static> driver::Adapter
    for Adapter<T, I2cBus>
{
    type IdInfo = T::IdInfo;

    fn of_id_table() -> Option<of::IdTable<Self::IdInfo>> {
        <T as I2cIdInfos>::OF_ID_TABLE
    }

    fn acpi_id_table() -> Option<acpi::IdTable<Self::IdInfo>> {
        <T as I2cIdInfos>::ACPI_ID_TABLE
    }
}

/// Options for creating an RTC device.
#[derive(Copy, Clone)]
pub struct RtcDeviceOptions {
    /// The name of the RTC device.
    pub name: &'static CStr,
}

/// Trait implemented by RTC device operations.
///
/// This trait defines the operations that an RTC device driver must implement.
/// Most methods are optional and have default implementations that return an error.
#[vtable]
pub trait RtcOperations: Sized {
    /// What kind of pointer should `Self` be wrapped in.
    type Ptr: ForeignOwnable + Send + Sync;

    /// Read the current time from the RTC.
    ///
    /// This is a required method and must be implemented.
    fn read_time(drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>, tm: &mut RtcTime) -> Result;

    /// Set the time in the RTC.
    ///
    /// This is a required method and must be implemented.
    ///
    /// Note: The parameter is `&mut` to match the C API signature, even though
    /// it's conceptually read-only from the Rust side.
    fn set_time(drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>, tm: &mut RtcTime) -> Result;

    /// Read the alarm time from the RTC.
    fn read_alarm(
        _drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _alarm: &mut RtcWkAlrm,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Set the alarm time in the RTC.
    ///
    /// Note: The parameter is `&mut` to match the C API signature, even though
    /// it's conceptually read-only from the Rust side.
    fn set_alarm(
        _drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _alarm: &mut RtcWkAlrm,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Enable or disable the alarm interrupt.
    ///
    /// `enabled` is non-zero to enable, zero to disable.
    fn alarm_irq_enable(
        _drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _enabled: u32,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Handle custom ioctl commands.
    fn ioctl(
        _drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _cmd: u32,
        _arg: c_ulong,
    ) -> Result<c_int> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Show information in /proc/driver/rtc.
    fn proc(_drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>, _seq: &mut SeqFile) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Read the time offset.
    fn read_offset(
        _drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _offset: &mut i64,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Set the time offset.
    fn set_offset(_drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>, _offset: i64) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Get an RTC parameter.
    fn param_get(
        _drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _param: &mut RtcParam,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Set an RTC parameter.
    ///
    /// Note: The parameter is `&mut` to match the C API signature, even though
    /// it's conceptually read-only from the Rust side.
    fn param_set(
        _drvdata: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _param: &mut RtcParam,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// A vtable for the RTC operations of a Rust RTC device.
pub struct RtcVTable<T: RtcOperations>(PhantomData<T>);

impl<T: RtcOperations> RtcVTable<T> {
    /// Get the vtable for the RTC operations.
    pub const fn get() -> &'static bindings::rtc_class_ops {
        &Self::VTABLE
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device`
    /// (rtc->dev.parent) that has driver_data set to a pointer returned by
    /// `T::Ptr::into_foreign`. `tm` must be a valid pointer to a `struct rtc_time`.
    unsafe extern "C" fn read_time(
        dev: *mut bindings::device,
        tm: *mut bindings::rtc_time,
    ) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `tm` is valid and writable.
        // `RtcTime` is `#[repr(transparent)]` over `bindings::rtc_time`, so we can safely cast.
        let rtc_tm = unsafe { &mut *tm.cast::<RtcTime>() };

        match T::read_time(drvdata, rtc_tm) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device`
    /// (rtc->dev.parent) that has driver_data set to a pointer returned by
    /// `T::Ptr::into_foreign`. `tm` must be a valid pointer to a `struct rtc_time`.
    unsafe extern "C" fn set_time(
        dev: *mut bindings::device,
        tm: *mut bindings::rtc_time,
    ) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `tm` is valid and writable.
        // `RtcTime` is `#[repr(transparent)]` over `bindings::rtc_time`, so we can safely cast.
        let rtc_tm = unsafe { &mut *tm.cast::<RtcTime>() };

        match T::set_time(drvdata, rtc_tm) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device` (rtc->dev.parent)
    /// that has driver_data set to a pointer returned by `T::Ptr::into_foreign`.
    /// `alarm` must be a valid pointer to a `struct rtc_wkalrm`.
    unsafe extern "C" fn read_alarm(
        dev: *mut bindings::device,
        alarm: *mut bindings::rtc_wkalrm,
    ) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `alarm` is valid and writable.
        // `RtcWkAlrm` is `#[repr(transparent)]` over `bindings::rtc_wkalrm`, so we can safely cast.
        let rtc_alarm = unsafe { &mut *alarm.cast::<RtcWkAlrm>() };

        match T::read_alarm(drvdata, rtc_alarm) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device` (rtc->dev.parent)
    /// that has driver_data set to a pointer returned by `T::Ptr::into_foreign`.
    /// `alarm` must be a valid pointer to a `struct rtc_wkalrm`.
    unsafe extern "C" fn set_alarm(
        dev: *mut bindings::device,
        alarm: *mut bindings::rtc_wkalrm,
    ) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `alarm` is valid and writable.
        // `RtcWkAlrm` is `#[repr(transparent)]` over `bindings::rtc_wkalrm`, so we can safely cast.
        let rtc_alarm = unsafe { &mut *alarm.cast::<RtcWkAlrm>() };

        match T::set_alarm(drvdata, rtc_alarm) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device` (rtc->dev.parent)
    /// that has driver_data set to a pointer returned by `T::Ptr::into_foreign`.
    unsafe extern "C" fn alarm_irq_enable(dev: *mut bindings::device, enabled: c_uint) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };

        match T::alarm_irq_enable(drvdata, enabled) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device` (rtc->dev.parent)
    /// that has driver_data set to a pointer returned by `T::Ptr::into_foreign`.
    unsafe extern "C" fn ioctl(dev: *mut bindings::device, cmd: c_uint, arg: c_ulong) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };

        match T::ioctl(drvdata, cmd, arg) {
            Ok(ret) => ret,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device`
    /// (rtc->dev.parent) that has driver_data set to a pointer returned by
    /// `T::Ptr::into_foreign`. `seq` must be a valid pointer to a `struct seq_file`.
    unsafe extern "C" fn proc(dev: *mut bindings::device, seq: *mut bindings::seq_file) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `seq` is valid and writable.
        let seq_file = unsafe { &mut *seq.cast::<SeqFile>() };

        match T::proc(drvdata, seq_file) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device`
    /// (rtc->dev.parent) that has driver_data set to a pointer returned by
    /// `T::Ptr::into_foreign`. `offset` must be a valid pointer to a `long`.
    unsafe extern "C" fn read_offset(dev: *mut bindings::device, offset: *mut c_long) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `offset` is valid and writable.
        let mut offset_val: i64 = unsafe { *offset.cast() };

        match T::read_offset(drvdata, &mut offset_val) {
            Ok(()) => {
                // SAFETY: The caller ensures that `offset` is valid and writable.
                unsafe { *offset.cast() = offset_val as c_long };
                0
            }
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device` (rtc->dev.parent)
    /// that has driver_data set to a pointer returned by `T::Ptr::into_foreign`.
    unsafe extern "C" fn set_offset(dev: *mut bindings::device, offset: c_long) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };

        match T::set_offset(drvdata, offset as i64) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device`
    /// (rtc->dev.parent) that has driver_data set to a pointer returned by
    /// `T::Ptr::into_foreign`. `param` must be a valid pointer to a `struct rtc_param`.
    unsafe extern "C" fn param_get(
        dev: *mut bindings::device,
        param: *mut bindings::rtc_param,
    ) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `param` is valid and writable.
        // `RtcParam` is `#[repr(transparent)]` over `bindings::rtc_param`, so we can safely cast.
        let rtc_param = unsafe { &mut *param.cast::<RtcParam>() };

        match T::param_get(drvdata, rtc_param) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the platform device's `struct device`
    /// (rtc->dev.parent) that has driver_data set to a pointer returned by
    /// `T::Ptr::into_foreign`. `param` must be a valid pointer to a `struct rtc_param`.
    unsafe extern "C" fn param_set(
        dev: *mut bindings::device,
        param: *mut bindings::rtc_param,
    ) -> c_int {
        // SAFETY: The caller ensures that `dev` is valid and has driver_data set to a pointer
        // returned by `T::Ptr::into_foreign`.
        let private = unsafe { bindings::dev_get_drvdata(dev) };
        if private.is_null() {
            return code::ENODEV.to_errno();
        }
        // SAFETY: RTC operations can borrow the private data of the device.
        let drvdata = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY: The caller ensures that `param` is valid and writable.
        // `RtcParam` is `#[repr(transparent)]` over `bindings::rtc_param`, so we can safely cast.
        let rtc_param = unsafe { &mut *param.cast::<RtcParam>() };

        match T::param_set(drvdata, rtc_param) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    const VTABLE: bindings::rtc_class_ops = bindings::rtc_class_ops {
        read_time: Some(Self::read_time),
        set_time: Some(Self::set_time),
        read_alarm: if T::HAS_READ_ALARM {
            Some(Self::read_alarm)
        } else {
            None
        },
        set_alarm: if T::HAS_SET_ALARM {
            Some(Self::set_alarm)
        } else {
            None
        },
        alarm_irq_enable: if T::HAS_ALARM_IRQ_ENABLE {
            Some(Self::alarm_irq_enable)
        } else {
            None
        },
        ioctl: if T::HAS_IOCTL {
            Some(Self::ioctl)
        } else {
            None
        },
        proc_: if T::HAS_PROC { Some(Self::proc) } else { None },
        read_offset: if T::HAS_READ_OFFSET {
            Some(Self::read_offset)
        } else {
            None
        },
        set_offset: if T::HAS_SET_OFFSET {
            Some(Self::set_offset)
        } else {
            None
        },
        param_get: if T::HAS_PARAM_GET {
            Some(Self::param_get)
        } else {
            None
        },
        param_set: if T::HAS_PARAM_SET {
            Some(Self::param_set)
        } else {
            None
        },
    };
}

/// Declares a kernel module that exposes a single RTC driver.
///
/// This macro uses `module_driver!` with the RTC-specific [`Adapter`] to register
/// an RTC driver. RTC drivers can be registered on different bus types (platform, amba, etc.).
///
/// # Examples
///
/// For Platform bus:
/// ```ignore
/// kernel::module_rtc_driver! {
///     bus: PlatformBus,
///     type: MyRtcDriver,
///     name: "my_rtc",
///     authors: ["Author name"],
///     description: "My RTC driver",
///     license: "GPL v2",
/// }
/// ```
///
/// For AMBA bus:
/// ```ignore
/// kernel::module_rtc_driver! {
///     bus: AmbaBus,
///     type: MyRtcDriver,
///     name: "my_rtc",
///     authors: ["Author name"],
///     description: "My RTC driver",
///     license: "GPL v2",
/// }
/// ```
///
/// For I2C bus:
/// ```ignore
/// kernel::module_rtc_driver! {
///     bus: I2cBus,
///     type: MyRtcDriver,
///     name: "my_rtc",
///     authors: ["Author name"],
///     description: "My RTC driver",
///     license: "GPL v2",
/// }
/// ```
///
/// The `bus` parameter is required. Valid bus types are `PlatformBus`, `AmbaBus`, and `I2cBus`.
#[macro_export]
macro_rules! module_rtc_driver {
    // With explicit bus parameter (required)
    (bus: $bus:ident, $($f:tt)*) => {
        $crate::module_driver!(<T>, $crate::rtc::Adapter<T, $crate::rtc::$bus>, { $($f)* });
    };
}
