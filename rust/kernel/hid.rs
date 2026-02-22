// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Rahul Rameshbabu <sergeantsagara@protonmail.com>

//! Abstractions for the HID interface.
//!
//! C header: [`include/linux/hid.h`](srctree/include/linux/hid.h)

use crate::{
    device,
    device_id::{
        RawDeviceId,
        RawDeviceIdIndex, //
    },
    driver,
    error::*,
    prelude::*,
    types::Opaque, //
};
use core::{
    marker::PhantomData,
    ptr::{
        addr_of_mut,
        NonNull, //
    } //
};

/// Indicates the item is static read-only.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_CONSTANT: u8 = bindings::HID_MAIN_ITEM_CONSTANT as u8;

/// Indicates the item represents data from a physical control.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_VARIABLE: u8 = bindings::HID_MAIN_ITEM_VARIABLE as u8;

/// Indicates the item should be treated as a relative change from the previous
/// report.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_RELATIVE: u8 = bindings::HID_MAIN_ITEM_RELATIVE as u8;

/// Indicates the item should wrap around when reaching the extreme high or
/// extreme low values.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_WRAP: u8 = bindings::HID_MAIN_ITEM_WRAP as u8;

/// Indicates the item should wrap around when reaching the extreme high or
/// extreme low values.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_NONLINEAR: u8 = bindings::HID_MAIN_ITEM_NONLINEAR as u8;

/// Indicates whether the control has a preferred state it will physically
/// return to without user intervention.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_NO_PREFERRED: u8 = bindings::HID_MAIN_ITEM_NO_PREFERRED as u8;

/// Indicates whether the control has a physical state where it will not send
/// any reports.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_NULL_STATE: u8 = bindings::HID_MAIN_ITEM_NULL_STATE as u8;

/// Indicates whether the control requires host system logic to change state.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_VOLATILE: u8 = bindings::HID_MAIN_ITEM_VOLATILE as u8;

/// Indicates whether the item is fixed size or a variable buffer of bytes.
///
/// Refer to [Device Class Definition for HID 1.11]
/// Section 6.2.2.5 Input, Output, and Feature Items.
///
/// [Device Class Definition for HID 1.11]: https://www.usb.org/sites/default/files/hid1_11.pdf
pub const MAIN_ITEM_BUFFERED_BYTE: u8 = bindings::HID_MAIN_ITEM_BUFFERED_BYTE as u8;

/// HID device groups are intended to help categories HID devices based on a set
/// of common quirks and logic that they will require to function correctly.
#[repr(u16)]
pub enum Group {
    /// Used to match a device against any group when probing.
    Any = bindings::HID_GROUP_ANY as u16,

    /// Indicates a generic device that should need no custom logic from the
    /// core HID stack.
    Generic = bindings::HID_GROUP_GENERIC as u16,

    /// Maps multitouch devices to hid-multitouch instead of hid-generic.
    Multitouch = bindings::HID_GROUP_MULTITOUCH as u16,

    /// Used for autodetecing and mapping of HID sensor hubs to
    /// hid-sensor-hub.
    SensorHub = bindings::HID_GROUP_SENSOR_HUB as u16,

    /// Used for autodetecing and mapping Win 8 multitouch devices to set the
    /// needed quirks.
    MultitouchWin8 = bindings::HID_GROUP_MULTITOUCH_WIN_8 as u16,

    // Vendor-specific device groups.
    /// Used to distinguish Synpatics touchscreens from other products. The
    /// touchscreens will be handled by hid-multitouch instead, while everything
    /// else will be managed by hid-rmi.
    RMI = bindings::HID_GROUP_RMI as u16,

    /// Used for hid-core handling to automatically identify Wacom devices and
    /// have them probed by hid-wacom.
    Wacom = bindings::HID_GROUP_WACOM as u16,

    /// Used by logitech-djreceiver and logitech-djdevice to autodetect if
    /// devices paied to the DJ receivers are DJ devices and handle them with
    /// the device driver.
    LogitechDJDevice = bindings::HID_GROUP_LOGITECH_DJ_DEVICE as u16,

    /// Since the Valve Steam Controller only has vendor-specific usages,
    /// prevent hid-generic from parsing its reports since there would be
    /// nothing hid-generic could do for the device.
    Steam = bindings::HID_GROUP_STEAM as u16,

    /// Used to differentiate 27 Mhz frequency Logitech DJ devices from other
    /// Logitech DJ devices.
    Logitech27MHzDevice = bindings::HID_GROUP_LOGITECH_27MHZ_DEVICE as u16,

    /// Used for autodetecting and mapping Vivaldi devices to hid-vivaldi.
    Vivaldi = bindings::HID_GROUP_VIVALDI as u16,
}

// TODO: use `const_trait_impl` once stabilized:
//
// ```
// impl const From<Group> for u16 {
//     /// [`Group`] variants are represented by [`u16`] values.
//     fn from(value: Group) -> Self {
//         value as Self
//     }
// }
// ```
impl Group {
    /// Internal function used to convert [`Group`] variants into [`u16`].
    const fn into_u16(self) -> u16 {
        self as u16
    }
}

impl TryFrom<u16> for Group {
    type Error = &'static str;

    /// [`u16`] values can be safely converted to [`Group`] variants.
    fn try_from(value: u16) -> Result<Group, Self::Error> {
        match value.into() {
            bindings::HID_GROUP_GENERIC => Ok(Group::Generic),
            bindings::HID_GROUP_MULTITOUCH => Ok(Group::Multitouch),
            bindings::HID_GROUP_SENSOR_HUB => Ok(Group::SensorHub),
            bindings::HID_GROUP_MULTITOUCH_WIN_8 => Ok(Group::MultitouchWin8),
            bindings::HID_GROUP_RMI => Ok(Group::RMI),
            bindings::HID_GROUP_WACOM => Ok(Group::Wacom),
            bindings::HID_GROUP_LOGITECH_DJ_DEVICE => Ok(Group::LogitechDJDevice),
            bindings::HID_GROUP_STEAM => Ok(Group::Steam),
            bindings::HID_GROUP_LOGITECH_27MHZ_DEVICE => Ok(Group::Logitech27MHzDevice),
            bindings::HID_GROUP_VIVALDI => Ok(Group::Vivaldi),
            _ => Err("Unknown HID group encountered!"),
        }
    }
}

/// The HID device representation.
///
/// This structure represents the Rust abstraction for a C `struct hid_device`.
/// The implementation abstracts the usage of an already existing C `struct
/// hid_device` within Rust code that we get passed from the C side.
///
/// # Invariants
///
/// A [`Device`] instance represents a valid `struct hid_device` created by the
/// C portion of the kernel.
#[repr(transparent)]
pub struct Device<Ctx: device::DeviceContext = device::Normal>(
    Opaque<bindings::hid_device>,
    PhantomData<Ctx>,
);

impl<Ctx: device::DeviceContext> Device<Ctx> {
    fn as_raw(&self) -> *mut bindings::hid_device {
        self.0.get()
    }

    /// Returns the HID transport bus ID.
    pub fn bus(&self) -> u16 {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct hid_device`
        unsafe { *self.as_raw() }.bus
    }

    /// Returns the HID report group.
    pub fn group(&self) -> Result<Group, &'static str> {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct hid_device`
        unsafe { *self.as_raw() }.group.try_into()
    }

    /// Returns the HID vendor ID.
    pub fn vendor(&self) -> u32 {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct hid_device`
        unsafe { *self.as_raw() }.vendor
    }

    /// Returns the HID product ID.
    pub fn product(&self) -> u32 {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct hid_device`
        unsafe { *self.as_raw() }.product
    }
}

// SAFETY: `Device` is a transparent wrapper of a type that doesn't depend on `Device`'s generic
// argument.
kernel::impl_device_context_deref!(unsafe { Device });
kernel::impl_device_context_into_aref!(Device);

// SAFETY: Instances of `Device` are always reference-counted.
unsafe impl crate::types::AlwaysRefCounted for Device {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::get_device(&raw mut (*self.as_raw()).dev) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::put_device(&raw mut (*obj.cast::<bindings::hid_device>().as_ptr()).dev) }
    }
}

impl<Ctx: device::DeviceContext> AsRef<device::Device<Ctx>> for Device<Ctx> {
    fn as_ref(&self) -> &device::Device<Ctx> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct hid_device`.
        let dev = unsafe { addr_of_mut!((*self.as_raw()).dev) };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(dev) }
    }
}

/// Abstraction for the HID device ID structure `struct hid_device_id`.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::hid_device_id);

impl DeviceId {
    /// Equivalent to C's `HID_USB_DEVICE` macro.
    ///
    /// Create a new `hid::DeviceId` from a group, vendor ID, and device ID
    /// number.
    pub const fn new_usb(group: Group, vendor: u32, product: u32) -> Self {
        Self(bindings::hid_device_id {
            bus: 0x3, // BUS_USB
            group: group.into_u16(),
            vendor,
            product,
            driver_data: 0,
        })
    }

    /// Returns the HID transport bus ID.
    pub fn bus(&self) -> u16 {
        self.0.bus
    }

    /// Returns the HID report group.
    pub fn group(&self) -> Result<Group, &'static str> {
        self.0.group.try_into()
    }

    /// Returns the HID vendor ID.
    pub fn vendor(&self) -> u32 {
        self.0.vendor
    }

    /// Returns the HID product ID.
    pub fn product(&self) -> u32 {
        self.0.product
    }
}

// SAFETY:
// * `DeviceId` is a `#[repr(transparent)` wrapper of `hid_device_id` and does not add
//   additional invariants, so it's safe to transmute to `RawType`.
// * `DRIVER_DATA_OFFSET` is the offset to the `driver_data` field.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::hid_device_id;
}

// SAFETY: `DRIVER_DATA_OFFSET` is the offset to the `driver_data` field.
unsafe impl RawDeviceIdIndex for DeviceId {
    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::hid_device_id, driver_data);

    fn index(&self) -> usize {
        self.0.driver_data
    }
}

/// [`IdTable`] type for HID.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// Create a HID [`IdTable`] with its alias for modpost.
#[macro_export]
macro_rules! hid_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::hid::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("hid", $module_table_name, $table_name);
    };
}

/// The HID driver trait.
///
/// # Examples
///
/// ```
/// use kernel::{bindings, device, hid};
///
/// struct MyDriver;
///
/// kernel::hid_device_table!(
///     HID_TABLE,
///     MODULE_HID_TABLE,
///     <MyDriver as hid::Driver>::IdInfo,
///     [(
///         hid::DeviceId::new_usb(
///             hid::Group::Steam,
///             bindings::USB_VENDOR_ID_VALVE,
///             bindings::USB_DEVICE_ID_STEAM_DECK,
///         ),
///         (),
///     )]
/// );
///
/// #[vtable]
/// impl hid::Driver for MyDriver {
///     type IdInfo = ();
///     const ID_TABLE: hid::IdTable<Self::IdInfo> = &HID_TABLE;
///
///     /// This function is optional to implement.
///     fn report_fixup<'a, 'b: 'a>(_hdev: &hid::Device<device::Core>, rdesc: &'b mut [u8]) -> &'a [u8] {
///         // Perform some report descriptor fixup.
///         rdesc
///     }
/// }
/// ```
/// Drivers must implement this trait in order to get a HID driver registered.
/// Please refer to the `Adapter` documentation for an example.
#[vtable]
pub trait Driver: Send {
    /// The type holding information about each device id supported by the driver.
    // TODO: Use `associated_type_defaults` once stabilized:
    //
    // ```
    // type IdInfo: 'static = ();
    // ```
    type IdInfo: 'static;

    /// The table of device ids supported by the driver.
    const ID_TABLE: IdTable<Self::IdInfo>;

    /// Called before report descriptor parsing. Can be used to mutate the
    /// report descriptor before the core HID logic processes the descriptor.
    /// Useful for problematic report descriptors that prevent HID devices from
    /// functioning correctly.
    ///
    /// Optional to implement.
    fn report_fixup<'a, 'b: 'a>(_hdev: &Device<device::Core>, _rdesc: &'b mut [u8]) -> &'a [u8] {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// An adapter for the registration of HID drivers.
pub struct Adapter<T: Driver>(T);

// SAFETY:
// - `bindings::hid_driver` is a C type declared as `repr(C)`.
// - `T` is the type of the driver's device private data.
// - `struct hid_driver` embeds a `struct device_driver`.
// - `DEVICE_DRIVER_OFFSET` is the correct byte offset to the embedded `struct device_driver`.
unsafe impl<T: Driver + 'static> driver::DriverLayout for Adapter<T> {
    type DriverType = bindings::hid_driver;
    type DriverData = T;
    const DEVICE_DRIVER_OFFSET: usize = core::mem::offset_of!(Self::DriverType, driver);
}

// SAFETY: A call to `unregister` for a given instance of `DriverType` is guaranteed to be valid if
// a preceding call to `register` has been successful.
unsafe impl<T: Driver + 'static> driver::RegistrationOps for Adapter<T> {
    unsafe fn register(
        hdrv: &Opaque<Self::DriverType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result {
        // SAFETY: It's safe to set the fields of `struct hid_driver` on initialization.
        unsafe {
            (*hdrv.get()).name = name.as_char_ptr();
            (*hdrv.get()).id_table = T::ID_TABLE.as_ptr();
            (*hdrv.get()).report_fixup = if T::HAS_REPORT_FIXUP {
                Some(Self::report_fixup_callback)
            } else {
                None
            };
        }

        // SAFETY: `hdrv` is guaranteed to be a valid `DriverType`
        to_result(unsafe {
            bindings::__hid_register_driver(hdrv.get(), module.0, name.as_char_ptr())
        })
    }

    unsafe fn unregister(hdrv: &Opaque<Self::DriverType>) {
        // SAFETY: `hdrv` is guaranteed to be a valid `DriverType`
        unsafe { bindings::hid_unregister_driver(hdrv.get()) }
    }
}

impl<T: Driver + 'static> Adapter<T> {
    extern "C" fn report_fixup_callback(
        hdev: *mut bindings::hid_device,
        buf: *mut u8,
        size: *mut kernel::ffi::c_uint,
    ) -> *const u8 {
        // SAFETY: The HID subsystem only ever calls the report_fixup callback
        // with a valid pointer to a `struct hid_device`.
        //
        // INVARIANT: `hdev` is valid for the duration of
        // `report_fixup_callback()`.
        let hdev = unsafe { &*hdev.cast::<Device<device::Core>>() };

        // SAFETY: The HID subsystem only ever calls the report_fixup callback
        // with a valid pointer to a `kernel::ffi::c_uint`.
        //
        // INVARIANT: `size` is valid for the duration of
        // `report_fixup_callback()`.
        let buf_len: usize = match unsafe { *size }.try_into() {
            Ok(len) => len,
            Err(e) => {
                dev_err!(
                    hdev.as_ref(),
                    "Cannot fix report description due to {:?}!\n",
                    e
                );

                return buf;
            }
        };

        // Build a mutable Rust slice from `buf` and `size`.
        //
        // SAFETY: The HID subsystem only ever calls the `report_fixup callback`
        // with a valid pointer to a `u8` buffer.
        //
        // INVARIANT: `buf` is valid for the duration of
        // `report_fixup_callback()`.
        let rdesc_slice = unsafe { core::slice::from_raw_parts_mut(buf, buf_len) };
        let rdesc_slice = T::report_fixup(hdev, rdesc_slice);

        match rdesc_slice.len().try_into() {
            // SAFETY: The HID subsystem only ever calls the report_fixup
            // callback with a valid pointer to a `kernel::ffi::c_uint`.
            //
            // INVARIANT: `size` is valid for the duration of
            // `report_fixup_callback()`.
            Ok(len) => unsafe { *size = len },
            Err(e) => {
                dev_err!(
                    hdev.as_ref(),
                    "Fixed report description will not be used due to {:?}!\n",
                    e
                );

                return buf;
            }
        }

        rdesc_slice.as_ptr()
    }
}

/// Declares a kernel module that exposes a single HID driver.
///
/// # Examples
///
/// ```ignore
/// kernel::module_hid_driver! {
///     type: MyDriver,
///     name: "Module name",
///     authors: ["Author name"],
///     description: "Description",
///     license: "GPL",
/// }
/// ```
#[macro_export]
macro_rules! module_hid_driver {
    ($($f:tt)*) => {
        $crate::module_driver!(<T>, $crate::hid::Adapter<T>, { $($f)* });
    };
}
