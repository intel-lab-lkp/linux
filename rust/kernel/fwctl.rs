// SPDX-License-Identifier: GPL-2.0-only

//! Abstractions for the fwctl subsystem.
//!
//! C header: `include/linux/fwctl.h`

use crate::{
    bindings,
    container_of,
    device,
    prelude::*,
    sync::aref::{
        ARef,
        AlwaysRefCounted, //
    },
    types::Opaque, //
};
use core::{
    cell::UnsafeCell,
    marker::PhantomData,
    mem,
    ptr::NonNull,
    slice, //
};

/// Represents a fwctl device type.
///
/// Corresponds to the C `enum fwctl_device_type`.
#[repr(u32)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum DeviceType {
    /// Mellanox ConnectX (mlx5) device.
    Mlx5 = bindings::fwctl_device_type_FWCTL_DEVICE_TYPE_MLX5,
    /// CXL (Compute Express Link) device.
    Cxl = bindings::fwctl_device_type_FWCTL_DEVICE_TYPE_CXL,
    /// AMD/Pensando PDS device.
    Pds = bindings::fwctl_device_type_FWCTL_DEVICE_TYPE_PDS,
    /// Broadcom NetXtreme (bnxt) device.
    Bnxt = bindings::fwctl_device_type_FWCTL_DEVICE_TYPE_BNXT,
}

impl From<DeviceType> for u32 {
    fn from(device_type: DeviceType) -> Self {
        device_type as u32
    }
}

/// Scope of access for an RPC request.
///
/// Corresponds to the C `enum fwctl_rpc_scope`.
#[repr(u32)]
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum RpcScope {
    /// Read/write access to device configuration.
    Configuration = bindings::fwctl_rpc_scope_FWCTL_RPC_CONFIGURATION,
    /// Read-only access to debug information.
    DebugReadOnly = bindings::fwctl_rpc_scope_FWCTL_RPC_DEBUG_READ_ONLY,
    /// Write access to lockdown-compatible debug information.
    DebugWrite = bindings::fwctl_rpc_scope_FWCTL_RPC_DEBUG_WRITE,
    /// Full read/write access to all debug information (requires `CAP_SYS_RAWIO`).
    DebugWriteFull = bindings::fwctl_rpc_scope_FWCTL_RPC_DEBUG_WRITE_FULL,
}

impl TryFrom<u32> for RpcScope {
    type Error = Error;

    #[inline]
    fn try_from(value: u32) -> Result<Self, Error> {
        match value {
            v if v == Self::Configuration as u32 => Ok(Self::Configuration),
            v if v == Self::DebugReadOnly as u32 => Ok(Self::DebugReadOnly),
            v if v == Self::DebugWrite as u32 => Ok(Self::DebugWrite),
            v if v == Self::DebugWriteFull as u32 => Ok(Self::DebugWriteFull),
            _ => Err(EINVAL),
        }
    }
}

/// Response from a [`Operations::fw_rpc`] call.
pub enum FwRpcResponse {
    /// Reuse the input buffer as the output, with the given output length.
    InPlace(usize),
    /// Return a newly allocated buffer as the output.
    NewBuffer(KVec<u8>),
}

/// Trait implemented by each Rust driver that integrates with the fwctl subsystem.
///
/// The implementing type **is** the per-FD user context: one instance is
/// created for each `open()` call and dropped when the FD is closed.
///
/// Each implementation corresponds to a specific device type and provides the
/// vtable used by the core `fwctl` layer to manage per-FD user contexts and
/// handle RPC requests.
pub trait Operations: Sized + Send + Sync + 'static {
    /// Data owned by the [`Registration`] and accessible during callbacks.
    ///
    /// The lifetime `'a` is tied to the [`Registration`] scope (which lives within the parent bus
    /// device binding scope). Drivers use it to store references to resources bound to this scope,
    /// such as PCI BARs or typed bus device references.
    type RegistrationData<'a>: Send + Sync + 'a
    where
        Self: 'a;

    /// fwctl device type identifier.
    const DEVICE_TYPE: DeviceType;

    /// Called when a new user context is opened.
    ///
    /// Returns a [`PinInit`] initializer for `Self`. The instance is dropped
    /// automatically when the FD is closed (after [`close`](Self::close)).
    fn open<'a>(
        device: &'a Device<Self>,
        reg_data: &'a Self::RegistrationData<'a>,
    ) -> impl PinInit<Self, Error>;

    /// Called when the user context is closed.
    ///
    /// The driver may perform additional cleanup here that requires access
    /// to the owning [`Device`]. `Self` is dropped automatically after this
    /// returns.
    fn close<'a>(
        _this: Pin<&mut Self>,
        _device: &'a Device<Self>,
        _reg_data: &'a Self::RegistrationData<'a>,
    ) {
    }

    /// Return device information to userspace.
    ///
    /// The default implementation returns no device-specific data.
    fn info<'a>(
        _this: Pin<&Self>,
        _device: &'a Device<Self>,
        _reg_data: &'a Self::RegistrationData<'a>,
    ) -> Result<KVec<u8>, Error> {
        Ok(KVec::new())
    }

    /// Handle a userspace RPC request.
    fn fw_rpc<'a>(
        this: Pin<&Self>,
        device: &'a Device<Self>,
        reg_data: &'a Self::RegistrationData<'a>,
        scope: RpcScope,
        rpc_in: &mut [u8],
    ) -> Result<FwRpcResponse, Error>;
}

/// A fwctl device.
///
/// `#[repr(C)]` with the `fwctl_device` at offset 0, matching the C `fwctl_alloc_device()` layout
/// convention. Contains a pointer to the [`Registration`]'s data, set at registration time and
/// cleared on unregistration.
///
/// # Invariants
///
/// - `dev` is embedded at offset 0 and is initialised by fwctl.
/// - The fwctl refcount owns the allocation lifetime.
/// - `registration_data` is either `NonNull::dangling()` (before registration / after
///   unregistration) or points to valid data owned by the [`Registration`].
#[repr(C)]
pub struct Device<T: Operations> {
    dev: Opaque<bindings::fwctl_device>,
    registration_data: UnsafeCell<NonNull<T::RegistrationData<'static>>>,
}

impl<T: Operations> Device<T> {
    /// Allocate a new fwctl device.
    ///
    /// Returns an [`ARef`] that can be passed to [`Registration::new()`]
    /// to make the device visible to userspace.
    pub fn new(parent: &device::Device<device::Bound>) -> Result<ARef<Self>> {
        const_assert!(
            core::mem::offset_of!(Self, dev) == 0,
            "struct fwctl_device must be at offset 0"
        );

        let ops = core::ptr::from_ref::<bindings::fwctl_ops>(&VTable::<T>::VTABLE).cast_mut();

        // SAFETY: `ops` is static, `parent` is bound, and `size` covers the full `Device<T>`.
        let raw = unsafe {
            bindings::_fwctl_alloc_device(parent.as_raw(), ops, core::mem::size_of::<Self>())
        };

        if raw.is_null() {
            return Err(ENOMEM);
        }

        let this = raw.cast::<Self>();

        // INVARIANT: Set `registration_data` to dangling (no registration yet).
        // SAFETY: `this` points to the allocation just returned by fwctl.
        unsafe {
            core::ptr::addr_of_mut!((*this).registration_data)
                .write(UnsafeCell::new(NonNull::dangling()));
        };

        // SAFETY: `raw` owns the initial reference.
        Ok(unsafe { ARef::from_raw(NonNull::new_unchecked(this)) })
    }

    #[inline]
    fn as_raw(&self) -> *mut bindings::fwctl_device {
        self.dev.get()
    }

    /// # Safety
    ///
    /// `ptr` must point to a valid `fwctl_device` embedded in a `Device<T>`.
    #[inline]
    unsafe fn from_raw<'a>(ptr: *mut bindings::fwctl_device) -> &'a Self {
        // SAFETY: The caller upholds the offset-0 `Device<T>` invariant.
        unsafe { &*ptr.cast() }
    }

    /// Returns a reference to the registration data.
    ///
    /// # Safety
    ///
    /// The caller must ensure the device is registered, i.e. that this is called within a fwctl
    /// callback protected by `registration_lock`. The pointer cast from
    /// `RegistrationData<'static>` to `RegistrationData<'_>` is sound because lifetimes do not
    /// affect layout, and the data is guaranteed alive for the duration of the borrow.
    #[inline]
    unsafe fn registration_data_unchecked(&self) -> &T::RegistrationData<'_> {
        // SAFETY: Caller guarantees the device is registered, so the pointer is valid.
        // Lifetimes do not affect layout, so the cast is sound.
        unsafe { (*self.registration_data.get()).cast::<_>().as_ref() }
    }
}

impl<T: Operations> AsRef<device::Device> for Device<T> {
    #[inline]
    fn as_ref(&self) -> &device::Device {
        // SAFETY: `self` contains a live fwctl_device.
        let dev = unsafe { core::ptr::addr_of_mut!((*self.as_raw()).dev) };
        // SAFETY: The embedded device is initialised by fwctl.
        unsafe { device::Device::from_raw(dev) }
    }
}

// SAFETY: `fwctl_get` increments the refcount of a valid fwctl_device.
// `fwctl_put` decrements it and frees the device when it reaches zero.
unsafe impl<T: Operations> AlwaysRefCounted for Device<T> {
    #[inline]
    fn inc_ref(&self) {
        // SAFETY: `self` holds a live reference.
        unsafe { bindings::fwctl_get(self.as_raw()) };
    }

    #[inline]
    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The caller owns a live reference.
        unsafe { bindings::fwctl_put(obj.cast().as_ptr()) };
    }
}

// SAFETY: `Device<T>` is refcounted by the fwctl core and may be released from any thread.
unsafe impl<T: Operations> Send for Device<T> {}

// SAFETY: Shared access to the embedded `fwctl_device` is protected by the fwctl core. The
// `registration_data` field is only mutated before registration and after unregistration (both
// single-threaded with respect to callbacks).
unsafe impl<T: Operations> Sync for Device<T> {}

/// A registered fwctl device.
///
/// Carries the lifetime `'a` of the parent device to ensure that [`fwctl_unregister`] runs (via
/// [`Drop`]) before the parent driver unbinds. Owns the
/// [`RegistrationData`](Operations::RegistrationData) which is accessible during callbacks via the
/// pointer stored in [`Device`].
///
/// On drop the device is unregistered (all user contexts are closed and `ops` is set to `NULL`)
/// and the registration data is dropped.
///
/// [`fwctl_unregister`]: srctree/drivers/fwctl/main.c
pub struct Registration<'a, T: Operations> {
    dev: ARef<Device<T>>,
    _reg_data: Pin<KBox<T::RegistrationData<'a>>>,
}

impl<'a, T: Operations> Registration<'a, T> {
    /// Register a previously allocated fwctl device with the given registration data.
    ///
    /// The `reg_data` is owned by the registration and accessible during callbacks via
    /// `Device::registration_data_unchecked()`.
    ///
    /// # Safety
    ///
    /// Callers must not `mem::forget()` the returned [`Registration`] or otherwise prevent its
    /// [`Drop`] implementation from running, since `fwctl_unregister` must be called before the
    /// parent device is unbound.
    ///
    /// `dev` must be an unregistered [`Device`] that is not associated with any live
    /// [`Registration`], and no other thread may attempt to register the same device concurrently.
    pub unsafe fn new(
        _parent: &'a device::Device<device::Bound>,
        dev: &Device<T>,
        reg_data: impl PinInit<T::RegistrationData<'a>, Error>,
    ) -> Result<Self> {
        let reg_data: Pin<KBox<T::RegistrationData<'a>>> = KBox::pin_init(reg_data, GFP_KERNEL)?;

        // Store the registration data pointer in the device before registration, so that it is
        // visible once callbacks can be invoked.
        //
        // SAFETY: Lifetimes do not affect layout, so the pointer cast is sound.
        let ptr: NonNull<T::RegistrationData<'static>> =
            unsafe { mem::transmute(NonNull::from(Pin::get_ref(reg_data.as_ref()))) };

        // SAFETY: No concurrent access; the device is not yet registered.
        unsafe { *dev.registration_data.get() = ptr };

        // SAFETY: `dev` is a valid fwctl_device backed by an ARef.
        let ret = unsafe { bindings::fwctl_register(dev.as_raw()) };
        if ret != 0 {
            // SAFETY: No concurrent readers; registration failed.
            unsafe { *dev.registration_data.get() = NonNull::dangling() };
            return Err(Error::from_errno(ret));
        }

        Ok(Self {
            dev: dev.into(),
            _reg_data: reg_data,
        })
    }
}

impl<T: Operations> Drop for Registration<'_, T> {
    fn drop(&mut self) {
        // SAFETY: The Registration lifetime guarantees that the parent device is still bound.
        // `fwctl_unregister` takes the write lock, closes all user contexts, and sets ops=NULL.
        // After it returns, no callbacks can be running or will run.
        unsafe { bindings::fwctl_unregister(self.dev.as_raw()) };

        // SAFETY: `fwctl_unregister` guarantees no concurrent readers.
        unsafe { *self.dev.registration_data.get() = NonNull::dangling() };

        // `self._reg_data` is dropped here, after callbacks have stopped.
    }
}

// SAFETY: `Registration` can be sent between threads; the underlying fwctl_device uses internal
// locking.
unsafe impl<T: Operations> Send for Registration<'_, T> {}

// SAFETY: `Registration` provides no mutable access; the underlying fwctl_device is protected by
// internal locking.
unsafe impl<T: Operations> Sync for Registration<'_, T> {}

/// Internal per-FD user context wrapping `struct fwctl_uctx` and `T`.
///
/// Not exposed to drivers; they work with `&T` / `Pin<&mut T>` directly.
#[repr(C)]
#[pin_data]
struct UserCtx<T: Operations> {
    #[pin]
    fwctl_uctx: Opaque<bindings::fwctl_uctx>,
    #[pin]
    uctx: T,
}

impl<T: Operations> UserCtx<T> {
    /// # Safety
    ///
    /// `ptr` must point to a `fwctl_uctx` embedded in a live `UserCtx<T>`.
    #[inline]
    unsafe fn from_raw<'a>(ptr: *mut bindings::fwctl_uctx) -> &'a Self {
        // SAFETY: The caller upholds the `UserCtx<T>` embedding invariant.
        unsafe { &*container_of!(Opaque::cast_from(ptr), Self, fwctl_uctx) }
    }

    /// # Safety
    ///
    /// `ptr` must point to a `fwctl_uctx` embedded in a live `UserCtx<T>`.
    /// The caller must ensure exclusive access to the `UserCtx<T>`.
    #[inline]
    unsafe fn from_raw_mut<'a>(ptr: *mut bindings::fwctl_uctx) -> &'a mut Self {
        // SAFETY: The caller upholds the embedding and exclusivity invariants.
        unsafe { &mut *container_of!(Opaque::cast_from(ptr), Self, fwctl_uctx).cast_mut() }
    }

    /// Returns a reference to the fwctl [`Device`] that owns this context.
    #[inline]
    fn device(&self) -> &Device<T> {
        // SAFETY: fwctl initialises this pointer before any driver callback.
        let raw_fwctl = unsafe { (*self.fwctl_uctx.get()).fwctl };
        // SAFETY: Rust fwctl devices use the offset-0 `Device<T>` layout.
        unsafe { Device::from_raw(raw_fwctl) }
    }
}

/// Static vtable mapping Rust trait methods to C callbacks.
pub struct VTable<T: Operations>(PhantomData<T>);

impl<T: Operations> VTable<T> {
    /// The fwctl operations vtable for this driver type.
    pub const VTABLE: bindings::fwctl_ops = bindings::fwctl_ops {
        device_type: T::DEVICE_TYPE as u32,
        uctx_size: core::mem::size_of::<UserCtx<T>>(),
        open_uctx: Some(Self::open_uctx_callback),
        close_uctx: Some(Self::close_uctx_callback),
        info: Some(Self::info_callback),
        fw_rpc: Some(Self::fw_rpc_callback),
    };

    /// # Safety
    ///
    /// `uctx` must be a valid `fwctl_uctx` embedded in a `UserCtx<T>` with
    /// sufficient allocated space for the uctx field.
    unsafe extern "C" fn open_uctx_callback(uctx: *mut bindings::fwctl_uctx) -> ffi::c_int {
        const_assert!(
            core::mem::offset_of!(UserCtx<T>, fwctl_uctx) == 0,
            "struct fwctl_uctx must be at offset 0"
        );

        // SAFETY: fwctl sets this pointer before calling `open_uctx`.
        let raw_fwctl = unsafe { (*uctx).fwctl };
        // SAFETY: Rust fwctl devices use the offset-0 `Device<T>` layout.
        let device = unsafe { Device::<T>::from_raw(raw_fwctl) };

        // SAFETY: open_uctx is called under registration_lock read; the device is registered.
        let reg_data = unsafe { device.registration_data_unchecked() };

        let initializer = T::open(device, reg_data);

        let uctx_offset = core::mem::offset_of!(UserCtx<T>, uctx);
        // SAFETY: `uctx_size` reserves space for the full `UserCtx<T>`.
        let uctx_ptr: *mut T = unsafe { uctx.cast::<u8>().add(uctx_offset).cast() };

        // SAFETY: `uctx_ptr` addresses the uninitialised pinned context.
        match unsafe { initializer.__pinned_init(uctx_ptr.cast()) } {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `uctx` must point to a fully initialised `UserCtx<T>`.
    unsafe extern "C" fn close_uctx_callback(uctx: *mut bindings::fwctl_uctx) {
        // SAFETY: fwctl keeps the owning device live for this callback.
        let device = unsafe { Device::<T>::from_raw((*uctx).fwctl) };

        // SAFETY: close_uctx is called under registration_lock write (from fwctl_unregister) or
        // under registration_lock read (from fwctl_fops_release); the device is registered.
        let reg_data = unsafe { device.registration_data_unchecked() };

        // SAFETY: close is called for an opened Rust user context.
        let ctx = unsafe { UserCtx::<T>::from_raw_mut(uctx) };

        {
            // SAFETY: fwctl never moves an opened user context.
            let pinned = unsafe { Pin::new_unchecked(&mut ctx.uctx) };
            T::close(pinned, device, reg_data);
        }

        // SAFETY: close is the last callback before fwctl frees the allocation.
        unsafe { core::ptr::drop_in_place(&mut ctx.uctx) };
    }

    /// # Safety
    ///
    /// `uctx` must point to a fully initialised `UserCtx<T>`.
    /// `length` must be a valid pointer.
    unsafe extern "C" fn info_callback(
        uctx: *mut bindings::fwctl_uctx,
        length: *mut usize,
    ) -> *mut ffi::c_void {
        // SAFETY: info is called for an opened Rust user context.
        let ctx = unsafe { UserCtx::<T>::from_raw(uctx) };
        let device = ctx.device();

        // SAFETY: info is called under registration_lock read; the device is registered.
        let reg_data = unsafe { device.registration_data_unchecked() };

        // SAFETY: fwctl never moves an opened user context.
        let pinned = unsafe { Pin::new_unchecked(&ctx.uctx) };

        match T::info(pinned, device, reg_data) {
            Ok(kvec) if kvec.is_empty() => {
                // SAFETY: `length` is a valid out-parameter.
                unsafe { *length = 0 };
                // Return NULL for empty data; kfree(NULL) is safe.
                core::ptr::null_mut()
            }
            Ok(kvec) => {
                let (ptr, len, _cap) = kvec.into_raw_parts();
                // SAFETY: `length` is a valid out-parameter.
                unsafe { *length = len };
                ptr.cast::<ffi::c_void>()
            }
            Err(e) => Error::to_ptr(e),
        }
    }

    /// # Safety
    ///
    /// `uctx` must point to a fully initialised `UserCtx<T>`.
    /// `rpc_in` must be valid for `in_len` bytes. `out_len` must be valid.
    unsafe extern "C" fn fw_rpc_callback(
        uctx: *mut bindings::fwctl_uctx,
        scope: u32,
        rpc_in: *mut ffi::c_void,
        in_len: usize,
        out_len: *mut usize,
    ) -> *mut ffi::c_void {
        let scope = match RpcScope::try_from(scope) {
            Ok(s) => s,
            Err(e) => return Error::to_ptr(e),
        };

        // SAFETY: RPC is called for an opened Rust user context.
        let ctx = unsafe { UserCtx::<T>::from_raw(uctx) };
        let device = ctx.device();

        // SAFETY: fw_rpc is called under registration_lock read; the device is registered.
        let reg_data = unsafe { device.registration_data_unchecked() };

        // SAFETY: fwctl passes a valid in/out buffer for this callback.
        let rpc_in_slice: &mut [u8] =
            unsafe { slice::from_raw_parts_mut(rpc_in.cast::<u8>(), in_len) };

        // SAFETY: fwctl never moves an opened user context.
        let pinned = unsafe { Pin::new_unchecked(&ctx.uctx) };

        match T::fw_rpc(pinned, device, reg_data, scope, rpc_in_slice) {
            Ok(FwRpcResponse::InPlace(len)) => {
                if len > in_len {
                    return Error::to_ptr(EINVAL);
                }

                // SAFETY: `out_len` is valid.
                unsafe { *out_len = len };
                rpc_in
            }
            Ok(FwRpcResponse::NewBuffer(kvec)) => {
                let (ptr, len, _cap) = kvec.into_raw_parts();
                // SAFETY: `out_len` is valid.
                unsafe { *out_len = len };
                ptr.cast::<ffi::c_void>()
            }
            Err(e) => Error::to_ptr(e),
        }
    }
}
