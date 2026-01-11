// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the WMI devices.
//!
//! C header: [`include/linux/wmi.h`](srctree/include/linux/wmi.h).

use crate::{
    acpi::AcpiObject,
    device,
    device_id::{
        RawDeviceId,
        RawDeviceIdIndex, //
    },
    driver,
    error::{
        from_result,
        to_result,
        VTABLE_DEFAULT_ERROR, //
    },
    prelude::*,
    types::Opaque, //
};
use core::{
    marker::PhantomData,
    ptr::NonNull, //
};
use macros::vtable;

/// [`IdTable`](kernel::device_id::IdTable) type for WMI.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// The WMI driver trait.
///
/// Driver can be called from arbitary thread without any ordering guarantees.
#[vtable]
pub trait Driver: Send + Sync {
    /// The type holding information about each one of the device ids supported by the driver.
    type IdInfo: 'static;

    /// The table of device ids supported by the driver.
    const TABLE: IdTable<Self::IdInfo>;

    /// WMI driver probe.
    ///
    /// Called when a new WMI device is bound to this driver.
    /// Implementers should attempt to initialize the driver here.
    fn probe(dev: &Device<device::Core>, id_info: &Self::IdInfo) -> impl PinInit<Self, Error>;

    /// WMI device notify.
    ///
    /// Called when new WMI event received from bounded device.
    fn notify(self: Pin<&Self>, _dev: &Device<device::Bound>, _event: Option<&AcpiObject<'_>>) {
        build_error!(VTABLE_DEFAULT_ERROR);
    }

    /// WMI driver remove.
    ///
    /// Called when the WMI driver is unbound from a WMI device.
    fn unbind(self: Pin<&Self>, _dev: &Device<device::Core>) {
        build_error!(VTABLE_DEFAULT_ERROR);
    }
}

/// A WMI device.
///
/// This structure represents the Rust abstraction for a C [`struct wmi_device`].
/// The implementation abstracts the usage of a C [`struct wmi_device`] passed
/// in from the C side.
pub struct Device<Cxt: device::DeviceContext = device::Normal> {
    inner: Opaque<bindings::wmi_device>,
    _p: PhantomData<Cxt>,
}

impl<Cxt: device::DeviceContext> Device<Cxt> {
    fn as_raw(&self) -> *mut bindings::wmi_device {
        self.inner.get()
    }
}

/// An adapter for the registration of WMI drivers.
pub struct Adapter<T: Driver>(T);

// SAFETY: A call to `unregister` for a given instance of `RegType` is guaranteed to be valid if
// a preceding call to `register` has been successful.
unsafe impl<T: Driver + 'static> driver::RegistrationOps for Adapter<T> {
    type RegType = bindings::wmi_driver;

    unsafe fn register(
        wdrv: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result {
        macro_rules! map_callback {
            ($flag:ident -> $callback:ident) => {
                if T::$flag {
                    Some(Self::$callback)
                } else {
                    None
                }
            };
        }

        // SAFETY: It's safe to set the fields of `struct wmi_driver` on initialization.
        unsafe {
            (*wdrv.get()).driver.name = name.as_char_ptr();
            (*wdrv.get()).driver.probe_type = bindings::probe_type_PROBE_PREFER_ASYNCHRONOUS;
            (*wdrv.get()).id_table = T::TABLE.as_ptr();
            (*wdrv.get()).probe = map_callback!(HAS_PROBE -> probe_callback);
            (*wdrv.get()).notify = map_callback!(HAS_NOTIFY -> notify_callback);
            (*wdrv.get()).remove = map_callback!(HAS_UNBIND -> remove_callback);
            (*wdrv.get()).shutdown = None;
            (*wdrv.get()).no_singleton = true;
            (*wdrv.get()).no_notify_data = true;
        }

        // SAFETY: `wdrv` is guaranteed to be a valid `RegType`.
        to_result(unsafe { bindings::__wmi_driver_register(wdrv.get(), module.as_ptr()) })
    }

    unsafe fn unregister(wdrv: &Opaque<Self::RegType>) {
        // SAFETY: `wdrv` is guaranteed to be a valid `RegType`.
        unsafe { bindings::wmi_driver_unregister(wdrv.get()) };
    }
}

impl<T: Driver + 'static> Adapter<T> {
    extern "C" fn probe_callback(
        wdev: *mut bindings::wmi_device,
        id: *const c_void,
    ) -> kernel::ffi::c_int {
        // SAFETY: The WMI core only ever calls the probe callback with a valid pointer to a
        // `struct wmi_device`.
        //
        // INVARIANT: `wdev` is valid for the duration of `probe_callback()`.
        let wdev = unsafe { &*wdev.cast::<Device<device::CoreInternal>>() };

        let id = id as usize;
        let info = T::TABLE.info(id);

        from_result(|| {
            let data = T::probe(wdev, info);

            wdev.as_ref().set_drvdata(data)?;
            Ok(0)
        })
    }

    extern "C" fn notify_callback(
        wdev: *mut bindings::wmi_device,
        obj: *mut bindings::acpi_object,
    ) {
        // SAFETY: The WMI system only ever calls the notify callback with a valid pointer to a
        // `struct wmi_device`.
        let wdev = unsafe { &*wdev.cast::<Device<device::CoreInternal>>() };
        // SAFETY:
        // - AcpiObject is repr(transparent) wrapper around FFI object, so it's safe to cast
        //    raw pointer to reference (in terms of alignment and etc),
        // - Option<&ref> is guaranteed to have same layout as raw pointer (with NULL representing
        //    None) by Rust's "nullable pointer optimization".
        let obj: Option<&AcpiObject<'_>> =
            unsafe { core::mem::transmute(obj as *const AcpiObject<'_>) };

        // SAFETY: `notify_callback` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `T`.
        let this = unsafe { wdev.as_ref().drvdata_borrow::<T>() };
        this.notify(wdev, obj);
    }

    extern "C" fn remove_callback(wdev: *mut bindings::wmi_device) {
        // SAFETY: The WMI system only ever calls the remove callback with a valid pointer to a
        // `struct wmi_device`.
        let wdev = unsafe { &*wdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `remove_callback` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `T`.
        let this = unsafe { wdev.as_ref().drvdata_borrow::<T>() };
        this.unbind(wdev);
    }
}

impl<Ctx: device::DeviceContext> AsRef<device::Device<Ctx>> for Device<Ctx> {
    fn as_ref(&self) -> &device::Device<Ctx> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct platform_device`.
        let dev = unsafe { &raw mut (*self.inner.get()).dev };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(dev) }
    }
}

// SAFETY: `Device` is a transparent wrapper of a type that doesn't depend on `Device`'s generic
// argument.
kernel::impl_device_context_deref!(unsafe { Device });
kernel::impl_device_context_into_aref!(Device);

// SAFETY: Instances of `Device` are always reference-counted.
unsafe impl crate::sync::aref::AlwaysRefCounted for Device {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::get_device(self.as_ref().as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::put_device(&raw mut (*obj.as_ref().as_raw()).dev) }
    }
}

/// Abstraction for the WMI device ID structure, i.e. [`struct wmi_device_id`].
///
/// [`struct wmi_device_id`]: https://docs.kernel.org/driver-api/basics.html#c.wmi_device_id
#[repr(transparent)]
pub struct DeviceId(bindings::wmi_device_id);

impl DeviceId {
    const GUID_LEN: usize = bindings::UUID_STRING_LEN as usize;

    /// Constructs new DeviceId from GUID string.
    pub const fn new(guid: &[u8; Self::GUID_LEN]) -> Self {
        let mut inner: bindings::wmi_device_id = pin_init::zeroed();

        build_assert!(inner.guid_string.len() == Self::GUID_LEN + 1);

        // We are copying UUID_STRING_LEN bytes and we verified that UUID_STRING_LEN + 1 byte
        // exists and will remain '\0'. So we will construct valid C string.
        let mut i = 0;
        while i < Self::GUID_LEN {
            inner.guid_string[i] = guid[i];
            i += 1;
        }

        Self(inner)
    }
}

// SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `wmi_device_id` and does not add
// additional invariants, so it's safe to transmute to `RawType`.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::wmi_device_id;
}

// SAFETY: `DRIVER_DATA_OFFSET` is the offset to the `context` field.
unsafe impl RawDeviceIdIndex for DeviceId {
    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::wmi_device_id, context);

    fn index(&self) -> usize {
        self.0.context as usize
    }
}

/// Declares a kernel module that exposes a single WMI driver.
///
/// # Examples
///
/// ```ignore
/// module_wmi_driver! {
///     type: MyDriver,
///     name: "Module name",
///     author: ["Author name"],
///     description: "Description",
///     license: "GPL v2",
/// }
/// ```
#[macro_export]
macro_rules! module_wmi_driver {
    ($($f:tt)*) => {
        $crate::module_driver!(<T>, $crate::wmi::Adapter<T>, { $($f)* });
    }
}

/// Create a WMI `IdTable` with its alias for modpost.
#[macro_export]
macro_rules! wmi_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::wmi::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("wmi", $module_table_name, $table_name);
    };
}
