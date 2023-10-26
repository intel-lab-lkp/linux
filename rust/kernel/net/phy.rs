// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2023 FUJITA Tomonori <fujita.tomonori@gmail.com>

//! Network PHY device.
//!
//! C headers: [`include/linux/phy.h`](../../../../../../../include/linux/phy.h).

use crate::{
    bindings,
    error::*,
    prelude::{vtable, Pin},
    str::CStr,
    types::Opaque,
};
use core::marker::PhantomData;

/// PHY state machine states.
///
/// Corresponds to the kernel's [`enum phy_state`].
///
/// Some of PHY drivers access to the state of PHY's software state machine.
///
/// [`enum phy_state`]: ../../../../../../../include/linux/phy.h
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

/// A mode of Ethernet communication.
///
/// PHY drivers get duplex information from hardware and update the current state.
pub enum DuplexMode {
    /// PHY is in full-duplex mode.
    Full,
    /// PHY is in half-duplex mode.
    Half,
    /// PHY is in unknown duplex mode.
    Unknown,
}

/// An instance of a PHY device.
///
/// Wraps the kernel's [`struct phy_device`].
///
/// A [`Device`] instance is created when a callback in [`Driver`] is executed. A PHY driver
/// executes [`Driver`]'s methods during the callback.
///
/// # Invariants
///
/// `self.0` is always in a valid state.
///
/// [`struct phy_device`]: ../../../../../../../include/linux/phy.h
// During the calls to most functions in [`Driver`], the C side (`PHYLIB`) holds a lock that is
// unique for every instance of [`Device`]. `PHYLIB` uses a different serialization technique for
// [`Driver::resume`] and [`Driver::suspend`]: `PHYLIB` updates `phy_device`'s state with
// the lock held, thus guaranteeing that [`Driver::resume`] has exclusive access to the instance.
// [`Driver::resume`] and [`Driver::suspend`] also are called where only one thread can access
// to the instance.
#[repr(transparent)]
pub struct Device(Opaque<bindings::phy_device>);

impl Device {
    /// Creates a new [`Device`] instance from a raw pointer.
    ///
    /// # Safety
    ///
    /// This function must only be called from the callbacks in `phy_driver`.
    unsafe fn from_raw<'a>(ptr: *mut bindings::phy_device) -> &'a mut Self {
        // CAST: `Self` is a `repr(transparent)` wrapper around `bindings::phy_device`.
        let ptr = ptr.cast::<Self>();
        // SAFETY: by the function requirements the pointer is valid and we have unique access for
        // the duration of `'a`.
        unsafe { &mut *ptr }
    }

    /// Gets the id of the PHY.
    pub fn phy_id(&self) -> u32 {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        unsafe { (*phydev).phy_id }
    }

    /// Gets the state of PHY state machine states.
    pub fn state(&self) -> DeviceState {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        let state = unsafe { (*phydev).state };
        // TODO: this conversion code will be replaced with automatically generated code by bindgen
        // when it becomes possible.
        // better to call WARN_ONCE() when the state is out-of-range.
        match state {
            bindings::phy_state_PHY_DOWN => DeviceState::Down,
            bindings::phy_state_PHY_READY => DeviceState::Ready,
            bindings::phy_state_PHY_HALTED => DeviceState::Halted,
            bindings::phy_state_PHY_ERROR => DeviceState::Error,
            bindings::phy_state_PHY_UP => DeviceState::Up,
            bindings::phy_state_PHY_RUNNING => DeviceState::Running,
            bindings::phy_state_PHY_NOLINK => DeviceState::NoLink,
            bindings::phy_state_PHY_CABLETEST => DeviceState::CableTest,
            _ => DeviceState::Error,
        }
    }

    /// Gets the current link state.
    ///
    /// It returns true if the link is up.
    pub fn is_link_up(&self) -> bool {
        const LINK_IS_UP: u32 = 1;
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        let phydev = unsafe { *self.0.get() };
        phydev.link() == LINK_IS_UP
    }

    /// Gets the current auto-negotiation configuration.
    ///
    /// It returns true if auto-negotiation is enabled.
    pub fn is_autoneg_enabled(&self) -> bool {
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        let phydev = unsafe { *self.0.get() };
        phydev.autoneg() == bindings::AUTONEG_ENABLE
    }

    /// Gets the current auto-negotiation state.
    ///
    /// It returns true if auto-negotiation is completed.
    pub fn is_autoneg_completed(&self) -> bool {
        const AUTONEG_COMPLETED: u32 = 1;
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        let phydev = unsafe { *self.0.get() };
        phydev.autoneg_complete() == AUTONEG_COMPLETED
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
    // This function reads a hardware register and updates the stats so takes `&mut self`.
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

    /// Executes software reset the PHY via `BMCR_RESET` bit.
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

    /// Resumes the PHY via `BMCR_PDOWN` bit.
    pub fn genphy_resume(&mut self) -> Result {
        let phydev = self.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So an FFI call with a valid pointer.
        to_result(unsafe { bindings::genphy_resume(phydev) })
    }

    /// Suspends the PHY via `BMCR_PDOWN` bit.
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

    /// Reads link partner ability.
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
///
/// These flag values are used in [`Driver::FLAGS`].
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
    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn soft_reset_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            T::soft_reset(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn get_features_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            T::get_features(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn suspend_callback(phydev: *mut bindings::phy_device) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            T::suspend(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn resume_callback(phydev: *mut bindings::phy_device) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            T::resume(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn config_aneg_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            T::config_aneg(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn read_status_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            T::read_status(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn match_phy_device_callback(
        phydev: *mut bindings::phy_device,
    ) -> core::ffi::c_int {
        // SAFETY: Preconditions ensure `phydev` is valid.
        let dev = unsafe { Device::from_raw(phydev) };
        T::match_phy_device(dev) as i32
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn read_mmd_callback(
        phydev: *mut bindings::phy_device,
        devnum: i32,
        regnum: u16,
    ) -> i32 {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            // CAST: the C side verifies devnum < 32.
            let ret = T::read_mmd(dev, devnum as u8, regnum)?;
            Ok(ret.into())
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn write_mmd_callback(
        phydev: *mut bindings::phy_device,
        devnum: i32,
        regnum: u16,
        val: u16,
    ) -> i32 {
        from_result(|| {
            // SAFETY: Preconditions ensure `phydev` is valid.
            let dev = unsafe { Device::from_raw(phydev) };
            T::write_mmd(dev, devnum as u8, regnum, val)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `phydev` must be passed by the corresponding callback in `phy_driver`.
    unsafe extern "C" fn link_change_notify_callback(phydev: *mut bindings::phy_device) {
        // SAFETY: Preconditions ensure `phydev` is valid.
        let dev = unsafe { Device::from_raw(phydev) };
        T::link_change_notify(dev);
    }
}

/// Driver structure for a particular PHY type.
///
/// Wraps the kernel's [`struct phy_driver`].
/// This is used to register a driver for a particular PHY type with the kernel.
///
/// # Invariants
///
/// `self.0` is always in a valid state.
///
/// [`struct phy_driver`]: ../../../../../../../include/linux/phy.h
#[repr(transparent)]
pub struct DriverVTable(Opaque<bindings::phy_driver>);

/// Creates a [`DriverVTable`] instance from [`Driver`].
///
/// This is used by [`module_phy_driver`] macro to create a static array of `phy_driver`.
///
/// [`module_phy_driver`]: crate::module_phy_driver
pub const fn create_phy_driver<T: Driver>() -> DriverVTable {
    // INVARIANT: All the fields of `struct phy_driver` are initialized properly.
    DriverVTable(Opaque::new(bindings::phy_driver {
        name: T::NAME.as_char_ptr().cast_mut(),
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
    }))
}

/// Driver implementation for a particular PHY type.
///
/// This trait is used to create a [`DriverVTable`].
#[vtable]
pub trait Driver {
    /// Defines certain other features this PHY supports.
    /// It is a combination of the flags in the [`flags`] module.
    const FLAGS: u32 = 0;

    /// The friendly name of this PHY type.
    const NAME: &'static CStr;

    /// This driver only works for PHYs with IDs which match this field.
    /// The default id and mask are zero.
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
    /// If not implemented, matching is based on [`Driver::PHY_DEVICE_ID`].
    fn match_phy_device(_dev: &Device) -> bool {
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

/// Registration structure for PHY drivers.
///
/// Registers [`DriverVTable`] instances with the kernel. They will be unregistered when dropped.
///
/// # Invariants
///
/// The `drivers` slice are currently registered to the kernel via `phy_drivers_register`.
pub struct Registration {
    drivers: Pin<&'static mut [DriverVTable]>,
}

impl Registration {
    /// Registers a PHY driver.
    pub fn register(
        module: &'static crate::ThisModule,
        drivers: Pin<&'static mut [DriverVTable]>,
    ) -> Result<Self> {
        if drivers.is_empty() {
            return Err(code::EINVAL);
        }
        // SAFETY: The type invariants of [`DriverVTable`] ensure that all elements of
        // the `drivers` slice are initialized properly. `drivers` will not be moved.
        // So an FFI call with a valid pointer.
        to_result(unsafe {
            bindings::phy_drivers_register(drivers[0].0.get(), drivers.len().try_into()?, module.0)
        })?;
        // INVARIANT: The `drivers` slice is successfully registered to the kernel via `phy_drivers_register`.
        Ok(Registration { drivers })
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        // SAFETY: The type invariants guarantee that `self.drivers` is valid.
        // So an FFI call with a valid pointer.
        unsafe {
            bindings::phy_drivers_unregister(self.drivers[0].0.get(), self.drivers.len() as i32)
        };
    }
}

// SAFETY: `Registration` does not expose any of its state across threads.
unsafe impl Send for Registration {}

// SAFETY: `Registration` does not expose any of its state across threads.
unsafe impl Sync for Registration {}

/// An identifier for PHY devices on an MDIO/MII bus.
///
/// Represents the kernel's `struct mdio_device_id`. This is used to find an appropriate
/// PHY driver.
pub struct DeviceId {
    id: u32,
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

    /// Get a `mask` as u32.
    pub const fn mask_as_int(&self) -> u32 {
        self.mask.as_int()
    }

    // macro use only
    #[doc(hidden)]
    pub const fn mdio_device_id(&self) -> bindings::mdio_device_id {
        bindings::mdio_device_id {
            phy_id: self.id,
            phy_id_mask: self.mask.as_int(),
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
    const MASK_EXACT: u32 = !0;
    const MASK_MODEL: u32 = !0 << 4;
    const MASK_VENDOR: u32 = !0 << 10;

    const fn as_int(&self) -> u32 {
        match self {
            DeviceMask::Exact => Self::MASK_EXACT,
            DeviceMask::Model => Self::MASK_MODEL,
            DeviceMask::Vendor => Self::MASK_VENDOR,
            DeviceMask::Custom(mask) => *mask,
        }
    }
}
