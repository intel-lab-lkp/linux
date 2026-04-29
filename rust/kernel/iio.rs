// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Muchamad Coirul Anwar <muchamadcoirulanwar@gmail.com>
//! IIO subsystem abstractions.
//!
//! Minimal safe Rust wrappers for the Linux IIO (Industrial I/O) subsystem.
//! Provides [`Device`] for allocating and registering an IIO device, and the
//! [`IioDriver`] trait for implementing `read_raw` callbacks in safe Rust.

use crate::{
    bindings::{
        __iio_device_register, iio_chan_spec, iio_dev, iio_device_alloc, iio_device_free,
        iio_device_unregister, iio_info, INDIO_DIRECT_MODE,
    },
    device,
    error::{code::*, to_result, Result},
    prelude::*,
    ThisModule,
};
use core::{
    ffi::c_int,
    marker::PhantomData,
    mem::{size_of, zeroed},
    ptr::{drop_in_place, write},
};
use pin_init::{pin_data, pinned_drop};
/// IIO value type: single integer (`IIO_VAL_INT`).
pub const IIO_VAL_INT: c_int = crate::bindings::IIO_VAL_INT as c_int;
/// IIO value type: integer plus micro part (`IIO_VAL_INT_PLUS_MICRO`).
pub const IIO_VAL_INT_PLUS_MICRO: c_int = crate::bindings::IIO_VAL_INT_PLUS_MICRO as c_int;
/// IIO value type: integer plus nano part (`IIO_VAL_INT_PLUS_NANO`).
pub const IIO_VAL_INT_PLUS_NANO: c_int = crate::bindings::IIO_VAL_INT_PLUS_NANO as c_int;
/// IIO value type: fractional (`IIO_VAL_FRACTIONAL`).
pub const IIO_VAL_FRACTIONAL: c_int = crate::bindings::IIO_VAL_FRACTIONAL as c_int;

/// Represents the return value of a `read_raw` operation.
///
/// Each variant corresponds to an `IIO_VAL_*` constant and tells the
/// IIO core how to format `val` and `val2` for userspace.
pub enum IioVal {
    /// A single integer value.
    Int(i32),
    /// A fractional value represented as `val / val2`.
    Fractional(i32, i32),
    /// An integer plus a micro (10⁻⁶) fractional part: `val.val2`.
    IntPlusMicro(i32, i32),
    /// An integer plus a nano (10⁻⁹) fractional part: `val.val2`.
    IntPlusNano(i32, i32),
}

/// Trait to be implemented by IIO driver private data.
///
/// Implementors supply the `read_raw` callback invoked by the IIO core
/// when userspace reads a channel attribute (e.g. `in_angl_raw`).
pub trait IioDriver {
    /// Called by the IIO core when userspace reads a channel attribute.
    ///
    /// `chan` is the channel being read; `mask` selects the attribute
    /// (e.g. `IIO_CHAN_INFO_RAW`, `IIO_CHAN_INFO_SCALE`).
    fn read_raw(&self, chan: *const iio_chan_spec, mask: isize) -> Result<IioVal>;

    /// Returns the channel specifications for this driver.
    ///
    /// The default implementation returns an empty slice.
    fn channels(&self) -> &[iio_chan_spec] {
        &[]
    }
}

/// C-compatible trampoline for the `iio_info.read_raw` callback.
///
/// # Safety
///
/// This function is only called by the IIO core with valid pointers:
/// - `indio_dev` is a valid `iio_dev` allocated by `iio_device_alloc`.
/// - `chan` points to a valid channel spec from the device's channel array.
/// - `val` and `val2` are valid pointers for writing the result.
unsafe extern "C" fn read_raw_callback<T: IioDriver>(
    indio_dev: *mut iio_dev,
    chan: *const iio_chan_spec,
    val: *mut c_int,
    val2: *mut c_int,
    mask: isize,
) -> c_int {
    // SAFETY: `indio_dev` is valid and was allocated with space for `T` in its
    // private data area. The `priv_` field was initialized in `Device::new()`.
    let priv_ptr = unsafe { (*indio_dev).priv_ as *mut T };
    // SAFETY: `priv_ptr` points to a valid, initialized instance of `T` that
    // lives as long as the `iio_dev` allocation.
    let driver = unsafe { &*priv_ptr };

    match driver.read_raw(chan, mask) {
        Ok(IioVal::Int(v)) => {
            // SAFETY: `val` is a valid pointer provided by the IIO core.
            unsafe { *val = v };
            IIO_VAL_INT
        }
        Ok(IioVal::Fractional(v, v2)) => {
            // SAFETY: `val` and `val2` are valid pointers provided by the IIO core.
            unsafe {
                *val = v;
                *val2 = v2;
            };
            IIO_VAL_FRACTIONAL
        }
        Ok(IioVal::IntPlusMicro(v, v2)) => {
            // SAFETY: `val` and `val2` are valid pointers provided by the IIO core.
            unsafe {
                *val = v;
                *val2 = v2;
            }
            IIO_VAL_INT_PLUS_MICRO
        }
        Ok(IioVal::IntPlusNano(v, v2)) => {
            // SAFETY: `val` and `val2` are valid pointers provided by the IIO core.
            unsafe {
                *val = v;
                *val2 = v2;
            }
            IIO_VAL_INT_PLUS_NANO
        }
        Err(e) => e.to_errno(),
    }
}

/// A registered IIO device.
///
/// Wraps a C `struct iio_dev` and manages its lifetime. On drop the
/// device is unregistered (if registered) and its memory freed.
#[pin_data(PinnedDrop)]
pub struct Device<T: IioDriver> {
    indio_dev: *mut iio_dev,
    registered: bool,
    _p: PhantomData<T>,
}

// SAFETY: `Device` only contains a raw pointer to a kernel-managed `iio_dev`.
// The IIO core serializes access to the device, and `T` is required to be `Send`.
unsafe impl<T: IioDriver + Send> Send for Device<T> {}
// SAFETY: All `&self` access to the `iio_dev` is read-only or goes through the
// IIO core which provides its own synchronization. `T` is required to be `Sync`.
unsafe impl<T: IioDriver + Sync> Sync for Device<T> {}

#[pinned_drop]
impl<T: IioDriver> PinnedDrop for Device<T> {
    // SAFETY: `self.indio_dev` was allocated by `iio_device_alloc` in `new()`
    // and is valid for the lifetime of this struct. We unregister first (if
    // registered), then drop the private data, then free the `iio_dev`.
    // This ordering is critical: unregister ensures no more callbacks can
    // fire before we drop the driver data they reference.
    fn drop(self: Pin<&mut Self>) {
        unsafe {
            if self.registered {
                iio_device_unregister(self.indio_dev);
            }
            let priv_ptr = (*self.indio_dev).priv_ as *mut T;
            drop_in_place(priv_ptr);
            iio_device_free(self.indio_dev);
        }
    }
}

impl<T: IioDriver> Device<T> {
    // SAFETY: The remaining fields of `iio_info` are pointers and function
    // pointers. Zeroed values are NULL, and the IIO core checks for NULL
    // before invoking callbacks or dereferencing attribute group pointers.
    const VTABLE: iio_info = iio_info {
        read_raw: Some(read_raw_callback::<T>),
        ..unsafe { zeroed() }
    };

    /// Allocates a new IIO device with the given driver data.
    ///
    /// The device is not yet registered; call [`register`](Self::register)
    /// to make it visible to userspace.
    pub fn new(dev: &device::Device, data: T, name: &'static CStr) -> Result<Self> {
        // SAFETY: `dev.as_raw()` returns a valid `struct device` pointer.
        // `iio_device_alloc` allocates an `iio_dev` with `sizeof(T)` bytes of
        // private data. Returns NULL on failure.
        let indio_dev = unsafe { iio_device_alloc(dev.as_raw(), size_of::<T>() as _) };
        if indio_dev.is_null() {
            return Err(ENOMEM);
        }

        // SAFETY: `indio_dev` is a valid, newly allocated `iio_dev`.
        // - `priv_` points to an uninitialized area of `sizeof(T)` bytes.
        // - `core::ptr::write` initializes it without reading the old value.
        // - `name` is a `'static` C string that outlives the device.
        // - `VTABLE` is a `'static` const and outlives the device.
        // - `channels()` returns a reference to data owned by `T` in `priv_`,
        //   which remains at a fixed address for the lifetime of `indio_dev`
        //   because `priv_` is heap-allocated by `iio_device_alloc`.
        unsafe {
            let priv_ptr = (*indio_dev).priv_ as *mut T;
            write(priv_ptr, data);
            (*indio_dev).name = name.as_char_ptr();
            (*indio_dev).info = &Self::VTABLE;

            let chans = (*priv_ptr).channels();
            (*indio_dev).channels = chans.as_ptr() as *const iio_chan_spec;
            (*indio_dev).num_channels = chans.len() as _;
            (*indio_dev).modes = INDIO_DIRECT_MODE as i32;
        }

        Ok(Self {
            indio_dev,
            registered: false,
            _p: PhantomData,
        })
    }

    /// Registers the IIO device, making it visible to userspace via sysfs.
    ///
    /// On success, channel attributes like `in_angl_raw` become readable.
    /// On failure the device stays unregistered and will be freed when
    /// this [`Device`] is dropped.
    pub fn register(&mut self, _dev: &device::Device, module: &'static ThisModule) -> Result {
        // SAFETY: `self.indio_dev` is a valid, fully initialized `iio_dev`.
        // `module.as_ptr()` provides the module owner for proper refcounting.
        let ret = unsafe { __iio_device_register(self.indio_dev, module.as_ptr()) };
        to_result(ret)?;
        self.registered = true;
        Ok(())
    }
}
