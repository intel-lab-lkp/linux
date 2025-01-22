// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the platform bus.
//!
//! C header: [`include/linux/platform_device.h`](srctree/include/linux/platform_device.h)

use crate::{
    bindings, container_of, device, driver,
    error::{to_result, Result},
    of,
    prelude::*,
    str::CStr,
    types::{ARef, ForeignOwnable, Opaque},
    ThisModule,
};
use core::{
    mem::ManuallyDrop,
    ops::*,
    ptr::{addr_of_mut, NonNull},
};

/// An adapter for the registration of platform drivers.
pub struct Adapter<T: Driver>(T);

// SAFETY: A call to `unregister` for a given instance of `RegType` is guaranteed to be valid if
// a preceding call to `register` has been successful.
unsafe impl<T: Driver + 'static> driver::RegistrationOps for Adapter<T> {
    type RegType = bindings::platform_driver;

    unsafe fn register(
        pdrv: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result {
        let of_table = match T::OF_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        // SAFETY: It's safe to set the fields of `struct platform_driver` on initialization.
        unsafe {
            (*pdrv.get()).driver.name = name.as_char_ptr();
            (*pdrv.get()).probe = Some(Self::probe_callback);
            (*pdrv.get()).remove = Some(Self::remove_callback);
            (*pdrv.get()).driver.of_match_table = of_table;
        }

        // SAFETY: `pdrv` is guaranteed to be a valid `RegType`.
        to_result(unsafe { bindings::__platform_driver_register(pdrv.get(), module.0) })
    }

    unsafe fn unregister(pdrv: &Opaque<Self::RegType>) {
        // SAFETY: `pdrv` is guaranteed to be a valid `RegType`.
        unsafe { bindings::platform_driver_unregister(pdrv.get()) };
    }
}

impl<T: Driver + 'static> Adapter<T> {
    extern "C" fn probe_callback(pdev: *mut bindings::platform_device) -> kernel::ffi::c_int {
        // SAFETY: The platform bus only ever calls the probe callback with a valid `pdev`.
        let dev = unsafe { device::Device::get_device(addr_of_mut!((*pdev).dev)) };
        // SAFETY: `dev` is guaranteed to be embedded in a valid `struct platform_device` by the
        // call above.
        let mut pdev = unsafe { Device::from_dev(dev) };

        let info = <Self as driver::Adapter>::id_info(pdev.as_ref());
        match T::probe(&mut pdev, info) {
            Ok(data) => {
                // Let the `struct platform_device` own a reference of the driver's private data.
                // SAFETY: By the type invariant `pdev.as_raw` returns a valid pointer to a
                // `struct platform_device`.
                unsafe { bindings::platform_set_drvdata(pdev.as_raw(), data.into_foreign() as _) };
            }
            Err(err) => return Error::to_errno(err),
        }

        0
    }

    extern "C" fn remove_callback(pdev: *mut bindings::platform_device) {
        // SAFETY: `pdev` is a valid pointer to a `struct platform_device`.
        let ptr = unsafe { bindings::platform_get_drvdata(pdev) };

        // SAFETY: `remove_callback` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `ptr` points to a valid and initialized
        // `KBox<T>` pointer created through `KBox::into_foreign`.
        let _ = unsafe { KBox::<T>::from_foreign(ptr) };
    }
}

impl<T: Driver + 'static> driver::Adapter for Adapter<T> {
    type IdInfo = T::IdInfo;

    fn of_id_table() -> Option<of::IdTable<Self::IdInfo>> {
        T::OF_ID_TABLE
    }
}

/// Declares a kernel module that exposes a single platform driver.
///
/// # Examples
///
/// ```ignore
/// kernel::module_platform_driver! {
///     type: MyDriver,
///     name: "Module name",
///     author: "Author name",
///     description: "Description",
///     license: "GPL v2",
/// }
/// ```
#[macro_export]
macro_rules! module_platform_driver {
    ($($f:tt)*) => {
        $crate::module_driver!(<T>, $crate::platform::Adapter<T>, { $($f)* });
    };
}

/// The platform driver trait.
///
/// Drivers must implement this trait in order to get a platform driver registered.
///
/// # Example
///
///```
/// # use kernel::{bindings, c_str, of, platform};
///
/// struct MyDriver;
///
/// kernel::of_device_table!(
///     OF_TABLE,
///     MODULE_OF_TABLE,
///     <MyDriver as platform::Driver>::IdInfo,
///     [
///         (of::DeviceId::new(c_str!("test,device")), ())
///     ]
/// );
///
/// impl platform::Driver for MyDriver {
///     type IdInfo = ();
///     const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
///
///     fn probe(
///         _pdev: &mut platform::Device,
///         _id_info: Option<&Self::IdInfo>,
///     ) -> Result<Pin<KBox<Self>>> {
///         Err(ENODEV)
///     }
/// }
///```
pub trait Driver {
    /// The type holding driver private data about each device id supported by the driver.
    ///
    /// TODO: Use associated_type_defaults once stabilized:
    ///
    /// type IdInfo: 'static = ();
    type IdInfo: 'static;

    /// The table of OF device ids supported by the driver.
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>>;

    /// Platform driver probe.
    ///
    /// Called when a new platform device is added or discovered.
    /// Implementers should attempt to initialize the device here.
    fn probe(dev: &mut Device, id_info: Option<&Self::IdInfo>) -> Result<Pin<KBox<Self>>>;
}

/// The platform device representation.
///
/// A platform device is based on an always reference counted `device:Device` instance. Cloning a
/// platform device, hence, also increments the base device' reference count.
///
/// # Invariants
///
/// `Device` holds a valid reference of `ARef<device::Device>` whose underlying `struct device` is a
/// member of a `struct platform_device`.
#[derive(Clone)]
pub struct Device(ARef<device::Device>);

impl Device {
    /// Convert a raw kernel device into a `Device`
    ///
    /// # Safety
    ///
    /// `dev` must be an `Aref<device::Device>` whose underlying `bindings::device` is a member of a
    /// `bindings::platform_device`.
    unsafe fn from_dev(dev: ARef<device::Device>) -> Self {
        Self(dev)
    }

    /// Convert a raw pointer to a `struct platform_device` into a `Device`.
    ///
    /// # Safety
    ///
    /// * `pdev` must be a valid pointer to a `bindings::platform_device`.
    /// * The caller must be guaranteed to hold at least one reference to `pdev`.
    unsafe fn from_raw(pdev: *mut bindings::platform_device) -> Self {
        // SAFETY:
        // * Our safety contract ensures `pdev` is a valid pointer which we hold at least one
        //   reference to.
        // * struct device and `device::Device` have equivalent data layouts via the
        //   `device::Device` type invariants.
        Self(unsafe { ARef::from_raw(NonNull::new_unchecked(addr_of_mut!((*pdev).dev).cast())) })
    }

    fn as_raw(&self) -> *mut bindings::platform_device {
        // SAFETY: By the type invariant `self.0.as_raw` is a pointer to the `struct device`
        // embedded in `struct platform_device`.
        unsafe { container_of!(self.0.as_raw(), bindings::platform_device, dev) }.cast_mut()
    }
}

impl AsRef<device::Device> for Device {
    fn as_ref(&self) -> &device::Device {
        &self.0
    }
}

/// A platform device ID specifier.
///
/// This type is used for selecting the kind of device ID to use when constructing a new
/// [`ModuleDevice`].
#[derive(Copy, Clone)]
pub enum ModuleDeviceId {
    /// Do not use a device ID with a device.
    None,
    /// Automatically allocate a device ID for a device.
    Auto,
    /// Explicitly specify a device ID for a device.
    Explicit(i32),
}

impl ModuleDeviceId {
    fn as_raw(self) -> Result<i32> {
        match self {
            ModuleDeviceId::Explicit(id) => {
                if matches!(
                    id,
                    bindings::PLATFORM_DEVID_NONE | bindings::PLATFORM_DEVID_AUTO
                ) {
                    Err(EINVAL)
                } else {
                    Ok(id)
                }
            }
            ModuleDeviceId::None => Ok(bindings::PLATFORM_DEVID_NONE),
            ModuleDeviceId::Auto => Ok(bindings::PLATFORM_DEVID_AUTO),
        }
    }
}

/// A platform device that was created by a module.
///
/// This type represents a platform device that was manually created by a kernel module, typically a
/// virtual device, instead of being discovered by the kernel. It is probed upon creation in the
/// same manner as a typical platform device, and the device will not be unregistered until this
/// type is dropped.
// We store the Device in a ManuallyDrop container, since we must enforce that our reference to the
// Device is dropped using platform_device_unregister()
pub struct ModuleDevice(ManuallyDrop<Device>);

impl ModuleDevice {
    /// Create and register a new platform device.
    ///
    /// This creates and registers a new platform device. This is usually only useful for drivers
    /// which create virtual devices, as drivers for real hardware can rely on the kernel's probing
    /// process.
    pub fn new(name: &'static CStr, id: ModuleDeviceId) -> Result<Self> {
        // SAFETY:
        // * ModuleDeviceId::as_raw() always returns a valid device ID
        // * Returns NULL on failure, or a valid platform_device pointer on success
        let pdev_ptr = unsafe { bindings::platform_device_alloc(name.as_char_ptr(), id.as_raw()?) };
        if pdev_ptr.is_null() {
            return Err(ENOMEM);
        }

        // SAFETY:
        // * The previous function is guaranteed to have returned a valid pointer to a platform_dev,
        //   or NULL (which we checked for already)
        // * The previous function also took a single reference to the platform_dev
        let pdev = unsafe { Device::from_raw(pdev_ptr) };

        // SAFETY: We already checked that pdev_ptr is valid above.
        to_result(unsafe { bindings::platform_device_add(pdev_ptr) })
            .map(|_| ModuleDevice(ManuallyDrop::new(pdev)))
    }
}

impl Deref for ModuleDevice {
    type Target = Device;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Drop for ModuleDevice {
    fn drop(&mut self) {
        // SAFETY: Only one instance of this type can exist for a given platform device, so this is
        // safe to call.
        unsafe { bindings::platform_device_unregister(self.as_raw()) }

        // No need to manually drop our contents, as platform_device_unregister() dropped the ref
        // count that was owned by this type.
    }
}
