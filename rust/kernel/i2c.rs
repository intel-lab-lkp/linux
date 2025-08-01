// SPDX-License-Identifier: GPL-2.0

//! I2C Driver subsystem

// I2C Driver abstractions.
use crate::{
    acpi, container_of, device,
    device_id::{RawDeviceId, RawDeviceIdIndex},
    driver,
    error::*,
    of,
    prelude::*,
    types::Opaque,
};

use core::{
    marker::PhantomData,
    ptr::{addr_of_mut, NonNull},
};

/// An I2C device id table.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::i2c_device_id);

impl DeviceId {
    const I2C_NAME_SIZE: usize = 20;

    /// Create a new device id from an I2C 'id' string.
    #[inline(always)]
    pub const fn new(id: &'static CStr) -> Self {
        build_assert!(
            id.len_with_nul() <= Self::I2C_NAME_SIZE,
            "ID exceeds 20 bytes"
        );
        let src = id.as_bytes_with_nul();
        // Replace with `bindings::acpi_device_id::default()` once stabilized for `const`.
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut i2c: bindings::i2c_device_id = unsafe { core::mem::zeroed() };
        let mut i = 0;
        while i < src.len() {
            i2c.name[i] = src[i];
            i += 1;
        }

        Self(i2c)
    }
}

// SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `i2c_device_id` and does not add
// additional invariants, so it's safe to transmute to `RawType`.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::i2c_device_id;
}

// SAFETY: `DRIVER_DATA_OFFSET` is the offset to the `driver_data` field.
unsafe impl RawDeviceIdIndex for DeviceId {
    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::i2c_device_id, driver_data);

    fn index(&self) -> usize {
        self.0.driver_data as _
    }
}

/// IdTable type for I2C
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// Create a I2C `IdTable` with its alias for modpost.
#[macro_export]
macro_rules! i2c_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::i2c::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("i2c", $module_table_name, $table_name);
    };
}

/// An adapter for the registration of I2C drivers.
pub struct Adapter<T: Driver>(T);

// SAFETY: A call to `unregister` for a given instance of `RegType` is guaranteed to be valid if
// a preceding call to `register` has been successful.
unsafe impl<T: Driver + 'static> driver::RegistrationOps for Adapter<T> {
    type RegType = bindings::i2c_driver;

    unsafe fn register(
        pdrv: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result {
        let i2c_table = match T::I2C_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        let of_table = match T::OF_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        let acpi_table = match T::ACPI_ID_TABLE {
            Some(table) => table.as_ptr(),
            None => core::ptr::null(),
        };

        // SAFETY: It's safe to set the fields of `struct i2c_client` on initialization.
        unsafe {
            (*pdrv.get()).driver.name = name.as_char_ptr();
            (*pdrv.get()).probe = Some(Self::probe_callback);
            (*pdrv.get()).remove = Some(Self::remove_callback);
            (*pdrv.get()).shutdown = Some(Self::shutdown_callback);
            (*pdrv.get()).id_table = i2c_table;
            (*pdrv.get()).driver.of_match_table = of_table;
            (*pdrv.get()).driver.acpi_match_table = acpi_table;
        }

        // SAFETY: `pdrv` is guaranteed to be a valid `RegType`.
        to_result(unsafe { bindings::i2c_register_driver(module.0, pdrv.get()) })
    }

    unsafe fn unregister(pdrv: &Opaque<Self::RegType>) {
        // SAFETY: `pdrv` is guaranteed to be a valid `RegType`.
        unsafe { bindings::i2c_del_driver(pdrv.get()) }
    }
}

impl<T: Driver + 'static> Adapter<T> {
    extern "C" fn probe_callback(pdev: *mut bindings::i2c_client) -> kernel::ffi::c_int {
        // SAFETY: The I2C bus only ever calls the probe callback with a valid pointer to a
        // `struct i2c_client`.
        //
        // INVARIANT: `pdev` is valid for the duration of `probe_callback()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        let info =
            Self::i2c_id_info(pdev).or_else(|| <Self as driver::Adapter>::id_info(pdev.as_ref()));

        from_result(|| {
            let data = T::probe(pdev, info)?;

            pdev.as_ref().set_drvdata(data);
            Ok(0)
        })
    }

    extern "C" fn remove_callback(pdev: *mut bindings::i2c_client) {
        // SAFETY: `pdev` is a valid pointer to a `struct i2c_client`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `remove_callback` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        drop(unsafe { pdev.as_ref().drvdata_obtain::<Pin<KBox<T>>>() });
    }

    extern "C" fn shutdown_callback(pdev: *mut bindings::i2c_client) {
        let pdev = unsafe { &*pdev.cast::<Device<device::Core>>() };

        T::shutdown(pdev);
    }

    /// The [`i2c::IdTable`] of the corresponding driver.
    fn i2c_id_table() -> Option<IdTable<<Self as driver::Adapter>::IdInfo>> {
        T::I2C_ID_TABLE
    }

    /// Returns the driver's private data from the matching entry in the [`i2c::IdTable`], if any.
    ///
    /// If this returns `None`, it means there is no match with an entry in the [`i2c::IdTable`].
    fn i2c_id_info(dev: &Device) -> Option<&'static <Self as driver::Adapter>::IdInfo> {
        let table = Self::i2c_id_table()?;

        // SAFETY:
        // - `table` has static lifetime, hence it's valid for read,
        // - `dev` is guaranteed to be valid while it's alive, and so is `pdev.as_ref().as_raw()`.
        let raw_id = unsafe { bindings::i2c_match_id(table.as_ptr(), dev.as_raw()) };

        if raw_id.is_null() {
            None
        } else {
            // SAFETY: `DeviceId` is a `#[repr(transparent)` wrapper of `struct i2c_device_id` and
            // does not add additional invariants, so it's safe to transmute.
            let id = unsafe { &*raw_id.cast::<DeviceId>() };

            Some(table.info(<DeviceId as crate::device_id::RawDeviceIdIndex>::index(id)))
        }
    }
}

impl<T: Driver + 'static> driver::Adapter for Adapter<T> {
    type IdInfo = T::IdInfo;

    fn of_id_table() -> Option<of::IdTable<Self::IdInfo>> {
        T::OF_ID_TABLE
    }

    fn acpi_id_table() -> Option<acpi::IdTable<Self::IdInfo>> {
        T::ACPI_ID_TABLE
    }
}

/// Declares a kernel module that exposes a single i2c driver.
///
/// # Examples
///
/// ```ignore
/// kernel::module_i2c_driver! {
///     type: MyDriver,
///     name: "Module name",
///     authors: ["Author name"],
///     description: "Description",
///     license: "GPL v2",
/// }
/// ```
#[macro_export]
macro_rules! module_i2c_driver {
    ($($f:tt)*) => {
        $crate::module_driver!(<T>, $crate::i2c::Adapter<T>, { $($f)* });
    };
}

/// The i2c driver trait.
///
/// Drivers must implement this trait in order to get a i2c driver registered.
///
/// # Example
///
///```
/// # use kernel::{acpi, bindings, c_str, device::Core, i2c, of};
///
/// struct MyDriver;
///
/// kernel::acpi_device_table!(
///     ACPI_TABLE,
///     MODULE_ACPI_TABLE,
///     <MyDriver as i2c::Driver>::IdInfo,
///     [
///         (acpi::DeviceId::new(c_str!("LNUXBEEF")), ())
///     ]
/// );
///
/// kernel::i2c_device_table!(
///     I2C_TABLE,
///     MODULE_I2C_TABLE,
///     <MyDriver as i2c::Driver>::IdInfo,
///     [
///          (i2c::DeviceId::new(c_str!("rust_driver_i2c")), ())
///     ]
/// );
///
/// kernel::of_device_table!(
///     OF_TABLE,
///     MODULE_OF_TABLE,
///     <MyDriver as i2c::Driver>::IdInfo,
///     [
///         (of::DeviceId::new(c_str!("test,device")), ())
///     ]
/// );
///
/// impl i2c::Driver for MyDriver {
///     type IdInfo = ();
///     const I2C_ID_TABLE: Option<i2c::IdTable<Self::IdInfo>> = Some(&I2C_TABLE);
///     const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
///     const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);
///
///     fn probe(
///         _pdev: &i2c::Device<Core>,
///         _id_info: Option<&Self::IdInfo>,
///     ) -> Result<Pin<KBox<Self>>> {
///         Err(ENODEV)
///     }
///
///     fn shutdown(_pdev: &i2c::Device<Core>) {
///     }
/// }
///```
pub trait Driver: Send {
    /// The type holding information about each device id supported by the driver.
    // TODO: Use `associated_type_defaults` once stabilized:
    //
    // ```
    // type IdInfo: 'static = ();
    // ```
    type IdInfo: 'static;

    /// The table of device ids supported by the driver.
    const I2C_ID_TABLE: Option<IdTable<Self::IdInfo>> = None;

    /// The table of OF device ids supported by the driver.
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = None;

    /// The table of ACPI device ids supported by the driver.
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = None;

    /// I2C driver probe.
    ///
    /// Called when a new i2c device is added or discovered.
    /// Implementers should attempt to initialize the device here.
    fn probe(dev: &Device<device::Core>, id_info: Option<&Self::IdInfo>)
        -> Result<Pin<KBox<Self>>>;

    /// I2C driver shutdown
    ///
    /// Called when
    fn shutdown(dev: &Device<device::Core>) {
        let _ = dev;
    }
}

/// The i2c adapter reference
///
/// This represents the Rust abstraction for a reference to an existing
/// C `struct i2c_adapter`.
///
/// # Invariants
///
/// A [`I2cAdapterRef`] instance represents a valid `struct i2c_adapter` created by the C portion of
/// the kernel.
#[repr(transparent)]
pub struct I2cAdapterRef(NonNull<bindings::i2c_adapter>);

impl I2cAdapterRef {
    fn as_raw(&self) -> *mut bindings::i2c_adapter {
        self.0.as_ptr()
    }

    /// Gets pointer to an `i2c_adapter` by index.
    pub fn get(index: i32) -> Option<Self> {
        // SAFETY: `index` must refer to a valid I2C adapter; the kernel
        // guarantees that `i2c_get_adapter(index)` returns either a valid
        // pointer or NULL. `NonNull::new` guarantees the correct check.
        let adapter = NonNull::new(unsafe { bindings::i2c_get_adapter(index) })?;
        Some(Self(adapter))
    }
}

impl Drop for I2cAdapterRef {
    fn drop(&mut self) {
        // SAFETY: This `I2cAdapterRef` was obtained from `i2c_get_adapter`,
        // and calling `i2c_put_adapter` exactly once will correctly release
        // the reference count in the I2C core. It is safe to call from any context
        unsafe { bindings::i2c_put_adapter(self.as_raw()) }
    }
}

/// The i2c board info representation
///
/// This structure represents the Rust abstraction for a C `struct i2c_board_info` structure,
/// which is used for manual I2C client creation.
#[repr(transparent)]
pub struct I2cBoardInfo(bindings::i2c_board_info);

impl I2cBoardInfo {
    const I2C_TYPE_SIZE: usize = 20;
    /// Create a new board‐info for a kernel driver.
    #[inline(always)]
    pub const fn new(type_: &'static CStr, addr: u16) -> Self {
        build_assert!(
            type_.len_with_nul() <= Self::I2C_TYPE_SIZE,
            "Type exceeds 20 bytes"
        );
        let src = type_.as_bytes_with_nul();
        // Replace with `bindings::acpi_device_id::default()` once stabilized for `const`.
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut i2c_board_info: bindings::i2c_board_info = unsafe { core::mem::zeroed() };
        let mut i: usize = 0;
        while i < src.len() {
            i2c_board_info.type_[i] = src[i];
            i += 1;
        }

        i2c_board_info.addr = addr;
        Self(i2c_board_info)
    }

    fn as_raw(&self) -> *const bindings::i2c_board_info {
        &self.0 as *const _
    }
}

/// The i2c client representation.
///
/// This structure represents the Rust abstraction for a C `struct i2c_client`. The
/// implementation abstracts the usage of an already existing C `struct i2c_client` within Rust
/// code that we get passed from the C side.
///
/// # Invariants
///
/// A [`Device`] instance represents a valid `struct i2c_client` created by the C portion of
/// the kernel.
#[repr(transparent)]
pub struct Device<Ctx: device::DeviceContext = device::Normal>(
    Opaque<bindings::i2c_client>,
    PhantomData<Ctx>,
);

impl<Ctx: device::DeviceContext> Device<Ctx> {
    fn as_raw(&self) -> *mut bindings::i2c_client {
        self.0.get()
    }
}

// SAFETY: `Device` is a transparent wrapper of a type that doesn't depend on `Device`'s generic
// argument.
kernel::impl_device_context_deref!(unsafe { Device });
kernel::impl_device_context_into_aref!(Device);

// SAFETY: Instances of `Device` are always reference-counted.
unsafe impl<Ctx: device::DeviceContext> crate::types::AlwaysRefCounted for Device<Ctx> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::get_device(self.as_ref().as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::put_device(addr_of_mut!((*obj.as_ref().as_raw()).dev)) }
    }
}

impl<Ctx: device::DeviceContext> AsRef<device::Device<Ctx>> for Device<Ctx> {
    fn as_ref(&self) -> &device::Device<Ctx> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct i2c_client`.
        let dev = unsafe { addr_of_mut!((*self.as_raw()).dev) };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(dev) }
    }
}

impl<Ctx: device::DeviceContext> TryFrom<&device::Device<Ctx>> for &Device<Ctx> {
    type Error = kernel::error::Error;

    fn try_from(dev: &device::Device<Ctx>) -> Result<Self, Self::Error> {
        // SAFETY: By the type invariant of `Device`, `dev.as_raw()` is a valid pointer to a
        // `struct device`.
        if unsafe { bindings::i2c_verify_client(dev.as_raw()).is_null() } {
            return Err(EINVAL);
        }

        // SAFETY: We've just verified that the type of `dev` equals to
        // `bindings::i2c_client_type`, hence `dev` must be embedded in a valid
        // `struct i2c_client` as guaranteed by the corresponding C code.
        let pdev = unsafe { container_of!(dev.as_raw(), bindings::i2c_client, dev) };

        // SAFETY: `pdev` is a valid pointer to a `struct i2c_client`.
        Ok(unsafe { &*pdev.cast() })
    }
}

// SAFETY: A `Device` is always reference-counted and can be released from any thread.
unsafe impl Send for Device {}

// SAFETY: `Device` can be shared among threads because all methods of `Device`
// (i.e. `Device<Normal>) are thread safe.
unsafe impl Sync for Device {}

/// The registration of an i2c client device.
///
/// This type represents the registration of a [`struct i2c_client`]. When an instance of this
/// type is dropped, its respective i2c client device will be unregistered from the system.
///
/// # Invariants
///
/// `self.0` always holds a valid pointer to an initialized and registered
/// [`struct i2c_client`].
#[repr(transparent)]
pub struct Registration(NonNull<bindings::i2c_client>);

impl Registration {
    /// The C `i2c_new_client_device` function wrapper for manual I2C client creation.
    pub fn new(i2c_adapter: &I2cAdapterRef, i2c_board_info: &I2cBoardInfo) -> Result<Self> {
        // SAFETY: the kernel guarantees that `i2c_new_client_device()` returns either a valid
        // pointer or NULL. `NonNull::new` guarantees the correct check.
        let raw_dev = from_err_ptr(unsafe {
            bindings::i2c_new_client_device(i2c_adapter.as_raw(), i2c_board_info.as_raw())
        })?;

        Ok(Self(NonNull::new(raw_dev).unwrap()))
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        unsafe { bindings::i2c_unregister_device(self.0.as_ptr()) }
    }
}

// SAFETY: A `Device` is always reference-counted and can be released from any thread.
unsafe impl Send for Registration {}

// SAFETY: A `Device` is always reference-counted and can be released from any thread.
unsafe impl Sync for Registration {}
