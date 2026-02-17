// SPDX-License-Identifier: GPL-2.0-only

//! Abstractions for the fwctl subsystem.
//!
//! C header: [`include/linux/fwctl.h`]

use crate::{
    bindings,
    container_of,
    device,
    devres::Devres,
    prelude::*,
    sync::aref::{
        ARef,
        AlwaysRefCounted, //
    },
    types::Opaque, //
};
use core::{
    marker::PhantomData,
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

    fn try_from(value: u32) -> Result<Self, Error> {
        match value {
            v if v == Self::Configuration as u32 => Ok(Self::Configuration),
            v if v == Self::DebugReadOnly as u32 => Ok(Self::DebugReadOnly),
            v if v == Self::DebugWrite as u32 => Ok(Self::DebugWrite),
            v if v == Self::DebugWriteFull as u32 => Ok(Self::DebugWriteFull),
            _ => Err(ENOTSUPP),
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
pub trait Operations: Sized {
    /// Driver data embedded alongside the `fwctl_device` allocation.
    type DeviceData;

    /// fwctl device type identifier.
    const DEVICE_TYPE: DeviceType;

    /// Called when a new user context is opened.
    ///
    /// Returns a [`PinInit`] initializer for `Self`. The instance is dropped
    /// automatically when the FD is closed (after [`close`](Self::close)).
    fn open(device: &Device<Self>) -> Result<impl PinInit<Self, Error>, Error>;

    /// Called when the user context is closed.
    ///
    /// The driver may perform additional cleanup here that requires access
    /// to the owning [`Device`]. `Self` is dropped automatically after this
    /// returns.
    fn close(_this: Pin<&mut Self>, _device: &Device<Self>) {}

    /// Return device information to userspace.
    ///
    /// The default implementation returns no device-specific data.
    fn info(_this: &Self, _device: &Device<Self>) -> Result<KVec<u8>, Error> {
        Ok(KVec::new())
    }

    /// Handle a userspace RPC request.
    fn fw_rpc(
        this: &Self,
        device: &Device<Self>,
        scope: RpcScope,
        rpc_in: &mut [u8],
    ) -> Result<FwRpcResponse, Error>;
}

/// A fwctl device with embedded driver data.
///
/// `#[repr(C)]` with the `fwctl_device` at offset 0, matching the C
/// `fwctl_alloc_device()` layout convention.
///
/// # Invariants
///
/// The `fwctl_device` portion is initialised by the C subsystem during
/// [`Device::new()`]. The `data` field is initialised in-place via
/// [`PinInit`] before the struct is exposed.
#[repr(C)]
pub struct Device<T: Operations> {
    dev: Opaque<bindings::fwctl_device>,
    data: T::DeviceData,
}

impl<T: Operations> Device<T> {
    /// Allocate a new fwctl device with embedded driver data.
    ///
    /// Returns an [`ARef`] that can be passed to [`Registration::new()`]
    /// to make the device visible to userspace. The caller may inspect or
    /// configure the device between allocation and registration.
    pub fn new(
        parent: &device::Device<device::Bound>,
        data: impl PinInit<T::DeviceData, Error>,
    ) -> Result<ARef<Self>> {
        let ops = core::ptr::from_ref::<bindings::fwctl_ops>(&VTable::<T>::VTABLE).cast_mut();

        // SAFETY: `_fwctl_alloc_device` allocates `size` bytes via kzalloc and
        // initialises the embedded fwctl_device. `ops` points to a static vtable
        // that outlives the device. `parent` is bound.
        let raw = unsafe {
            bindings::_fwctl_alloc_device(parent.as_raw(), ops, core::mem::size_of::<Self>())
        };

        if raw.is_null() {
            return Err(ENOMEM);
        }

        // CAST: Device<T> is repr(C) with fwctl_device at offset 0.
        let this = raw as *mut Self;

        // SAFETY: `data` field is within the kzalloc'd allocation, uninitialised.
        let data_ptr = unsafe { core::ptr::addr_of_mut!((*this).data) };
        unsafe { data.__pinned_init(data_ptr) }.inspect_err(|_| {
            // SAFETY: Init failed; release the allocation.
            unsafe { bindings::fwctl_put(raw) };
        })?;

        // SAFETY: `_fwctl_alloc_device` returned a valid pointer with refcount 1
        // and DeviceData is fully initialised.
        Ok(unsafe { ARef::from_raw(NonNull::new_unchecked(raw as *mut Self)) })
    }

    /// Returns a reference to the embedded driver data.
    pub fn data(&self) -> &T::DeviceData {
        &self.data
    }

    fn as_raw(&self) -> *mut bindings::fwctl_device {
        self.dev.get()
    }

    /// # Safety
    ///
    /// `ptr` must point to a valid `fwctl_device` embedded in a `Device<T>`.
    unsafe fn from_raw<'a>(ptr: *mut bindings::fwctl_device) -> &'a Self {
        unsafe { &*ptr.cast() }
    }

    /// Returns the parent device.
    ///
    /// The parent is guaranteed to be bound while any fwctl callback is
    /// running (ensured by the `registration_lock` read lock on the ioctl
    /// path and by `Devres` on the teardown path).
    pub fn parent(&self) -> &device::Device<device::Bound> {
        // SAFETY: fwctl_device always has a valid parent.
        let parent_dev = unsafe { (*self.as_raw()).dev.parent };
        let dev: &device::Device = unsafe { device::Device::from_raw(parent_dev) };
        // SAFETY: The parent is guaranteed to be bound while fwctl ops are active.
        unsafe { dev.as_bound() }
    }
}

impl<T: Operations> AsRef<device::Device> for Device<T> {
    fn as_ref(&self) -> &device::Device {
        // SAFETY: self.as_raw() is a valid fwctl_device.
        let dev = unsafe { core::ptr::addr_of_mut!((*self.as_raw()).dev) };
        // SAFETY: dev points to a valid struct device.
        unsafe { device::Device::from_raw(dev) }
    }
}

// SAFETY: `fwctl_get` increments the refcount of a valid fwctl_device.
// `fwctl_put` decrements it and frees the device when it reaches zero.
unsafe impl<T: Operations> AlwaysRefCounted for Device<T> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::fwctl_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::fwctl_put(obj.cast().as_ptr()) };
    }
}

/// A registered fwctl device.
///
/// Must live inside a [`Devres`] to guarantee that [`fwctl_unregister`] runs
/// before the parent driver unbinds. `Devres` prevents the `Registration`
/// from being moved to a context that could outlive the parent device.
///
/// On drop the device is unregistered (all user contexts are closed and
/// `ops` is set to `NULL`) and the [`ARef`] is released.
///
/// [`fwctl_unregister`]: srctree/drivers/fwctl/main.c
pub struct Registration<T: Operations> {
    dev: ARef<Device<T>>,
}

impl<T: Operations> Registration<T> {
    /// Register a previously allocated fwctl device.
    pub fn new<'a>(
        parent: &'a device::Device<device::Bound>,
        dev: &'a Device<T>,
    ) -> impl PinInit<Devres<Self>, Error> + 'a {
        pin_init::pin_init_scope(move || {
            // SAFETY: `dev` is a valid fwctl_device backed by an ARef.
            let ret = unsafe { bindings::fwctl_register(dev.as_raw()) };
            if ret != 0 {
                return Err(Error::from_errno(ret));
            }

            Ok(Devres::new(parent, Self { dev: dev.into() }))
        })
    }
}

impl<T: Operations> Drop for Registration<T> {
    fn drop(&mut self) {
        // SAFETY: `Registration` lives inside a `Devres`, which guarantees
        // that drop runs while the parent device is still bound.
        unsafe { bindings::fwctl_unregister(self.dev.as_raw()) };
        // ARef<Device<T>> is dropped after this, calling fwctl_put.
    }
}

// SAFETY: `Registration` can be sent between threads; the underlying
// fwctl_device uses internal locking.
unsafe impl<T: Operations> Send for Registration<T> {}

// SAFETY: `Registration` provides no mutable access; the underlying
// fwctl_device is protected by internal locking.
unsafe impl<T: Operations> Sync for Registration<T> {}

/// Internal per-FD user context wrapping `struct fwctl_uctx` and `T`.
///
/// Not exposed to drivers — they work with `&T` / `Pin<&mut T>` directly.
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
    unsafe fn from_raw<'a>(ptr: *mut bindings::fwctl_uctx) -> &'a Self {
        unsafe { &*container_of!(Opaque::cast_from(ptr), Self, fwctl_uctx) }
    }

    /// # Safety
    ///
    /// `ptr` must point to a `fwctl_uctx` embedded in a live `UserCtx<T>`.
    /// The caller must ensure exclusive access to the `UserCtx<T>`.
    unsafe fn from_raw_mut<'a>(ptr: *mut bindings::fwctl_uctx) -> &'a mut Self {
        unsafe { &mut *container_of!(Opaque::cast_from(ptr), Self, fwctl_uctx).cast_mut() }
    }

    /// Returns a reference to the fwctl [`Device`] that owns this context.
    fn device(&self) -> &Device<T> {
        // SAFETY: fwctl_uctx.fwctl is set by the subsystem and always valid.
        let raw_fwctl = unsafe { (*self.fwctl_uctx.get()).fwctl };
        // SAFETY: The fwctl_device is embedded in a Device<T>.
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
        // SAFETY: `fwctl_uctx.fwctl` is set by the subsystem before calling open.
        let raw_fwctl = unsafe { (*uctx).fwctl };
        // SAFETY: `raw_fwctl` points to a valid fwctl_device embedded in a Device<T>.
        let device = unsafe { Device::<T>::from_raw(raw_fwctl) };

        let initializer = match T::open(device) {
            Ok(init) => init,
            Err(e) => return e.to_errno(),
        };

        let uctx_offset = core::mem::offset_of!(UserCtx<T>, uctx);
        // SAFETY: The C side allocated space for the entire UserCtx<T>.
        let uctx_ptr: *mut T = unsafe { uctx.cast::<u8>().add(uctx_offset).cast() };

        // SAFETY: uctx_ptr points to uninitialised, properly aligned memory.
        match unsafe { initializer.__pinned_init(uctx_ptr.cast()) } {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `uctx` must point to a fully initialised `UserCtx<T>`.
    unsafe extern "C" fn close_uctx_callback(uctx: *mut bindings::fwctl_uctx) {
        // SAFETY: `fwctl_uctx.fwctl` is set by the subsystem and always valid.
        let device = unsafe { Device::<T>::from_raw((*uctx).fwctl) };

        // SAFETY: uctx is a valid pointer promised by C side.
        let ctx = unsafe { UserCtx::<T>::from_raw_mut(uctx) };

        {
            // SAFETY: driver uctx is pinned in place by the C allocation.
            let pinned = unsafe { Pin::new_unchecked(&mut ctx.uctx) };
            T::close(pinned, device);
        }

        // SAFETY: After close returns, no further callbacks will access UserCtx.
        // Drop T before the C side kfree's the allocation.
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
        // SAFETY: uctx is a valid pointer promised by C side.
        let ctx = unsafe { UserCtx::<T>::from_raw(uctx) };
        let device = ctx.device();

        match T::info(&ctx.uctx, device) {
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

        // SAFETY: `uctx` is fully initialised; shared access is sufficient.
        let ctx = unsafe { UserCtx::<T>::from_raw(uctx) };
        let device = ctx.device();

        // SAFETY: `rpc_in` / `in_len` are guaranteed valid by the fwctl subsystem.
        let rpc_in_slice: &mut [u8] =
            unsafe { slice::from_raw_parts_mut(rpc_in.cast::<u8>(), in_len) };

        match T::fw_rpc(&ctx.uctx, device, scope, rpc_in_slice) {
            Ok(FwRpcResponse::InPlace(len)) => {
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
