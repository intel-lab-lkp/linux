// SPDX-License-Identifier: GPL-2.0

//! Network PHY device.
//!
//! C headers: [`include/linux/phy.h`](../../../../include/linux/phy.h).

use crate::{bindings, error::*, prelude::vtable, str::CStr, types::Opaque};
use core::marker::PhantomData;

/// Corresponds to the kernel's `enum phy_state`.
#[derive(PartialEq)]
pub enum DeviceState {
    /// PHY device and driver are not ready for anything.
    Down,
    /// PHY is ready to send and receive packets.
    Ready,
    /// PHY is up, but no polling or interrupts are done.
    Halted,
    /// PHY is up, but is in an error state.
    Error,
    /// PHY and attached device are ready to do work.
    Up,
    /// PHY is currently running.
    Running,
    /// PHY is up, but not currently plugged in.
    NoLink,
    /// PHY is performing a cable test.
    CableTest,
}

/// Represents duplex mode.
pub enum DuplexMode {
    /// Full-duplex mode
    Half,
    /// Half-duplex mode
    Full,
    /// Unknown
    Unknown,
}

/// Wraps the kernel's `struct phy_device`.
///
/// # Invariants
///
/// `self.0` is always in a valid state.
#[repr(transparent)]
pub struct Device(Opaque<bindings::phy_device>);

impl Device {
    /// Creates a new [`Device`] instance from a raw pointer.
    ///
    /// # Safety
    ///
    /// For the duration of the lifetime 'a, the pointer must be valid for writing and nobody else
    /// may read or write to the `phy_device` object.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::phy_device) -> &'a mut Self {
        unsafe { &mut *ptr.cast() }
    }

    /// Gets the id of the PHY.
    pub fn phy_id(&mut self) -> u32 {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        unsafe { (*phydev).phy_id }
    }

    /// Gets the state of the PHY.
    pub fn state(&mut self) -> DeviceState {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        let state = unsafe { (*phydev).state };
        match state {
            bindings::phy_state::PHY_DOWN => DeviceState::Down,
            bindings::phy_state::PHY_READY => DeviceState::Ready,
            bindings::phy_state::PHY_HALTED => DeviceState::Halted,
            bindings::phy_state::PHY_ERROR => DeviceState::Error,
            bindings::phy_state::PHY_UP => DeviceState::Up,
            bindings::phy_state::PHY_RUNNING => DeviceState::Running,
            bindings::phy_state::PHY_NOLINK => DeviceState::NoLink,
            bindings::phy_state::PHY_CABLETEST => DeviceState::CableTest,
        }
    }

    /// Returns true if the link is up.
    pub fn get_link(&mut self) -> bool {
        const LINK_IS_UP: u32 = 1;
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        unsafe { (*phydev).link() == LINK_IS_UP }
    }

    /// Returns true if auto-negotiation is enabled.
    pub fn is_autoneg_enabled(&mut self) -> bool {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        unsafe { (*phydev).autoneg() == bindings::AUTONEG_ENABLE }
    }

    /// Returns true if auto-negotiation is completed.
    pub fn is_autoneg_completed(&mut self) -> bool {
        const AUTONEG_COMPLETED: u32 = 1;
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        unsafe { (*phydev).autoneg_complete() == AUTONEG_COMPLETED }
    }

    /// Sets the speed of the PHY.
    pub fn set_speed(&mut self, speed: u32) {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        unsafe { (*phydev).speed = speed as i32 };
    }

    /// Sets duplex mode.
    pub fn set_duplex(&mut self, mode: DuplexMode) {
        let phydev = self.0.get();
        let v = match mode {
            DuplexMode::Full => bindings::DUPLEX_FULL as i32,
            DuplexMode::Half => bindings::DUPLEX_HALF as i32,
            DuplexMode::Unknown => bindings::DUPLEX_UNKNOWN as i32,
        };
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        unsafe { (*phydev).duplex = v };
    }

    /// Reads a given C22 PHY register.
    pub fn read(&mut self, regnum: u16) -> Result<u16> {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        let ret = unsafe {
            bindings::mdiobus_read((*phydev).mdio.bus, (*phydev).mdio.addr, regnum.into())
        };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }

    /// Writes a given C22 PHY register.
    pub fn write(&mut self, regnum: u16, val: u16) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe {
            bindings::mdiobus_write((*phydev).mdio.bus, (*phydev).mdio.addr, regnum.into(), val)
        })
    }

    /// Reads a paged register.
    pub fn read_paged(&mut self, page: u16, regnum: u16) -> Result<u16> {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        let ret = unsafe { bindings::phy_read_paged(phydev, page.into(), regnum.into()) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }

    /// Resolves the advertisements into PHY settings.
    pub fn resolve_aneg_linkmode(&mut self) {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        unsafe { bindings::phy_resolve_aneg_linkmode(phydev) };
    }

    /// Executes software reset the PHY via BMCR_RESET bit.
    pub fn genphy_soft_reset(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::genphy_soft_reset(phydev) })
    }

    /// Initializes the PHY.
    pub fn init_hw(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // so an FFI call with a valid pointer.
        to_result(unsafe { bindings::phy_init_hw(phydev) })
    }

    /// Starts auto-negotiation.
    pub fn start_aneg(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::phy_start_aneg(phydev) })
    }

    /// Resumes the PHY via BMCR_PDOWN bit.
    pub fn genphy_resume(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::genphy_resume(phydev) })
    }

    /// Suspends the PHY via BMCR_PDOWN bit.
    pub fn genphy_suspend(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::genphy_suspend(phydev) })
    }

    /// Checks the link status and updates current link state.
    pub fn genphy_read_status(&mut self) -> Result<u16> {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_read_status(phydev) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }

    /// Updates the link status.
    pub fn genphy_update_link(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::genphy_update_link(phydev) })
    }

    /// Reads Link partner ability.
    pub fn genphy_read_lpa(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::genphy_read_lpa(phydev) })
    }

    /// Reads PHY abilities.
    pub fn genphy_read_abilities(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::genphy_read_abilities(phydev) })
    }
}

/// Defines certain other features this PHY supports (like interrupts).
pub mod flags {
    /// PHY is internal.
    pub const IS_INTERNAL: u32 = bindings::PHY_IS_INTERNAL;
    /// PHY needs to be reset after the refclk is enabled.
    pub const RST_AFTER_CLK_EN: u32 = bindings::PHY_RST_AFTER_CLK_EN;
    /// Polling is used to detect PHY status changes.
    pub const POLL_CABLE_TEST: u32 = bindings::PHY_POLL_CABLE_TEST;
    /// Don't suspend.
    pub const ALWAYS_CALL_SUSPEND: u32 = bindings::PHY_ALWAYS_CALL_SUSPEND;
}

/// An adapter for the registration of a PHY driver.
struct Adapter<T: Driver> {
    _p: PhantomData<T>,
}

impl<T: Driver> Adapter<T> {
    unsafe extern "C" fn soft_reset_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            T::soft_reset(dev)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn get_features_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            T::get_features(dev)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn suspend_callback(phydev: *mut bindings::phy_device) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            T::suspend(dev)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn resume_callback(phydev: *mut bindings::phy_device) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            T::resume(dev)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn config_aneg_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            T::config_aneg(dev)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn read_status_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            T::read_status(dev)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn match_phy_device_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
        let dev = unsafe { Device::from_raw(phydev) };
        T::match_phy_device(dev) as i32
    }

    unsafe extern "C" fn read_mmd_callback(
        phydev: *mut bindings::phy_device,
        devnum: i32,
        regnum: u16,
    ) -> i32 {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            let ret = T::read_mmd(dev, devnum as u8, regnum)?;
            Ok(ret.into())
        })
    }

    unsafe extern "C" fn write_mmd_callback(
        phydev: *mut bindings::phy_device,
        devnum: i32,
        regnum: u16,
        val: u16,
    ) -> i32 {
        from_result(|| {
            // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
            let dev = unsafe { Device::from_raw(phydev) };
            T::write_mmd(dev, devnum as u8, regnum, val)?;
            Ok(0)
        })
    }

    unsafe extern "C" fn link_change_notify_callback(phydev: *mut bindings::phy_device) {
        // SAFETY: The C API guarantees that `phydev` is valid while this function is running.
        let dev = unsafe { Device::from_raw(phydev) };
        T::link_change_notify(dev);
    }
}

/// Creates the kernel's `phy_driver` instance.
///
/// This is used by [`module_phy_driver`] macro to create a static array of phy_driver`.
pub const fn create_phy_driver<T: Driver>() -> Opaque<bindings::phy_driver> {
    Opaque::new(bindings::phy_driver {
        name: T::NAME.as_char_ptr() as *mut i8,
        flags: T::FLAGS,
        phy_id: T::PHY_DEVICE_ID.id,
        phy_id_mask: T::PHY_DEVICE_ID.mask_as_int(),
        soft_reset: if T::HAS_SOFT_RESET {
            Some(Adapter::<T>::soft_reset_callback)
        } else {
            None
        },
        get_features: if T::HAS_GET_FEATURES {
            Some(Adapter::<T>::get_features_callback)
        } else {
            None
        },
        match_phy_device: if T::HAS_MATCH_PHY_DEVICE {
            Some(Adapter::<T>::match_phy_device_callback)
        } else {
            None
        },
        suspend: if T::HAS_SUSPEND {
            Some(Adapter::<T>::suspend_callback)
        } else {
            None
        },
        resume: if T::HAS_RESUME {
            Some(Adapter::<T>::resume_callback)
        } else {
            None
        },
        config_aneg: if T::HAS_CONFIG_ANEG {
            Some(Adapter::<T>::config_aneg_callback)
        } else {
            None
        },
        read_status: if T::HAS_READ_STATUS {
            Some(Adapter::<T>::read_status_callback)
        } else {
            None
        },
        read_mmd: if T::HAS_READ_MMD {
            Some(Adapter::<T>::read_mmd_callback)
        } else {
            None
        },
        write_mmd: if T::HAS_WRITE_MMD {
            Some(Adapter::<T>::write_mmd_callback)
        } else {
            None
        },
        link_change_notify: if T::HAS_LINK_CHANGE_NOTIFY {
            Some(Adapter::<T>::link_change_notify_callback)
        } else {
            None
        },
        // SAFETY: The rest is zeroed out to initialize `struct phy_driver`,
        // sets `Option<&F>` to be `None`.
        ..unsafe { core::mem::MaybeUninit::<bindings::phy_driver>::zeroed().assume_init() }
    })
}

/// Corresponds to functions in `struct phy_driver`.
///
/// This is used to register a PHY driver.
#[vtable]
pub trait Driver {
    /// Defines certain other features this PHY supports.
    const FLAGS: u32 = 0;
    /// The friendly name of this PHY type.
    const NAME: &'static CStr;
    /// This driver only works for PHYs with IDs which match this field.
    const PHY_DEVICE_ID: DeviceId = DeviceId::new_with_custom_mask(0, 0);

    /// Issues a PHY software reset.
    fn soft_reset(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Probes the hardware to determine what abilities it has.
    fn get_features(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Returns true if this is a suitable driver for the given phydev.
    /// If not implemented, matching is based on [`PHY_DEVICE_ID`].
    fn match_phy_device(_dev: &mut Device) -> bool {
        false
    }

    /// Configures the advertisement and resets auto-negotiation
    /// if auto-negotiation is enabled.
    fn config_aneg(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Determines the negotiated speed and duplex.
    fn read_status(_dev: &mut Device) -> Result<u16> {
        Err(code::ENOTSUPP)
    }

    /// Suspends the hardware, saving state if needed.
    fn suspend(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Resumes the hardware, restoring state if needed.
    fn resume(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Overrides the default MMD read function for reading a MMD register.
    fn read_mmd(_dev: &mut Device, _devnum: u8, _regnum: u16) -> Result<u16> {
        Err(code::ENOTSUPP)
    }

    /// Overrides the default MMD write function for writing a MMD register.
    fn write_mmd(_dev: &mut Device, _devnum: u8, _regnum: u16, _val: u16) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Callback for notification of link change.
    fn link_change_notify(_dev: &mut Device) {}
}

/// Registration structure for a PHY driver.
///
/// # Invariants
///
/// The `drivers` points to an array of `struct phy_driver`, which is
/// registered to the kernel via `phy_drivers_register`.
pub struct Registration {
    drivers: Option<&'static [Opaque<bindings::phy_driver>]>,
}

impl Registration {
    /// Registers a PHY driver.
    #[must_use]
    pub fn register(
        module: &'static crate::ThisModule,
        drivers: &'static [Opaque<bindings::phy_driver>],
    ) -> Result<Self> {
        if drivers.len() == 0 {
            return Err(code::EINVAL);
        }
        // SAFETY: `drivers` has static lifetime and used only in the C side.
        to_result(unsafe {
            bindings::phy_drivers_register(drivers[0].get(), drivers.len() as i32, module.0)
        })?;
        Ok(Registration {
            drivers: Some(drivers),
        })
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        if let Some(drv) = self.drivers.take() {
            // SAFETY: The type invariants guarantee that self.drivers is valid.
            unsafe { bindings::phy_drivers_unregister(drv[0].get(), drv.len() as i32) };
        }
    }
}

// SAFETY: `Registration` does not expose any of its state across threads.
unsafe impl Send for Registration {}

// SAFETY: `Registration` does not expose any of its state across threads.
unsafe impl Sync for Registration {}

/// Represents the kernel's `struct mdio_device_id`.
pub struct DeviceId {
    /// Corresponds to `phy_id` in `struct mdio_device_id`.
    pub id: u32,
    mask: DeviceMask,
}

impl DeviceId {
    /// Creates a new instance with the exact match mask.
    pub const fn new_with_exact_mask(id: u32) -> Self {
        DeviceId {
            id,
            mask: DeviceMask::Exact,
        }
    }

    /// Creates a new instance with the model match mask.
    pub const fn new_with_model_mask(id: u32) -> Self {
        DeviceId {
            id,
            mask: DeviceMask::Model,
        }
    }

    /// Creates a new instance with the vendor match mask.
    pub const fn new_with_vendor_mask(id: u32) -> Self {
        DeviceId {
            id,
            mask: DeviceMask::Vendor,
        }
    }

    /// Creates a new instance with a custom match mask.
    pub const fn new_with_custom_mask(id: u32, mask: u32) -> Self {
        DeviceId {
            id,
            mask: DeviceMask::Custom(mask),
        }
    }

    /// Creates a new instance from [`Driver`].
    pub const fn new_with_driver<T: Driver>() -> Self {
        T::PHY_DEVICE_ID
    }

    /// Get a mask as u32.
    pub const fn mask_as_int(self) -> u32 {
        self.mask.as_int()
    }
}

enum DeviceMask {
    Exact,
    Model,
    Vendor,
    Custom(u32),
}

impl DeviceMask {
    const MASK_EXACT: u32 = !0;
    const MASK_MODEL: u32 = !0 << 4;
    const MASK_VENDOR: u32 = !0 << 10;

    const fn as_int(self) -> u32 {
        match self {
            DeviceMask::Exact => Self::MASK_EXACT,
            DeviceMask::Model => Self::MASK_MODEL,
            DeviceMask::Vendor => Self::MASK_VENDOR,
            DeviceMask::Custom(mask) => mask,
        }
    }
}

/// Declares a kernel module for PHYs drivers.
///
/// This creates a static array of `struct phy_driver` and registers it.
/// This also corresponds to the kernel's MODULE_DEVICE_TABLE macro, which embeds the information
/// for module loading into the module binary file.
///
/// # Examples
///
/// ```ignore
///
/// use kernel::net::phy::{self, DeviceId, Driver};
/// use kernel::prelude::*;
///
/// kernel::module_phy_driver! {
///     drivers: [PhyAX88772A, PhyAX88772C, PhyAX88796B],
///     device_table: [
///         DeviceId::new_with_driver::<PhyAX88772A>(),
///         DeviceId::new_with_driver::<PhyAX88772C>(),
///         DeviceId::new_with_driver::<PhyAX88796B>()
///     ],
///     type: RustAsixPhy,
///     name: "rust_asix_phy",
///     author: "Rust for Linux Contributors",
///     description: "Rust Asix PHYs driver",
///     license: "GPL",
/// }
/// ```
#[macro_export]
macro_rules! module_phy_driver {
    (@replace_expr $_t:tt $sub:expr) => {$sub};

    (@count_devices $($x:expr),*) => {
        0usize $(+ $crate::module_phy_driver!(@replace_expr $x 1usize))*
    };

    (@device_table $name:ident, [$($dev:expr),+]) => {
        ::kernel::macros::paste! {
            #[no_mangle]
            static [<__mod_mdio__ $name _device_table>]: [
                kernel::bindings::mdio_device_id;
                $crate::module_phy_driver!(@count_devices $($dev),+) + 1
            ] = [
                $(kernel::bindings::mdio_device_id {
                    phy_id: $dev.id,
                    phy_id_mask: $dev.mask_as_int()
                }),+,
                kernel::bindings::mdio_device_id {
                    phy_id: 0,
                    phy_id_mask: 0
                }
            ];
        }
    };

    (drivers: [$($driver:ident),+], device_table: [$($dev:expr),+], type: $modname:ident, $($f:tt)*) => {
        struct Module<$modname> {
            _reg: kernel::net::phy::Registration,
            _p: core::marker::PhantomData<$modname>,
        }

        type ModuleType = Module<$modname>;

        $crate::prelude::module! {
             type: ModuleType,
             $($f)*
        }

        static mut DRIVERS: [
            kernel::types::Opaque<kernel::bindings::phy_driver>;
            $crate::module_phy_driver!(@count_devices $($driver),+)
        ] = [
            $(kernel::net::phy::create_phy_driver::<$driver>()),+
        ];

        impl kernel::Module for Module<$modname> {
            fn init(module: &'static ThisModule) -> Result<Self> {
                // SAFETY: static `DRIVERS` array is used only in the C side.
                let mut reg = unsafe { kernel::net::phy::Registration::register(module, &DRIVERS) }?;

                Ok(Module {
                    _reg: reg,
                    _p: core::marker::PhantomData,
                })
            }
        }

        $crate::module_phy_driver!(@device_table $modname, [$($dev),+]);
    }
}
