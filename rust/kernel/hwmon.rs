// SPDX-License-Identifier: GPL-2.0

//! Hardware Monitoring (Hwmon) abstractions.
//!
//! C header: [`include/linux/hwmon.h`](srctree/include/linux/hwmon.h)
//!
//! Currently, this abstraction supports registering a single temperature sensor with the
//! `temp1_input` attribute (read-only). Multi-channel support, additional sensor types (fan,
//! voltage, etc.), and writable attributes will be added in follow-up patches.

use crate::{
    bindings,
    device::Device,
    error::{from_err_ptr, from_result, Result},
    prelude::*,
    str::CStr,
};

use core::marker::{PhantomData, PhantomPinned};

/// Sensor type.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum SensorType {
    /// Temperature sensor.
    Temp,
}

impl TryFrom<u32> for SensorType {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            bindings::hwmon_sensor_types_hwmon_temp => Ok(Self::Temp),
            _ => Err(EINVAL),
        }
    }
}

/// Temperature attribute.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum TempAttr {
    /// Temperature input value, in millidegrees Celsius.
    Input,
}

impl TryFrom<u32> for TempAttr {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            bindings::hwmon_temp_attributes_hwmon_temp_input => Ok(Self::Input),
            _ => Err(ENOTSUPP),
        }
    }
}

const HWMON_T_INPUT: u32 = 1u32 << bindings::hwmon_temp_attributes_hwmon_temp_input;

/// The hwmon driver trait.
#[vtable]
pub trait Driver: Send + Sync {
    /// Reads a sensor value.
    fn read(&self, sensor: SensorType, attr: u32, channel: u32) -> Result<crate::ffi::c_long>;

    /// Returns the sysfs file permission bits for a sensor attribute.
    fn is_visible(&self, sensor: SensorType, attr: u32, channel: u32) -> u16;
}

/// Adapter translating C hwmon callbacks to [`Driver`] trait method calls.
struct Adapter<T: Driver> {
    _p: PhantomData<T>,
}

impl<T: Driver> Adapter<T> {
    /// # Safety
    ///
    /// Called by the hwmon core during and after registration with the `drvdata`
    /// pointer set in `hwmon_device_register_with_info`. The pointer remains valid
    /// until `hwmon_device_unregister` returns in [`Registration`]'s `Drop`.
    unsafe extern "C" fn is_visible_callback(
        drvdata: *const core::ffi::c_void,
        type_: u32,
        attr: u32,
        channel: crate::ffi::c_int,
    ) -> u16 {
        if drvdata.is_null() {
            return 0;
        }
        // SAFETY: `drvdata` is `inner_ptr` set in `Registration::new`. The
        // hwmon core's barrier in `hwmon_device_unregister` ensures this
        // pointer is valid for the lifetime of any callback invocation.
        let inner = unsafe { &*drvdata.cast::<HwmonInner<T>>() };

        let sensor = match SensorType::try_from(type_) {
            Ok(s) => s,
            Err(_) => return 0,
        };

        // C core guarantees `channel >= 0`.
        T::is_visible(&inner.driver, sensor, attr, channel as u32)
    }

    /// # Safety
    ///
    /// Called by the hwmon core. `dev` is the device created during registration, and `val`
    /// points to writable memory for the result.
    unsafe extern "C" fn read_callback(
        dev: *mut bindings::device,
        type_: u32,
        attr: u32,
        channel: crate::ffi::c_int,
        val: *mut crate::ffi::c_long,
    ) -> crate::ffi::c_int {
        from_result(|| {
            // SAFETY: `dev_get_drvdata` returns the pointer set during registration, valid
            // until `hwmon_device_unregister` completes.
            let drvdata = unsafe { bindings::dev_get_drvdata(dev) };
            if drvdata.is_null() {
                return Err(EINVAL);
            }
            let inner = unsafe { &*drvdata.cast::<HwmonInner<T>>() };

            let sensor = SensorType::try_from(type_)?;
            // C core guarantees `channel >= 0`.
            let result = T::read(&inner.driver, sensor, attr, channel as u32)?;

            // SAFETY: `val` is provided by the hwmon core and points to a valid `long`.
            unsafe { *val = result };
            Ok(0)
        })
    }
}

/// Container holding the driver and all C structures for hwmon registration.
struct HwmonInner<T: Driver> {
    driver: T,
    ops: bindings::hwmon_ops,
    temp_config: [u32; 2],
    temp_channel: bindings::hwmon_channel_info,
    channel_info_array: [*const bindings::hwmon_channel_info; 2],
    chip_info: bindings::hwmon_chip_info,
    _pin: PhantomPinned,
}

impl<T: Driver> HwmonInner<T> {
    /// Returns a placeholder with all pointer fields set to null. The returned structure is
    /// safe to drop — no dynamic resources are held before registration completes.
    fn new_placeholder(driver: T) -> Self {
        Self {
            driver,
            ops: bindings::hwmon_ops {
                is_visible: Some(Adapter::<T>::is_visible_callback),
                visible: 0,
                read: Some(Adapter::<T>::read_callback),
                read_string: None,
                write: None,
            },
            temp_config: [0; 2],
            temp_channel: bindings::hwmon_channel_info {
                type_: 0,
                config: core::ptr::null(),
            },
            channel_info_array: [core::ptr::null(), core::ptr::null()],
            chip_info: bindings::hwmon_chip_info {
                ops: core::ptr::null(),
                info: core::ptr::null(),
            },
            _pin: PhantomPinned,
        }
    }
}

/// A registered hwmon device.
pub struct Registration<T: Driver> {
    hwmon_dev: *mut bindings::device,
    // Held exclusively for drop ordering: keeps `HwmonInner<T>` alive until
    // `hwmon_device_unregister` returns. The value is never read.
    #[expect(dead_code)]
    inner: Pin<KBox<HwmonInner<T>>>,
}

impl<T: Driver> Registration<T> {
    /// Registers a new hwmon device.
    ///
    /// The device is registered as a child of `parent`. `name` must not contain characters
    /// rejected by `hwmon_is_bad_char` (hyphens, spaces, asterisks).
    pub fn new(parent: &Device, name: &CStr, data: T) -> Result<Self> {
        let mut boxed = KBox::new(HwmonInner::new_placeholder(data), GFP_KERNEL)?;

        // Wire self-referential pointers before pinning. We obtain a mutable raw pointer
        // from the uniquely-owned `KBox` — no intermediate `&mut` reference is created,
        // so no aliasing rules are violated.
        //
        // SAFETY: `boxed` is exclusively owned. We write only to C-struct fields meant
        // for one-time initialization, never to `driver`.
        let inner_ptr: *mut HwmonInner<T> = &raw mut *boxed;
        unsafe {
            (*inner_ptr).temp_config = [HWMON_T_INPUT, 0];
            (*inner_ptr).temp_channel = bindings::hwmon_channel_info {
                type_: bindings::hwmon_sensor_types_hwmon_temp,
                config: (*inner_ptr).temp_config.as_ptr(),
            };
            (*inner_ptr).channel_info_array =
                [&raw const (*inner_ptr).temp_channel, core::ptr::null()];
            (*inner_ptr).chip_info = bindings::hwmon_chip_info {
                ops: &raw const (*inner_ptr).ops,
                info: (*inner_ptr).channel_info_array.as_ptr(),
            };
        }

        // SAFETY: `HwmonInner<T>` is `!Unpin` (via `PhantomPinned`). All self-referential
        // pointers are now set to their final values. The struct will never be moved.
        let inner = unsafe { Pin::new_unchecked(boxed) };

        let drvdata: *mut core::ffi::c_void = inner_ptr.cast();

        // SAFETY: `chip_info` and all nested pointers target memory within the same
        // allocation, which remains valid until `hwmon_device_unregister` in `Drop`.
        // `parent.as_raw()` and `name.as_char_ptr()` are valid. The name is copied by
        // the kernel (via `dev_set_name` → `kvasprintf_const`), so no lifetime issue.
        let hwmon_dev = from_err_ptr(unsafe {
            bindings::hwmon_device_register_with_info(
                parent.as_raw(),
                name.as_char_ptr(),
                drvdata,
                &raw const (*inner_ptr).chip_info,
                core::ptr::null_mut(),
            )
        })?;

        Ok(Self { hwmon_dev, inner })
    }
}

impl<T: Driver> Drop for Registration<T> {
    fn drop(&mut self) {
        // SAFETY: `hwmon_dev` was returned by a successful registration. This call waits for
        // all in-flight callbacks before returning.
        unsafe { bindings::hwmon_device_unregister(self.hwmon_dev) };
    }
}

// SAFETY: `T: Driver` requires `T: Send`. `Pin<KBox<HwmonInner<T>>>` is `Send` when `T: Send`.
unsafe impl<T: Driver> Send for Registration<T> {}

// SAFETY: `T: Driver` requires `T: Sync`, which makes `HwmonInner<T>: Sync`, and therefore
// `Pin<KBox<HwmonInner<T>>>: Sync`. `Registration` has no public methods that could cause
// data races through shared references.
unsafe impl<T: Driver> Sync for Registration<T> {}
