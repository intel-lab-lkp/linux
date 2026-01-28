// SPDX-License-Identifier: GPL-2.0

//! nvmem framework provider.
//!
//! Copyright (C) 2026 Link Mauve <linkmauve@linkmauve.fr>

use crate::build_error;
use crate::device::Device;
use crate::error::{from_result, VTABLE_DEFAULT_ERROR};
use crate::prelude::*;
use core::marker::PhantomData;
use macros::vtable;

/// The possible types for a nvmem provider.
#[derive(Default)]
#[repr(u32)]
pub enum Type {
    /// The type of memory is unknown.
    #[default]
    Unknown = bindings::nvmem_type_NVMEM_TYPE_UNKNOWN,

    /// Electrically erasable programmable ROM.
    Eeprom = bindings::nvmem_type_NVMEM_TYPE_EEPROM,

    /// One-time programmable memory.
    Otp = bindings::nvmem_type_NVMEM_TYPE_OTP,

    /// This memory is backed by a battery.
    BatteryBacked = bindings::nvmem_type_NVMEM_TYPE_BATTERY_BACKED,

    /// Ferroelectric RAM.
    Fram = bindings::nvmem_type_NVMEM_TYPE_FRAM,
}

/// nvmem configuration.
///
/// Rust abstraction for the C `struct nvmem_config`.
#[derive(Default)]
pub struct NvmemConfig<T: NvmemProvider>
where
    T: Default,
{
    inner: bindings::nvmem_config,
    _p: PhantomData<T>,
}

impl<T: NvmemProvider + Default> NvmemConfig<T> {
    /// NvmemConfig's read callback.
    ///
    /// SAFETY: Called from C. Inputs must be valid pointers.
    extern "C" fn reg_read(
        context: *mut c_void,
        offset: u32,
        val: *mut c_void,
        bytes: usize,
    ) -> i32 {
        from_result(|| {
            // SAFETY: context is a valid T::Priv as defined in Self::set_priv().
            let context = unsafe { &*(context as *mut T::Priv) };
            let val = val as *mut u8;
            // SAFETY: val should be non-null, and allocated for bytes bytes.
            let data = unsafe { core::slice::from_raw_parts_mut(val, bytes) };
            T::read(context, offset, data).map(|()| 0)
        })
    }

    /// NvmemConfig's write callback.
    ///
    /// SAFETY: Called from C. Inputs must be valid pointers.
    extern "C" fn reg_write(
        context: *mut c_void,
        offset: u32,
        // TODO: Change val from void* to const void* in the C API!
        val: *mut c_void,
        bytes: usize,
    ) -> i32 {
        from_result(|| {
            // SAFETY: context is a valid T::Priv as defined in Self::set_priv().
            let context = unsafe { &*(context as *mut T::Priv) };
            let val = val as *mut u8 as *const u8;
            // SAFETY: val should be non-null, and allocated for bytes bytes.
            let data = unsafe { core::slice::from_raw_parts(val, bytes) };
            T::write(context, offset, data).map(|()| 0)
        })
    }

    /// User context passed to read/write callbacks.
    pub fn set_priv(&mut self, priv_: &T::Priv) {
        // FIXME: This list of as indicates some unsoundness in the types…
        self.inner.priv_ = priv_ as *const T::Priv as *const c_void as *mut c_void;
    }

    /// Sets the configuration on the given device.
    pub fn set(mut self, dev: &Device) {
        self.inner.reg_read = Some(Self::reg_read);
        self.inner.reg_write = Some(Self::reg_write);
        // SAFETY: All arguments should be non-null and what the function expects.
        unsafe { bindings::devm_nvmem_register(dev.as_raw(), &self.inner) };
    }

    /// Optional name.
    pub fn set_name(&mut self, name: &CStr) {
        // TODO: Why do we have to do this cast from i8 to u8?
        self.inner.name = name.as_ptr() as *const _;
    }

    /// Type of the nvmem storage
    pub fn set_type(&mut self, type_: Type) {
        self.inner.type_ = type_ as u32;
    }

    /// Device is read-only.
    pub fn set_read_only(&mut self, read_only: bool) {
        self.inner.read_only = read_only;
    }

    /// Device is accessibly to root only.
    pub fn set_root_only(&mut self, root_only: bool) {
        self.inner.root_only = root_only;
    }

    /// Device size.
    pub fn set_size(&mut self, size: i32) {
        self.inner.size = size;
    }

    /// Minimum read/write access granularity.
    pub fn set_word_size(&mut self, word_size: i32) {
        self.inner.word_size = word_size;
    }

    /// Minimum read/write access stride.
    pub fn set_stride(&mut self, stride: i32) {
        self.inner.stride = stride;
    }
}

/// Helper trait to define the callbacks of a nvmem provider.
#[vtable]
pub trait NvmemProvider {
    /// The type passed into the context for read and write functions.
    type Priv;

    /// Callback to read data.
    #[inline]
    fn read(_context: &Self::Priv, _offset: u32, _data: &mut [u8]) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Callback to write data.
    #[inline]
    fn write(_context: &Self::Priv, _offset: u32, _data: &[u8]) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}
