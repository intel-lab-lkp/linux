// SPDX-License-Identifier: GPL-2.0

//! ARM AMBA bus abstractions.
//!
//! C header: [`include/linux/amba/bus.h`](srctree/include/linux/amba/bus.h)

use crate::{
    bindings,
    container_of,
    device,
    device_id::{
        RawDeviceId,
        RawDeviceIdIndex, //
    },
    io::{
        mem::IoRequest,
        resource::Resource, //
    },
    irq::{
        self,
        IrqRequest, //
    },
    prelude::*,
    sync::aref::AlwaysRefCounted,
    types::Opaque, //
};
use core::{
    marker::PhantomData,
    ptr::NonNull, //
};

/// Device ID table type for AMBA drivers.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// AMBA device identifier.
///
/// Wraps the C `struct amba_id` from `include/linux/amba/bus.h`.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(pub(crate) bindings::amba_id);

// SAFETY: `DeviceId` is a transparent wrapper over `amba_id` with no additional
// invariants.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::amba_id;
}

// SAFETY: The offset matches the `data` field in `struct amba_id`.
unsafe impl RawDeviceIdIndex for DeviceId {
    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::amba_id, data);

    fn index(&self) -> usize {
        self.0.data as usize
    }
}

impl DeviceId {
    /// Creates a new device ID from an AMBA device ID and mask.
    ///
    /// A driver binds to a device when `(hardware_device_id & mask) == id`.
    #[inline(always)]
    pub const fn new(id: u32, mask: u32) -> Self {
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut amba: bindings::amba_id = unsafe { core::mem::zeroed() };
        amba.id = id;
        amba.mask = mask;
        amba.data = core::ptr::null_mut();

        Self(amba)
    }

    /// Creates a new device ID with driver-specific data.
    #[inline(always)]
    pub const fn new_with_data(id: u32, mask: u32, data: usize) -> Self {
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut amba: bindings::amba_id = unsafe { core::mem::zeroed() };
        amba.id = id;
        amba.mask = mask;
        amba.data = data as *mut core::ffi::c_void;

        Self(amba)
    }

    /// Returns the device ID.
    #[inline(always)]
    pub const fn id(&self) -> u32 {
        self.0.id
    }

    /// Returns the device ID mask.
    #[inline(always)]
    pub const fn mask(&self) -> u32 {
        self.0.mask
    }
}

/// Creates an AMBA device ID table with a module alias for modpost.
#[macro_export]
macro_rules! amba_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::amba::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("amba", $module_table_name, $table_name);
    };
}

/// An AMBA device.
///
/// Wraps the C `struct amba_device` from `include/linux/amba/bus.h`.
pub struct Device<Ctx: device::DeviceContext = device::Normal>(
    Opaque<bindings::amba_device>,
    PhantomData<Ctx>,
);

impl<Ctx: device::DeviceContext> Device<Ctx> {
    /// Obtain the raw `struct amba_device` pointer.
    pub fn as_raw(&self) -> *mut bindings::amba_device {
        self.0.get()
    }

    /// Returns the memory resource.
    pub fn resource(&self) -> Option<&Resource> {
        // SAFETY: `self.as_raw()` returns a valid pointer to a `struct amba_device`.
        let resource = unsafe { &raw mut (*self.as_raw()).res };
        // SAFETY: `resource` is a valid pointer to a `struct resource`.
        Some(unsafe { Resource::from_raw(resource) })
    }
}

impl<Ctx: device::DeviceContext> AsRef<device::Device<Ctx>> for Device<Ctx> {
    fn as_ref(&self) -> &device::Device<Ctx> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a
        // valid `struct amba_device`.
        let dev = unsafe { &raw mut (*self.as_raw()).dev };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(dev) }
    }
}

// SAFETY: `Device` is a transparent wrapper that doesn't depend on its generic
// argument.
crate::impl_device_context_deref!(unsafe { Device });
crate::impl_device_context_into_aref!(Device);

impl<Ctx: device::DeviceContext> TryFrom<&device::Device<Ctx>> for &Device<Ctx> {
    type Error = kernel::error::Error;

    fn try_from(dev: &device::Device<Ctx>) -> Result<Self, Self::Error> {
        // SAFETY: By the type invariant of `Device`, `dev.as_raw()` is a valid pointer
        // to a `struct device`.
        if !unsafe { bindings::dev_is_amba(dev.as_raw()) } {
            return Err(crate::error::code::EINVAL);
        }

        // SAFETY: We've just verified that the bus type of `dev` equals
        // `bindings::amba_bustype`, hence `dev` must be embedded in a valid
        // `struct amba_device` as guaranteed by the corresponding C code.
        let adev = unsafe { container_of!(dev.as_raw(), bindings::amba_device, dev) };

        // SAFETY: `adev` is a valid pointer to a `struct amba_device`.
        Ok(unsafe { &*adev.cast() })
    }
}

impl Device<device::Core> {}

impl Device<device::Bound> {
    /// Returns an [`IoRequest`] for the memory resource.
    pub fn io_request(&self) -> Option<IoRequest<'_>> {
        self.resource()
            // SAFETY: `resource` is valid for the lifetime of the `IoRequest`.
            .map(|resource| unsafe { IoRequest::new(self.as_ref(), resource) })
    }

    /// Returns an [`IrqRequest`] for the IRQ at the given index.
    pub fn irq_by_index(&self, index: u32) -> Result<IrqRequest<'_>> {
        if index >= bindings::AMBA_NR_IRQS {
            return Err(crate::error::code::EINVAL);
        }

        // SAFETY: `self.as_raw()` returns a valid pointer to a `struct amba_device`.
        let irq = unsafe { (*self.as_raw()).irq[index as usize] };

        if irq == 0 {
            return Err(crate::error::code::ENXIO);
        }

        // SAFETY: `irq` is guaranteed to be a valid IRQ number for `&self`.
        Ok(unsafe { IrqRequest::new(self.as_ref(), irq) })
    }

    /// Requests an IRQ at the given index and returns a [`irq::Registration`].
    pub fn request_irq_by_index<'a, T: irq::Handler + 'static>(
        &'a self,
        flags: irq::Flags,
        index: u32,
        name: &'static CStr,
        handler: impl PinInit<T, Error> + 'a,
    ) -> Result<impl PinInit<irq::Registration<T>, Error> + 'a> {
        let request = self.irq_by_index(index)?;

        Ok(irq::Registration::<T>::new(request, flags, name, handler))
    }
}

// SAFETY: `Device` instances are always reference-counted via the underlying
// `device`.
unsafe impl AlwaysRefCounted for Device {
    fn inc_ref(&self) {
        // SAFETY: A shared reference guarantees the refcount is non-zero.
        unsafe { bindings::get_device(self.as_ref().as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // Use `put_device` directly since `amba_device_put` is just a wrapper
        // around it.
        let adev: *mut bindings::amba_device = obj.cast().as_ptr();
        // SAFETY: `amba_device` contains `device` as its first field.
        let dev: *mut bindings::device = unsafe { &raw mut (*adev).dev };
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::put_device(dev) }
    }
}

// SAFETY: `Device` is reference-counted and can be released from any thread.
unsafe impl Send for Device {}

// SAFETY: All methods of `Device` (i.e., `Device<Normal>`) are thread-safe.
unsafe impl Sync for Device {}
