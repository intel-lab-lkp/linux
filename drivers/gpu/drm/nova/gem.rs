// SPDX-License-Identifier: GPL-2.0

use kernel::{
    drm::{
        gem,
        gem::{BaseObject, ObjectRef},
    },
    prelude::*,
};

use crate::{
    driver::{NovaDevice, NovaDriver},
    file::DrmFile,
};

/// GEM Object inner driver data
#[pin_data]
pub(crate) struct DriverObject {}

/// Type alias for the GEM object tyoe for this driver.
pub(crate) type Object = gem::Object<DriverObject>;

impl gem::BaseDriverObject<Object> for DriverObject {
    fn new(dev: &NovaDevice, _size: usize) -> impl PinInit<Self, Error> {
        dev_dbg!(dev.as_ref(), "DriverObject::new\n");
        DriverObject {}
    }
}

impl gem::DriverObject for DriverObject {
    type Driver = NovaDriver;
}

/// Create a new DRM GEM object.
pub(crate) fn object_new(dev: &NovaDevice, size: usize) -> Result<ObjectRef<Object>> {
    let aligned_size = size.next_multiple_of(1 << 12);

    if size == 0 || size > aligned_size {
        return Err(EINVAL);
    }

    let gem = Object::new(dev, aligned_size)?;

    Ok(ObjectRef::from_pinned_unique(gem))
}

/// Look up a GEM object handle for a `File` and return an `ObjectRef` for it.
pub(crate) fn lookup_handle(file: &DrmFile, handle: u32) -> Result<ObjectRef<Object>> {
    Object::lookup_handle(file, handle)
}
