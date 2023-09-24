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

/// Represents duplex mode
pub enum DuplexMode {
    /// Full-duplex mode
    Half,
    /// Half-duplex mode
    Full,
    /// Unknown
    Unknown,
}

/// Wraps the kernel's `struct phy_device`.
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
        unsafe { &mut *(ptr as *mut Self) }
    }

    /// Gets the id of the PHY.
    pub fn id(&mut self) -> u32 {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor, so it's valid.
        unsafe { (*phydev).phy_id }
    }

    /// Gets the state of the PHY.
    pub fn state(&mut self) -> DeviceState {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor, so it's valid.
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

    /// Returns true if the PHY has no link.
    pub fn get_link(&mut self) -> bool {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor, so it's valid.
        unsafe { (*phydev).link() != 0 }
    }

    /// Returns true if auto-negotiation is enabled.
    pub fn is_autoneg_enabled(&mut self) -> bool {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor, so it's valid.
        unsafe { (*phydev).autoneg() == bindings::AUTONEG_ENABLE }
    }

    /// Returns true if auto-negotiation is completed.
    pub fn is_autoneg_completed(&mut self) -> bool {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor, so it's valid.
        unsafe { (*phydev).autoneg_complete() != 0 }
    }

    /// Sets the speed of the PHY.
    pub fn set_speed(&mut self, speed: i32) {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor, so it's valid.
        unsafe {
            (*phydev).speed = speed;
        }
    }

    /// Sets duplex mode.
    pub fn set_duplex(&mut self, mode: DuplexMode) {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor, so it's valid.
        unsafe {
            match mode {
                DuplexMode::Full => (*phydev).duplex = bindings::DUPLEX_FULL as i32,
                DuplexMode::Half => (*phydev).duplex = bindings::DUPLEX_HALF as i32,
                DuplexMode::Unknown => (*phydev).duplex = bindings::DUPLEX_UNKNOWN as i32,
            }
        }
    }

    /// Reads a given C22 PHY register.
    pub fn read(&mut self, regnum: u16) -> Result<u16> {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe {
            bindings::mdiobus_read((*phydev).mdio.bus, (*phydev).mdio.addr, regnum as u32)
        };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }

    /// Writes a given C22 PHY register.
    pub fn write(&mut self, regnum: u16, val: u16) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe {
            bindings::mdiobus_write((*phydev).mdio.bus, (*phydev).mdio.addr, regnum as u32, val)
        };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }

    /// Reads a paged register.
    pub fn read_paged(&mut self, page: u16, regnum: u16) -> Result<u16> {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::phy_read_paged(phydev, page as i32, regnum as u32) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        } else {
            Ok(ret as u16)
        }
    }

    /// Resolves the advertisements into PHY settings.
    pub fn resolve_aneg_linkmode(&mut self) {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        unsafe {
            bindings::phy_resolve_aneg_linkmode(phydev);
        }
    }

    /// Executes software reset the PHY via BMCR_RESET bit.
    pub fn genphy_soft_reset(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_soft_reset(phydev) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }

    /// Initializes the PHY.
    pub fn init_hw(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::phy_init_hw(phydev) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        } else {
            Ok(())
        }
    }

    /// Starts auto-negotiation.
    pub fn start_aneg(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::phy_start_aneg(phydev) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        } else {
            Ok(())
        }
    }

    /// Resumes the PHY via BMCR_PDOWN bit.
    pub fn genphy_resume(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_resume(phydev) };
        if ret != 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }

    /// Suspends the PHY via BMCR_PDOWN bit.
    pub fn genphy_suspend(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_suspend(phydev) };
        if ret != 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }

    /// Checks the link status and updates current link state.
    pub fn genphy_read_status(&mut self) -> Result<u16> {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_read_status(phydev) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }

    /// Updates the link status.
    pub fn genphy_update_link(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_update_link(phydev) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        } else {
            Ok(())
        }
    }

    /// Reads Link partner ability.
    pub fn genphy_read_lpa(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_read_lpa(phydev) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        } else {
            Ok(())
        }
    }

    /// Reads PHY abilities.
    pub fn genphy_read_abilities(&mut self) -> Result {
        let phydev = Opaque::get(&self.0);
        // SAFETY: This object is initialized by the `from_raw` constructor,
        // so an FFI call with a valid pointer.
        let ret = unsafe { bindings::genphy_read_abilities(phydev) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        } else {
            Ok(())
        }
    }
}

// Will be replaced with the bitflags macro once it's merged in upstream.
const fn bit(shift: u8) -> u32 {
    1 << shift
}
/// PHY is internal.
pub const PHY_IS_INTERNAL: u32 = bit(0);
/// PHY needs to be reset after the refclk is enabled.
pub const PHY_RST_AFTER_CLK_EN: u32 = bit(1);
/// Polling is used to detect PHY status changes.
pub const PHY_POLL_CABLE_TEST: u32 = bit(2);
/// Don't suspend.
pub const PHY_ALWAYS_CALL_SUSPEND: u32 = bit(3);

/// An adapter for the registration of a PHY driver.
pub struct Adapter<T: Driver> {
    _p: PhantomData<T>,
}

impl<T: Driver> Adapter<T> {
    /// Creates a new `Adapter` instance.
    pub const fn new() -> Adapter<T> {
        Self { _p: PhantomData }
    }

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

    /// Creates the kernel's `phy_driver` instance.
    ///
    /// This is used by [`module_phy_driver`] macro to create a static array of phy_driver`.
    pub const fn create_phy_driver() -> Opaque<bindings::phy_driver> {
        Opaque::new(bindings::phy_driver {
            name: T::NAME.as_char_ptr() as *mut i8,
            flags: <T>::FLAGS,
            phy_id: <T>::PHY_ID,
            phy_id_mask: <T>::PHY_ID_MASK,
            soft_reset: if <T>::HAS_SOFT_RESET {
                Some(Self::soft_reset_callback)
            } else {
                None
            },
            get_features: if <T>::HAS_GET_FEATURES {
                Some(Self::get_features_callback)
            } else {
                None
            },
            match_phy_device: if <T>::HAS_MATCH_PHY_DEVICE {
                Some(Self::match_phy_device_callback)
            } else {
                None
            },
            suspend: if <T>::HAS_SUSPEND {
                Some(Self::suspend_callback)
            } else {
                None
            },
            resume: if <T>::HAS_RESUME {
                Some(Self::resume_callback)
            } else {
                None
            },
            config_aneg: if <T>::HAS_CONFIG_ANEG {
                Some(Self::config_aneg_callback)
            } else {
                None
            },
            read_status: if <T>::HAS_READ_STATUS {
                Some(Self::read_status_callback)
            } else {
                None
            },
            read_mmd: if <T>::HAS_READ_MMD {
                Some(Self::read_mmd_callback)
            } else {
                None
            },
            write_mmd: if <T>::HAS_WRITE_MMD {
                Some(Self::write_mmd_callback)
            } else {
                None
            },
            link_change_notify: if <T>::HAS_LINK_CHANGE_NOTIFY {
                Some(Self::link_change_notify_callback)
            } else {
                None
            },
            // SAFETY: The rest is zeroed out to initialize `struct phy_driver`,
            // sets `Option<&F>` to be `None`.
            ..unsafe { core::mem::MaybeUninit::<bindings::phy_driver>::zeroed().assume_init() }
        })
    }
}

/// Corresponds to functions in `struct phy_driver`.
#[vtable]
pub trait Driver {
    /// Corresponds to `flags` in `struct phy_driver`.
    const FLAGS: u32 = 0;
    /// Corresponds to `name` in `struct phy_driver`.
    const NAME: &'static CStr;
    /// Corresponds to `phy_id` in `struct phy_driver`.
    const PHY_ID: u32 = 0;
    /// Corresponds to `phy_id_mask` in `struct phy_driver`.
    const PHY_ID_MASK: u32 = 0;

    /// Corresponds to `soft_reset` in `struct phy_driver`.
    fn soft_reset(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `get_features` in `struct phy_driver`.
    fn get_features(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `match_phy_device` in `struct phy_driver`.
    fn match_phy_device(_dev: &mut Device) -> bool {
        false
    }

    /// Corresponds to `config_aneg` in `struct phy_driver`.
    fn config_aneg(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `read_status` in `struct phy_driver`.
    fn read_status(_dev: &mut Device) -> Result<u16> {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `suspend` in `struct phy_driver`.
    fn suspend(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `resume` in `struct phy_driver`.
    fn resume(_dev: &mut Device) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `read_mmd` in `struct phy_driver`.
    fn read_mmd(_dev: &mut Device, _devnum: u8, _regnum: u16) -> Result<u16> {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `write_mmd` in `struct phy_driver`.
    fn write_mmd(_dev: &mut Device, _devnum: u8, _regnum: u16, _val: u16) -> Result {
        Err(code::ENOTSUPP)
    }

    /// Corresponds to `link_change_notify` in `struct phy_driver`.
    fn link_change_notify(_dev: &mut Device) {}
}

/// Registration structure for a PHY driver.
pub struct Registration {
    module: &'static crate::ThisModule,
    drivers: Option<&'static [Opaque<bindings::phy_driver>]>,
}

impl Registration {
    /// Creates a new `Registration` instance.
    pub fn new(module: &'static crate::ThisModule) -> Self {
        Registration {
            module,
            drivers: None,
        }
    }

    /// Registers a PHY driver.
    pub fn register(&mut self, drivers: &'static [Opaque<bindings::phy_driver>]) -> Result {
        if drivers.len() > 0 {
            // SAFETY: Just an FFI call.
            let ret = unsafe {
                bindings::phy_drivers_register(
                    drivers[0].get(),
                    drivers.len() as i32,
                    self.module.0,
                )
            };
            if ret != 0 {
                return Err(Error::from_errno(ret));
            }
            self.drivers = Some(drivers);
            Ok(())
        } else {
            Err(code::EINVAL)
        }
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        if let Some(drv) = self.drivers.take() {
            // SAFETY: Just an FFI call.
            unsafe {
                bindings::phy_drivers_unregister(drv[0].get(), drv.len() as i32);
            }
        }
    }
}

// SAFETY: `Registration` does not expose any of its state across threads.
unsafe impl Send for Registration {}

// SAFETY: `Registration` does not expose any of its state across threads.
unsafe impl Sync for Registration {}

const DEVICE_MASK_EXACT: u32 = !0;
const DEVICE_MASK_MODEL: u32 = !0 << 4;
const DEVICE_MASK_VENDOR: u32 = !0 << 10;

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
        DeviceId {
            id: T::PHY_ID,
            mask: DeviceMask::new(T::PHY_ID_MASK),
        }
    }

    /// Get a mask as u32.
    pub const fn mask_as_int(self) -> u32 {
        match self.mask {
            DeviceMask::Exact => DEVICE_MASK_EXACT,
            DeviceMask::Model => DEVICE_MASK_MODEL,
            DeviceMask::Vendor => DEVICE_MASK_VENDOR,
            DeviceMask::Custom(mask) => mask,
        }
    }
}

enum DeviceMask {
    Exact,
    Model,
    Vendor,
    Custom(u32),
}

impl DeviceMask {
    const fn new(mask: u32) -> Self {
        match mask {
            DEVICE_MASK_EXACT => DeviceMask::Exact,
            DEVICE_MASK_MODEL => DeviceMask::Model,
            DEVICE_MASK_VENDOR => DeviceMask::Vendor,
            _ => DeviceMask::Custom(mask),
        }
    }
}

/// Declares a kernel module for PHYs drivers.
///
/// This creates a static array of `struct phy_driver] and registers it.
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
            static [<__mod_mdio__ $name _device_table>]: [kernel::bindings::mdio_device_id; $crate::module_phy_driver!(@count_devices $($dev),+) + 1] =
             [ $(kernel::bindings::mdio_device_id{phy_id: $dev.id, phy_id_mask: $dev.mask_as_int()}),+, kernel::bindings::mdio_device_id {phy_id: 0, phy_id_mask: 0}];
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

        static mut DRIVERS: [kernel::types::Opaque<kernel::bindings::phy_driver>; $crate::module_phy_driver!(@count_devices $($driver),+)] = [
            $(kernel::net::phy::Adapter::<$driver>::create_phy_driver()),+,
        ];

        impl kernel::Module for Module<$modname> {
            fn init(module: &'static ThisModule) -> Result<Self> {
                let mut reg: kernel::net::phy::Registration = kernel::net::phy::Registration::new(module);
                // SAFETY: static `DRIVERS` array is used only in the C side.
                unsafe {
                    reg.register(&DRIVERS)?;
                }

                Ok(Module {
                    _reg: reg,
                    _p: core::marker::PhantomData,
                })
            }
        }

        $crate::module_phy_driver!(@device_table $modname, [$($dev),+]);
    }
}
