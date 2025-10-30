// SPDX-License-Identifier: GPL-2.0-only

//! Abstractions for the fwctl.
//!
//! This module provides bindings for working with fwctl devices in kernel modules.
//!
//! C header: [`include/linux/fwctl.h`]

use crate::device::Device;
use crate::types::ARef;
use crate::{bindings, container_of, device, error::code::*, prelude::*};

use core::marker::PhantomData;
use core::ptr::NonNull;
use core::slice;

/// The registration of a fwctl device.
///
/// This type represents the registration of a [`struct fwctl_device`]. When an instance of this
/// type is dropped, its respective fwctl device will be unregistered and freed.
///
/// [`struct fwctl_device`]: srctree/include/linux/device/fwctl.h
pub struct Registration<T: FwCtlOps> {
    fwctl_dev: NonNull<bindings::fwctl_device>,
    _marker: PhantomData<T>,
}

impl<T: FwCtlOps> Registration<T> {
    /// Allocate and register a new fwctl device under the given parent device.
    pub fn new(parent: &device::Device) -> Result<Self> {
        let ops = &FwCtlVTable::<T>::VTABLE as *const _ as *mut _;

        // SAFETY: `_fwctl_alloc_device()` allocates a new `fwctl_device`
        // and initializes its embedded `struct device`.
        let dev = unsafe {
            bindings::_fwctl_alloc_device(
                parent.as_raw(),
                ops,
                core::mem::size_of::<bindings::fwctl_device>(),
            )
        };

        let dev = NonNull::new(dev).ok_or(ENOMEM)?;

        // SAFETY: `fwctl_register()` expects a valid device from `_fwctl_alloc_device()`.
        let ret = unsafe { bindings::fwctl_register(dev.as_ptr()) };
        if ret != 0 {
            // SAFETY: If registration fails, release the allocated fwctl_device().
            unsafe {
                bindings::put_device(core::ptr::addr_of_mut!((*dev.as_ptr()).dev));
            }
            return Err(Error::from_errno(ret));
        }

        Ok(Self {
            fwctl_dev: dev,
            _marker: PhantomData,
        })
    }

    fn as_raw(&self) -> *mut bindings::fwctl_device {
        self.fwctl_dev.as_ptr()
    }
}

impl<T: FwCtlOps> Drop for Registration<T> {
    fn drop(&mut self) {
        // SAFETY: `fwctl_unregister()` expects a valid device from `_fwctl_alloc_device()`.
        unsafe {
            bindings::fwctl_unregister(self.as_raw());
            bindings::put_device(core::ptr::addr_of_mut!((*self.as_raw()).dev));
        }
    }
}

// SAFETY: The only action allowed in a `Registration` instance is dropping it, which is safe to do
// from any thread because `fwctl_unregister()/put_device()` can be called from any sleepible
// context.
unsafe impl<T: FwCtlOps> Send for Registration<T> {}

/// Trait implemented by each Rust driver that integrates with the fwctl subsystem.
///
/// Each implementation corresponds to a specific device type and provides
/// the vtable used by the core `fwctl` layer to manage per-FD user contexts
/// and handle RPC requests.
pub trait FwCtlOps: Sized {
    /// Driver UCtx type.
    type UCtx;

    /// fwctl device type, matching the C enum `fwctl_device_type`.
    const DEVICE_TYPE: u32;

    /// Called when a new user context is opened by userspace.
    fn open_uctx(uctx: &mut FwCtlUCtx<Self::UCtx>) -> Result<(), Error>;

    /// Called when the user context is being closed.
    fn close_uctx(uctx: &mut FwCtlUCtx<Self::UCtx>);

    /// Return device or context information to userspace.
    fn info(uctx: &mut FwCtlUCtx<Self::UCtx>) -> Result<KVec<u8>, Error>;

    /// Called when a userspace RPC request is received.
    fn fw_rpc(
        uctx: &mut FwCtlUCtx<Self::UCtx>,
        scope: u32,
        rpc_in: &mut [u8],
        out_len: *mut usize,
    ) -> Result<Option<KVec<u8>>, Error>;
}

/// Represents a per-FD user context (`struct fwctl_uctx`).
///
/// Each driver embeds `struct fwctl_uctx` as the first field of its own
/// context type and uses this wrapper to access driver-specific data.
#[repr(C)]
#[pin_data]
pub struct FwCtlUCtx<T> {
    /// The core fwctl user context shared with the C implementation.
    #[pin]
    pub fwctl_uctx: bindings::fwctl_uctx,
    /// Driver-specific data associated with this user context.
    pub uctx: T,
}

impl<T> FwCtlUCtx<T> {
    /// Converts a raw C pointer to `struct fwctl_uctx` into a reference to the
    /// enclosing `FwCtlUCtx<T>`.
    ///
    /// # Safety
    /// * `ptr` must be a valid pointer to a `fwctl_uctx` that is embedded
    ///   inside an existing `FwCtlUCtx<T>` instance.
    /// * The caller must ensure that the lifetime of the returned reference
    ///   does not outlive the underlying object managed on the C side.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::fwctl_uctx) -> &'a mut Self {
        // SAFETY: `ptr` was originally created from a valid `FwCtlUCtx<T>`.
        unsafe { &mut *container_of!(ptr, FwCtlUCtx<T>, fwctl_uctx) }
    }

    /// Returns the parent device of this user context.
    ///
    /// # Safety
    /// The `fwctl_device` pointer inside `fwctl_uctx` must be valid.
    pub fn get_parent_device(&self) -> ARef<Device> {
        // SAFETY: `self.fwctl_uctx.fwctl` is initialized by the fwctl subsystem and guaranteed
        // to remain valid for the lifetime of this `FwCtlUCtx`.
        let raw_dev =
            unsafe { (*(self.fwctl_uctx.fwctl)).dev.parent as *mut kernel::bindings::device };
        // SAFETY: `raw_dev` points to a live device object.
        unsafe { Device::get_device(raw_dev) }
    }

    /// Returns a mutable reference to the driver-specific context.
    pub fn to_driver_uctx_mut(&mut self) -> &mut T {
        &mut self.uctx
    }
}

/// Static vtable mapping Rust trait methods to C callbacks.
pub struct FwCtlVTable<T: FwCtlOps>(PhantomData<T>);

impl<T: FwCtlOps> FwCtlVTable<T> {
    /// Static instance of `fwctl_ops` used by the C core to call into Rust.
    pub const VTABLE: bindings::fwctl_ops = bindings::fwctl_ops {
        device_type: T::DEVICE_TYPE,
        uctx_size: core::mem::size_of::<FwCtlUCtx<T::UCtx>>(),
        open_uctx: Some(Self::open_uctx_callback),
        close_uctx: Some(Self::close_uctx_callback),
        info: Some(Self::info_callback),
        fw_rpc: Some(Self::fw_rpc_callback),
    };

    /// Called when a new user context is opened by userspace.
    unsafe extern "C" fn open_uctx_callback(uctx: *mut bindings::fwctl_uctx) -> ffi::c_int {
        // SAFETY: `uctx` is guaranteed by the fwctl subsystem to be a valid pointer.
        let ctx = unsafe { FwCtlUCtx::<T::UCtx>::from_raw(uctx) };
        match T::open_uctx(ctx) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    /// Called when the user context is being closed.
    unsafe extern "C" fn close_uctx_callback(uctx: *mut bindings::fwctl_uctx) {
        // SAFETY: `uctx` is guaranteed by the fwctl subsystem to be a valid pointer.
        let ctx = unsafe { FwCtlUCtx::<T::UCtx>::from_raw(uctx) };
        T::close_uctx(ctx);
    }

    /// Returns device or context information.
    unsafe extern "C" fn info_callback(
        uctx: *mut bindings::fwctl_uctx,
        length: *mut usize,
    ) -> *mut ffi::c_void {
        // SAFETY: `uctx` is guaranteed by the fwctl subsystem to be a valid pointer.
        let ctx = unsafe { FwCtlUCtx::<T::UCtx>::from_raw(uctx) };

        match T::info(ctx) {
            Ok(kvec) => {
                // The ownership of the buffer is now transferred to the foreign
                // caller. It must eventually be released by fwctl framework.
                let (ptr, len, _cap) = kvec.into_raw_parts();

                // SAFETY: `length` is a valid out-parameter provided by the C
                // caller. Write the number of bytes in the returned buffer.
                unsafe {
                    *length = len;
                }

                ptr.cast::<ffi::c_void>()
            }

            Err(e) => Error::to_ptr(e),
        }
    }

    /// Called when a user-space RPC request is received.
    unsafe extern "C" fn fw_rpc_callback(
        uctx: *mut bindings::fwctl_uctx,
        scope: u32,
        rpc_in: *mut ffi::c_void,
        in_len: usize,
        out_len: *mut usize,
    ) -> *mut ffi::c_void {
        // SAFETY: `uctx` is guaranteed by the fwctl framework to be a valid pointer.
        let ctx = unsafe { FwCtlUCtx::<T::UCtx>::from_raw(uctx) };

        // SAFETY: `rpc_in` points to a valid input buffer of size `in_len`
        // provided by fwctl subsystem.
        let rpc_in_slice: &mut [u8] =
            unsafe { slice::from_raw_parts_mut(rpc_in as *mut u8, in_len) };

        match T::fw_rpc(ctx, scope, rpc_in_slice, out_len) {
            // Driver allocates a new output buffer.
            Ok(Some(kvec)) => {
                // The ownership of the buffer is now transferred to the foreign
                // caller. It must eventually be released by fwctl subsystem.
                let (ptr, len, _cap) = kvec.into_raw_parts();

                // SAFETY: `out_len` is a valid writable pointer provided by the C caller.
                unsafe {
                    *out_len = len;
                }

                ptr.cast::<ffi::c_void>()
            }

            // Driver re-uses the existing input buffer and writes the out_len.
            Ok(None) => rpc_in,

            // Return an ERR_PTR-style encoded error pointer.
            Err(e) => Error::to_ptr(e),
        }
    }
}
