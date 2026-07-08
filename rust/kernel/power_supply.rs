// SPDX-License-Identifier: GPL-2.0

//! Power supply class abstraction.
//!
//! C header: [`include/linux/power_supply.h`](srctree/include/linux/power_supply.h)

use crate::{
    bindings,
    device::{self, Device},
    error::*,
    prelude::*,
};

/// A power-supply property id.
pub use bindings::power_supply_property as Property;
/// The value returned for a property query.
pub use bindings::power_supply_propval as PropertyValue;
/// A power-supply type.
pub use bindings::power_supply_type as Type;

/// `POWER_SUPPLY_PROP_STATUS`.
pub const PROP_STATUS: Property = bindings::power_supply_property_POWER_SUPPLY_PROP_STATUS;
/// `POWER_SUPPLY_PROP_ONLINE`.
pub const PROP_ONLINE: Property = bindings::power_supply_property_POWER_SUPPLY_PROP_ONLINE;
/// `POWER_SUPPLY_TYPE_MAINS`.
pub const TYPE_MAINS: Type = bindings::power_supply_type_POWER_SUPPLY_TYPE_MAINS;
/// `POWER_SUPPLY_STATUS_CHARGING`.
pub const STATUS_CHARGING: i32 = bindings::POWER_SUPPLY_STATUS_CHARGING as i32;
/// `POWER_SUPPLY_STATUS_NOT_CHARGING`.
pub const STATUS_NOT_CHARGING: i32 = bindings::POWER_SUPPLY_STATUS_NOT_CHARGING as i32;
/// `POWER_SUPPLY_PROP_CHARGE_TYPE`.
pub const PROP_CHARGE_TYPE: Property =
    bindings::power_supply_property_POWER_SUPPLY_PROP_CHARGE_TYPE;
/// `POWER_SUPPLY_CHARGE_TYPE_NONE`.
pub const CHARGE_TYPE_NONE: i32 =
    bindings::power_supply_charge_type_POWER_SUPPLY_CHARGE_TYPE_NONE as i32;
/// `POWER_SUPPLY_CHARGE_TYPE_TRICKLE`.
pub const CHARGE_TYPE_TRICKLE: i32 =
    bindings::power_supply_charge_type_POWER_SUPPLY_CHARGE_TYPE_TRICKLE as i32;
/// `POWER_SUPPLY_CHARGE_TYPE_FAST`.
pub const CHARGE_TYPE_FAST: i32 =
    bindings::power_supply_charge_type_POWER_SUPPLY_CHARGE_TYPE_FAST as i32;

/// A driver backing a power supply.
pub trait Driver {
    /// sysfs name: appears at `/sys/class/power_supply/<NAME>`.
    const NAME: &'static CStr;

    /// The power-supply type (mains, USB, battery, …).
    const TYPE: Type;

    /// The properties this supply can answer.
    const PROPERTIES: &'static [Property];

    /// Answer a single property query.
    fn get_property(self: Pin<&Self>, psp: Property, val: &mut PropertyValue) -> Result;
}

/// `get_property` trampoline: the C power supply core calls this and it
/// dispatches to [`Driver::get_property`].
///
/// # Safety
///
/// May only be installed as the `get_property` field of a `power_supply_desc`
/// registered with `drv_data` set to the parent `*mut device`. The core
/// guarantees `psy` and `val` are valid for the duration of the call.
unsafe extern "C" fn get_property_trampoline<T: Driver + 'static>(
    psy: *mut bindings::power_supply,
    psp: Property,
    val: *mut PropertyValue,
) -> kernel::ffi::c_int {
    // SAFETY: core passes a valid `psy`; we stored the parent device ptr as its drv_data.
    let dev_ptr = unsafe { bindings::power_supply_get_drvdata(psy) }.cast::<bindings::device>();
    // SAFETY: that device is valid while the (devm) supply lives.
    let dev: &Device<device::CoreInternal<'_>> = unsafe { Device::from_raw(dev_ptr) };
    // SAFETY: callbacks only fire after probe returned, so set_drvdata stored a `T`.
    let data = unsafe { dev.drvdata_borrow::<T>() };
    // SAFETY: core passes a valid `val` for the call.
    let val = unsafe { &mut *val };
    from_result(|| {
        data.get_property(psp, val)?;
        Ok(0)
    })
}

/// A registered power supply. Unregisters and frees its descriptor on drop.
pub struct Registration {
    psy: *mut bindings::power_supply,
    // Kept alive so the descriptor the core holds stays valid until unregister.
    _desc: KBox<bindings::power_supply_desc>,
}

// SAFETY: `Registration` owns the `power_supply` and its descriptor; the raw
// pointer is only used to unregister on drop. Safe to send/share across threads.
unsafe impl Send for Registration {}
// SAFETY: as above — psy and descriptor are only accessed on drop, so sharing
// a `&Registration` across threads is sound.
unsafe impl Sync for Registration {}

impl Drop for Registration {
    fn drop(&mut self) {
        // SAFETY: `psy` came from a successful `power_supply_register` and has not been
        // unregistered. `power_supply_unregister` blocks until no callback is running;
        // `_desc` (dropped immediately after) is then no longer referenced by the core.
        unsafe { bindings::power_supply_unregister(self.psy) };
    }
}

/// Register a power supply backed by driver `T` against `dev`.
pub fn register<T: Driver + 'static>(dev: &Device) -> Result<Registration> {
    let mut desc = KBox::new(bindings::power_supply_desc::default(), GFP_KERNEL)?;
    desc.name = T::NAME.as_char_ptr();
    desc.type_ = T::TYPE;
    desc.properties = T::PROPERTIES.as_ptr();
    desc.num_properties = T::PROPERTIES.len();
    desc.get_property = Some(get_property_trampoline::<T>);

    // Stable heap address; moving the KBox into `Registration` keeps this valid
    // (moving a KBox moves the pointer, not the heap allocation).
    let desc_ptr: *const bindings::power_supply_desc = &*desc;

    let cfg = bindings::power_supply_config {
        drv_data: dev.as_raw().cast::<kernel::ffi::c_void>(),
        ..Default::default()
    };

    // SAFETY: `dev` valid; `desc_ptr` points into `desc`, kept alive in the returned
    // `Registration` and freed only after `power_supply_unregister`; `cfg` valid for the call.
    let psy = unsafe { bindings::power_supply_register(dev.as_raw(), desc_ptr, &cfg) };
    let psy = from_err_ptr(psy)?;

    Ok(Registration { psy, _desc: desc })
}
