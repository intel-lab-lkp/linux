// SPDX-License-Identifier: GPL-2.0

//! RTC (Real-Time Clock) device support.
//!
//! C header: [`include/linux/rtc.h`](srctree/include/linux/rtc.h).
use crate::{
    bindings,
    bitmap::Bitmap,
    container_of,
    device, //
    error::{
        from_err_ptr,
        to_result,
        Error,
        VTABLE_DEFAULT_ERROR, //
    },
    prelude::*,
    seq_file::SeqFile,
    sync::aref::{
        ARef,
        AlwaysRefCounted, //
    },
    types::{
        ForeignOwnable,
        Opaque, //
    },
};

use core::{
    ffi::c_void,
    marker::PhantomData,
    ptr::NonNull, //
};

/// RTC time structure.
/// Mirrors [`struct rtc_time`](srctree/include/uapi/linux/rtc.h).
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct RtcTime(pub bindings::rtc_time);

impl RtcTime {
    /// Creates a new `RtcTime` from a time64_t value.
    pub fn from_time64(time: i64) -> Self {
        let mut tm = Self(pin_init::zeroed());
        // SAFETY: `rtc_time64_to_tm` is a pure function that only writes to
        // the provided `struct rtc_time` pointer. The pointer is valid
        // because `tm.0` is a valid mutable reference, and the function does
        // not retain any references to it.
        unsafe { bindings::rtc_time64_to_tm(time, &mut tm.0) };

        tm
    }

    /// Converts this `RtcTime` to a time64_t value.
    pub fn to_time64(&self) -> i64 {
        // SAFETY: `rtc_tm_to_time64` is a pure function that only reads from
        // the provided `struct rtc_time` pointer. The pointer is valid because
        // `self.0` is a valid reference, and the function does not retain any
        // references to it.
        unsafe { bindings::rtc_tm_to_time64(core::ptr::from_ref(&self.0).cast_mut()) }
    }

    /// Sets this `RtcTime` from a time64_t value.
    pub fn set_from_time64(&mut self, time: i64) {
        // SAFETY: `rtc_time64_to_tm` is a pure function that only writes to
        // the provided `struct rtc_time` pointer. The pointer is valid
        // because `self.0` is a valid mutable reference, and the function
        // does not retain any references to it.
        unsafe { bindings::rtc_time64_to_tm(time, &mut self.0) };
    }

    /// Converts a time64_t value to an RTC time structure.
    #[inline]
    pub fn time64_to_tm(time: i64, tm: &mut Self) {
        tm.set_from_time64(time);
    }

    /// Converts an RTC time structure to a time64_t value.
    #[inline]
    pub fn tm_to_time64(tm: &Self) -> i64 {
        tm.to_time64()
    }

    /// Calculates the day of the year (0-365) from the date components.
    #[inline]
    pub fn year_days(&self) -> i32 {
        // SAFETY: `rtc_year_days` is a pure function that only performs
        // calculations based on the provided parameters. The values should be
        // from valid RTC time structures and non-negative, but the function
        // itself is safe to call with any values.
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
        Self(pin_init::zeroed())
    }
}

/// RTC wake alarm structure.
/// Mirrors [`struct rtc_wkalrm`](srctree/include/uapi/linux/rtc.h).
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct RtcWkAlrm(pub bindings::rtc_wkalrm);

impl Default for RtcWkAlrm {
    fn default() -> Self {
        Self(pin_init::zeroed())
    }
}

impl RtcWkAlrm {
    /// Returns a reference to the alarm time.
    #[inline]
    pub fn get_time(&self) -> &RtcTime {
        // SAFETY: `RtcTime` is `#[repr(transparent)]` over
        // `bindings::rtc_time`, so the memory layout is identical. We can
        // safely reinterpret the reference.
        unsafe { &*(&raw const self.0.time).cast::<RtcTime>() }
    }

    /// Returns a mutable reference to the alarm time.
    #[inline]
    pub fn get_time_mut(&mut self) -> &mut RtcTime {
        // SAFETY: `RtcTime` is `#[repr(transparent)]` over
        // `bindings::rtc_time`, so the memory layout is identical. We can
        // safely reinterpret the reference.
        unsafe { &mut *(&raw mut self.0.time).cast::<RtcTime>() }
    }

    /// Sets the alarm time.
    #[inline]
    pub fn set_time(&mut self, time: RtcTime) {
        self.0.time = time.0;
    }

    /// Returns the enabled field.
    #[inline]
    pub fn enabled(&self) -> bool {
        self.0.enabled != 0
    }

    /// Sets the `enabled` field.
    #[inline]
    pub fn set_enabled(&mut self, enabled: bool) {
        self.0.enabled = u8::from(enabled);
    }

    /// Returns the pending field.
    #[inline]
    pub fn pending(&self) -> bool {
        self.0.pending != 0
    }

    /// Sets the `pending` field.
    #[inline]
    pub fn set_pending(&mut self, pending: bool) {
        self.0.pending = u8::from(pending);
    }
}

/// RTC parameter structure.
/// Mirrors [`struct rtc_param`](srctree/include/uapi/linux/rtc.h).
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct RtcParam(pub bindings::rtc_param);

impl Default for RtcParam {
    fn default() -> Self {
        Self(pin_init::zeroed())
    }
}

/// Wrapper for an RTC device
/// [`struct rtc_device`](srctree/include/linux/rtc.h).
///
/// # Invariants
///
/// A [`RtcDevice`] instance holds a pointer to a valid
/// [`struct rtc_device`] that is registered and managed by the kernel.
///
#[repr(transparent)]
pub struct RtcDevice<T: 'static = ()>(Opaque<bindings::rtc_device>, PhantomData<T>);

impl<T: 'static> RtcDevice<T> {
    /// Returns a raw pointer to the underlying `rtc_device`.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::rtc_device {
        self.0.get()
    }

    /// Sets the minimum time range for the RTC device.
    #[inline]
    pub fn set_range_min(&self, min: i64) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only writing to the `range_min` field.
        unsafe { (*self.as_raw()).range_min = min };
    }

    /// Sets the maximum time range for the RTC device.
    #[inline]
    pub fn set_range_max(&self, max: u64) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only writing to the `range_max` field.
        unsafe { (*self.as_raw()).range_max = max };
    }

    /// Gets the minimum time range for the RTC device.
    #[inline]
    pub fn range_min(&self) -> i64 {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only reading the `range_min` field.
        unsafe { (*self.as_raw()).range_min }
    }

    /// Gets the maximum time range for the RTC device.
    #[inline]
    pub fn range_max(&self) -> u64 {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and we're only reading the `range_max` field.
        unsafe { (*self.as_raw()).range_max }
    }

    /// Notifies the RTC framework that an interrupt has occurred.
    #[inline]
    pub fn update_irq(&self, num: usize, events: usize) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer
        // to a `struct rtc_device`. The rtc_update_irq function handles
        // NULL/ERR checks internally.
        unsafe { bindings::rtc_update_irq(self.as_raw(), num, events) };
    }

    /// Clears a feature bit in the RTC device.
    #[inline]
    pub fn clear_feature(&self, feature: u32) {
        // SAFETY: By the type invariants, self.as_raw() is a valid pointer to a
        // `struct rtc_device`, and features is a valid bitmap array with
        // RTC_FEATURE_CNT bits.
        let features_bitmap = unsafe {
            Bitmap::from_raw_mut(
                (*self.as_raw()).features.as_mut_ptr().cast::<usize>(),
                bindings::RTC_FEATURE_CNT as usize,
            )
        };
        features_bitmap.clear_bit(feature as usize);
    }
}

impl<T: 'static> AsRef<device::Device> for RtcDevice<T> {
    fn as_ref(&self) -> &device::Device {
        let raw = self.as_raw();
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a
        // pointer to a valid `struct rtc_device`.
        let dev = unsafe { &raw mut (*raw).dev };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(dev) }
    }
}

// SAFETY: Instances of `RtcDevice` are always reference-counted via the
// underlying `device`. The `struct rtc_device` contains a `struct device dev`
// as its first field, and the reference counting is managed through
// `get_device`/`put_device` on the `dev` field.
unsafe impl<T: 'static> AlwaysRefCounted for RtcDevice<T> {
    fn inc_ref(&self) {
        let dev: &device::Device = self.as_ref();
        // SAFETY: The existence of a shared reference guarantees that the
        // refcount is non-zero. `dev.as_raw()` is a valid pointer to a
        // `struct device` with a non-zero refcount.
        unsafe { bindings::get_device(dev.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        let rtc: *mut bindings::rtc_device = obj.cast().as_ptr();

        // SAFETY: By the type invariant of `Self`, `rtc` is a pointer to a
        // valid `struct rtc_device`. The `dev` field is the first field of
        // `struct rtc_device`, so we can safely access it.
        let dev = unsafe { &raw mut (*rtc).dev };

        // SAFETY: The safety requirements guarantee that the refcount is
        // non-zero.
        unsafe { bindings::put_device(dev) };
    }
}

// SAFETY: `RtcDevice` is reference-counted and can be released from any thread.
unsafe impl<T: 'static> Send for RtcDevice<T> {}

// SAFETY: `RtcDevice` can be shared among threads because all immutable
// methods are protected by the synchronization in `struct rtc_device`
// (via `ops_lock` mutex).
unsafe impl<T: 'static> Sync for RtcDevice<T> {}

impl<T: RtcOps> RtcDevice<T> {
    /// Allocates a new RTC device managed by devres.
    pub fn new(
        parent_dev: &device::Device<device::Bound>,
        init: impl PinInit<T, Error>,
    ) -> Result<ARef<Self>> {
        // Allocate RTC device.
        // SAFETY: `devm_rtc_allocate_device` returns a pointer to a
        // devm-managed rtc_device. We use `dev_internal.as_raw()` which is
        // `pub(crate)`, but we can access it through the same device pointer.
        let rtc: *mut bindings::rtc_device =
            from_err_ptr(unsafe { bindings::devm_rtc_allocate_device(parent_dev.as_raw()) })?;

        // Set the RTC device ops.
        // SAFETY: We just allocated the RTC device, so it's safe to set
        // the ops.
        unsafe { (*rtc).ops = Adapter::<T>::VTABLE.as_raw() };

        // SAFETY: `rtc` is a valid pointer to a newly allocated rtc_device.
        // `RtcDevice` is `#[repr(transparent)]` over `Opaque<rtc_device>`, so
        // we can safely cast.
        let rtcdev = unsafe { &*rtc.cast::<Self>() };
        // SAFETY: `rtc_device.as_raw()` is a valid pointer to a `struct
        // rtc_device`.
        let flags = unsafe {
            Bitmap::from_raw_mut(
                &mut (*rtcdev.as_raw()).flags,
                bindings::RTC_FEATURE_CNT as usize,
            )
        };
        flags.set_bit(bindings::RTC_OPS_USE_RTC_DEV as usize);

        rtcdev.set_drvdata(init)?;
        Ok(rtcdev.into())
    }

    /// Store a pointer to the bound driver's private data.
    fn set_drvdata(&self, data: impl PinInit<T, Error>) -> Result {
        let data = KBox::pin_init(data, GFP_KERNEL)?;

        // SAFETY: `self.as_raw()` is a valid pointer to a `struct rtc_device`.
        unsafe { bindings::dev_set_drvdata(self.as_ref().as_raw(), data.into_foreign().cast()) };
        Ok(())
    }

    /// Borrows the driver's private data bound to this [`RtcDevice`].
    pub fn drvdata(&self) -> Result<Pin<&T>> {
        // SAFETY: `self.as_raw()` is a valid pointer to a `struct device`.
        let ptr = unsafe { bindings::dev_get_drvdata(self.as_ref().as_raw()) };

        if ptr.is_null() {
            return Err(ENOENT);
        }

        // SAFETY: The caller ensures that `ptr` is valid and writable.
        Ok(unsafe { Pin::<KBox<T>>::borrow(ptr.cast()) })
    }

    /// Registers this RTC device with the RTC subsystem.
    pub fn register(&self) -> Result {
        // Registers an RTC device with the RTC subsystem.
        // SAFETY: The device will be automatically unregistered when the parent
        // device is removed (devm cleanup). The helper function uses
        // `THIS_MODULE` internally.
        to_result(unsafe { bindings::devm_rtc_register_device(self.as_raw()) })
    }

    /// Returns a reference to the parent device of this RTC device.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that the parent device exists and is bound.
    /// This is guaranteed by the RTC core during `RtcOps` callbacks.
    pub unsafe fn bound_parent_device(&self) -> &device::Device<device::Bound> {
        // SAFETY: Per the function's safety contract, the parent device exists.
        let parent = unsafe { self.as_ref().parent().unwrap_unchecked() };

        // SAFETY: Per the function's safety contract, the parent device is
        // bound. This is guaranteed by the RTC core during `RtcOps`
        // callbacks.
        unsafe { parent.as_bound() }
    }
}

impl<T: 'static> Drop for RtcDevice<T> {
    fn drop(&mut self) {
        let dev = self.as_ref().as_raw();

        // SAFETY: By the type invariants, `self.as_raw()` is a valid pointer to
        // a `struct device`.
        let ptr: *mut c_void = unsafe { bindings::dev_get_drvdata(dev) };

        // SAFETY: By the type invariants, `self.as_raw()` is a valid pointer to
        // a `struct device`.
        unsafe { bindings::dev_set_drvdata(dev, core::ptr::null_mut()) };

        if !ptr.is_null() {
            // SAFETY: `ptr` comes from a previous call to `into_foreign()`,
            // and `dev_get_drvdata()` guarantees to return the same pointer
            // given to `dev_set_drvdata()`.
            unsafe { drop(Pin::<KBox<T>>::from_foreign(ptr.cast())) };
        }
    }
}

/// Options for creating an RTC device.
#[derive(Copy, Clone)]
pub struct RtcDeviceOptions {
    /// The name of the RTC device.
    pub name: &'static CStr,
}

/// Trait defining the operations for an RTC driver.
#[vtable]
pub trait RtcOps: Sized + 'static {
    /// Reads the current time from the RTC.
    fn read_time(
        _rtcdev: &RtcDevice<Self>,
        _tm: &mut RtcTime,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets the time in the RTC.
    fn set_time(
        _rtcdev: &RtcDevice<Self>,
        _tm: &RtcTime,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Reads the alarm time from the RTC.
    fn read_alarm(
        _rtcdev: &RtcDevice<Self>,
        _alarm: &mut RtcWkAlrm,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets the alarm time in the RTC.
    fn set_alarm(
        _rtcdev: &RtcDevice<Self>,
        _alarm: &RtcWkAlrm,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Enables or disables the alarm interrupt.
    fn alarm_irq_enable(
        _rtcdev: &RtcDevice<Self>,
        _enabled: bool,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Handles custom ioctl commands.
    fn ioctl(
        _rtcdev: &RtcDevice<Self>,
        _cmd: u32,
        _arg: c_ulong,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result<c_int> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Shows information in /proc/driver/rtc.
    fn proc(
        _rtcdev: &RtcDevice<Self>,
        _seq: &mut SeqFile,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Reads the time offset.
    fn read_offset(
        _rtcdev: &RtcDevice<Self>,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result<i64> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets the time offset.
    fn set_offset(
        _rtcdev: &RtcDevice<Self>,
        _offset: i64,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Gets an RTC parameter.
    fn param_get(
        _rtcdev: &RtcDevice<Self>,
        _param: &mut RtcParam,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets an RTC parameter.
    fn param_set(
        _rtcdev: &RtcDevice<Self>,
        _param: &RtcParam,
        _parent_dev: &device::Device<device::Bound>,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

struct Adapter<T: RtcOps> {
    _p: PhantomData<T>,
}

impl<T: RtcOps> Adapter<T> {
    const VTABLE: RtcOpsVTable = create_rtc_ops::<T>();

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `tm` must be a valid pointer to a `struct rtc_time`.
    unsafe extern "C" fn read_time(
        dev: *mut bindings::device,
        tm: *mut bindings::rtc_time,
    ) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The caller ensures that `tm` is valid and writable.
        // `RtcTime` is `#[repr(transparent)]` over `bindings::rtc_time`, so we
        // can safely cast.
        let rtc_tm = unsafe { &mut *tm.cast::<RtcTime>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::read_time(rtc_dev, rtc_tm, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `tm` must be a valid pointer to a `struct rtc_time`.
    unsafe extern "C" fn set_time(
        dev: *mut bindings::device,
        tm: *mut bindings::rtc_time,
    ) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The caller ensures that `tm` is valid.
        // `RtcTime` is `#[repr(transparent)]` over `bindings::rtc_time`, so we
        // can safely cast.
        let rtc_tm = unsafe { &*tm.cast::<RtcTime>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::set_time(rtc_dev, rtc_tm, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `alarm` must be a valid pointer to a `struct rtc_wkalrm`.
    unsafe extern "C" fn read_alarm(
        dev: *mut bindings::device,
        alarm: *mut bindings::rtc_wkalrm,
    ) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The caller ensures that `alarm` is valid and writable.
        // `RtcWkAlrm` is `#[repr(transparent)]` over `bindings::rtc_wkalrm`, so
        // we can safely cast.
        let rtc_alarm = unsafe { &mut *alarm.cast::<RtcWkAlrm>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::read_alarm(rtc_dev, rtc_alarm, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `alarm` must be a valid pointer to a `struct rtc_wkalrm`.
    unsafe extern "C" fn set_alarm(
        dev: *mut bindings::device,
        alarm: *mut bindings::rtc_wkalrm,
    ) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The caller ensures that `alarm` is valid.
        // `RtcWkAlrm` is `#[repr(transparent)]` over `bindings::rtc_wkalrm`, so
        // we can safely cast.
        let rtc_alarm = unsafe { &*alarm.cast::<RtcWkAlrm>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::set_alarm(rtc_dev, rtc_alarm, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    unsafe extern "C" fn alarm_irq_enable(dev: *mut bindings::device, enabled: c_uint) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::alarm_irq_enable(rtc_dev, enabled != 0, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    unsafe extern "C" fn ioctl(dev: *mut bindings::device, cmd: c_uint, arg: c_ulong) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::ioctl(rtc_dev, cmd, arg, parent_dev) {
            Ok(ret) => ret,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `seq` must be a valid pointer to a `struct seq_file`.
    unsafe extern "C" fn proc(dev: *mut bindings::device, seq: *mut bindings::seq_file) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The caller ensures that `seq` is valid and writable.
        let seq_file = unsafe { &mut *seq.cast::<SeqFile>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::proc(rtc_dev, seq_file, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `offset` must be a valid pointer to a `long`.
    unsafe extern "C" fn read_offset(dev: *mut bindings::device, offset: *mut c_long) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::read_offset(rtc_dev, parent_dev) {
            Ok(offset_val) => {
                // SAFETY: The caller ensures that `offset` is valid and
                // writable.
                unsafe { *offset.cast() = offset_val as c_long };
                0
            }
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    unsafe extern "C" fn set_offset(dev: *mut bindings::device, offset: c_long) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::set_offset(rtc_dev, offset as i64, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `param` must be a valid pointer to a `struct rtc_param`.
    unsafe extern "C" fn param_get(
        dev: *mut bindings::device,
        param: *mut bindings::rtc_param,
    ) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The caller ensures that `param` is valid and writable.
        // `RtcParam` is `#[repr(transparent)]` over `bindings::rtc_param`,
        // so we can safely cast.
        let rtc_param = unsafe { &mut *param.cast::<RtcParam>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::param_get(rtc_dev, rtc_param, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `dev` must be a valid pointer to the `struct device` embedded in a
    /// `struct rtc_device`.
    /// `param` must be a valid pointer to a `struct rtc_param`.
    unsafe extern "C" fn param_set(
        dev: *mut bindings::device,
        param: *mut bindings::rtc_param,
    ) -> c_int {
        // SAFETY: `dev` is embedded in a `struct rtc_device`, so we can use
        // `AsBusDevice` to get it.
        let rtc_dev =
            unsafe { &*container_of!(dev, bindings::rtc_device, dev).cast::<RtcDevice<T>>() };
        // SAFETY: The caller ensures that `param` is valid.
        // `RtcParam` is `#[repr(transparent)]` over `bindings::rtc_param`,
        // so we can safely cast.
        let rtc_param = unsafe { &*param.cast::<RtcParam>() };
        // SAFETY: The RTC core guarantees that the parent device exists and is
        // bound during `RtcOps` callbacks.
        let parent_dev = unsafe { rtc_dev.bound_parent_device() };

        match T::param_set(rtc_dev, rtc_param, parent_dev) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }
}

/// VTable structure wrapper for RTC operations.
/// Mirrors [`struct rtc_class_ops`](srctree/include/linux/rtc.h).
#[repr(transparent)]
pub struct RtcOpsVTable(bindings::rtc_class_ops);

// SAFETY: RtcOpsVTable is Send. The vtable contains only function pointers,
// which are simple data types that can be safely moved across threads. The
// thread-safety of calling these functions is handled by the kernel's locking
// mechanisms.
unsafe impl Send for RtcOpsVTable {}

// SAFETY: RtcOpsVTable is Sync. The vtable is immutable after it is created,
// so it can be safely referenced and accessed concurrently by multiple threads
// e.g. to read the function pointers.
unsafe impl Sync for RtcOpsVTable {}

impl RtcOpsVTable {
    /// Returns a raw pointer to the underlying `rtc_class_ops` struct.
    pub(crate) const fn as_raw(&self) -> *const bindings::rtc_class_ops {
        &self.0
    }
}

/// Creates an RTC operations vtable for a type `T` that implements
/// `RtcOps`.
pub const fn create_rtc_ops<T: RtcOps>() -> RtcOpsVTable {
    let mut ops: bindings::rtc_class_ops = pin_init::zeroed();

    ops.read_time = if T::HAS_READ_TIME {
        Some(Adapter::<T>::read_time)
    } else {
        None
    };
    ops.set_time = if T::HAS_SET_TIME {
        Some(Adapter::<T>::set_time)
    } else {
        None
    };
    ops.read_alarm = if T::HAS_READ_ALARM {
        Some(Adapter::<T>::read_alarm)
    } else {
        None
    };
    ops.set_alarm = if T::HAS_SET_ALARM {
        Some(Adapter::<T>::set_alarm)
    } else {
        None
    };
    ops.alarm_irq_enable = if T::HAS_ALARM_IRQ_ENABLE {
        Some(Adapter::<T>::alarm_irq_enable)
    } else {
        None
    };
    ops.ioctl = if T::HAS_IOCTL {
        Some(Adapter::<T>::ioctl)
    } else {
        None
    };
    ops.proc_ = if T::HAS_PROC {
        Some(Adapter::<T>::proc)
    } else {
        None
    };
    ops.read_offset = if T::HAS_READ_OFFSET {
        Some(Adapter::<T>::read_offset)
    } else {
        None
    };
    ops.set_offset = if T::HAS_SET_OFFSET {
        Some(Adapter::<T>::set_offset)
    } else {
        None
    };
    ops.param_get = if T::HAS_PARAM_GET {
        Some(Adapter::<T>::param_get)
    } else {
        None
    };
    ops.param_set = if T::HAS_PARAM_SET {
        Some(Adapter::<T>::param_set)
    } else {
        None
    };

    RtcOpsVTable(ops)
}

/// Declares a kernel module that exposes a single RTC driver.
///
/// # Examples
///
///```ignore
/// kernel::module_rtc_platform_driver! {
///     type: MyDriver,
///     name: "Module name",
///     authors: ["Author name"],
///     description: "Description",
///     license: "GPL v2",
/// }
///```
#[macro_export]
macro_rules! module_rtc_platform_driver {
    ($($user_args:tt)*) => {
        $crate::module_platform_driver! {
            $($user_args)*
            imports_ns: ["RTC"],
        }
    };
}
