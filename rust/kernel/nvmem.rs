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
#[repr(transparent)]
pub struct NvmemConfig<T: NvmemProvider + Default> {
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
            let context = context.cast::<T::Priv>();
            // SAFETY: context is a valid T::Priv as set in Device::nvmem_register().
            let context = unsafe { &*context };
            let val = val.cast::<u8>();
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
            let context = context.cast::<T::Priv>();
            // SAFETY: context is a valid T::Priv as set in Device::nvmem_register().
            let context = unsafe { &*context };
            let val = val.cast::<u8>().cast_const();
            // SAFETY: val should be non-null, and allocated for bytes bytes.
            let data = unsafe { core::slice::from_raw_parts(val, bytes) };
            T::write(context, offset, data).map(|()| 0)
        })
    }

    /// Optional name.
    pub fn with_name(mut self, name: &CStr) -> Self {
        self.inner.name = name.as_char_ptr();
        self
    }

    /// Type of the nvmem storage
    pub fn with_type(mut self, type_: Type) -> Self {
        self.inner.type_ = type_ as u32;
        self
    }

    /// Device is read-only.
    pub fn with_read_only(mut self, read_only: bool) -> Self {
        self.inner.read_only = read_only;
        self
    }

    /// Device is accessibly to root only.
    pub fn with_root_only(mut self, root_only: bool) -> Self {
        self.inner.root_only = root_only;
        self
    }

    /// Device size.
    pub fn with_size(mut self, size: i32) -> Self {
        self.inner.size = size;
        self
    }

    /// Minimum read/write access granularity.
    pub fn with_word_size(mut self, word_size: i32) -> Self {
        self.inner.word_size = word_size;
        self
    }

    /// Minimum read/write access stride.
    pub fn with_stride(mut self, stride: i32) -> Self {
        self.inner.stride = stride;
        self
    }
}

impl Device {
    /// Register a managed nvmem provider on the given device.
    pub fn nvmem_register<T>(&self, mut config: NvmemConfig<T>, priv_: &T::Priv)
    where
        T: NvmemProvider + Default,
    {
        // FIXME: The last cast to mut indicates some unsoundness here.
        config.inner.priv_ = core::ptr::from_ref(priv_).cast::<c_void>().cast_mut();
        config.inner.dev = self.as_raw();
        config.inner.reg_read = Some(NvmemConfig::<T>::reg_read);
        config.inner.reg_write = Some(NvmemConfig::<T>::reg_write);
        // SAFETY: Both self and config can’t be null here, and should have the correct type.
        unsafe { bindings::devm_nvmem_register(self.as_raw(), &config.inner) };
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
