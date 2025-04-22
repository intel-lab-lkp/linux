// SPDX-License-Identifier: GPL-2.0

//! Device Tree / Open Firmware abstractions.

use crate::{bindings, device_id::RawDeviceId, prelude::*, types::Opaque};

/// IdTable type for OF drivers.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// An open firmware device id.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::of_device_id);

// SAFETY:
// * `DeviceId` is a `#[repr(transparent)` wrapper of `struct of_device_id` and does not add
//   additional invariants, so it's safe to transmute to `RawType`.
// * `DRIVER_DATA_OFFSET` is the offset to the `data` field.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::of_device_id;

    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::of_device_id, data);

    fn index(&self) -> usize {
        self.0.data as _
    }
}

impl DeviceId {
    /// Create a new device id from an OF 'compatible' string.
    pub const fn new(compatible: &'static CStr) -> Self {
        let src = compatible.as_bytes_with_nul();
        // Replace with `bindings::of_device_id::default()` once stabilized for `const`.
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut of: bindings::of_device_id = unsafe { core::mem::zeroed() };

        // TODO: Use `clone_from_slice` once the corresponding types do match.
        let mut i = 0;
        while i < src.len() {
            of.compatible[i] = src[i] as _;
            i += 1;
        }

        Self(of)
    }
}

/// Create an OF `IdTable` with an "alias" for modpost.
#[macro_export]
macro_rules! of_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::of::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("of", $module_table_name, $table_name);
    };
}

/// Devicetree device node
#[repr(transparent)]
pub struct DeviceNode(Opaque<bindings::device_node>);

impl DeviceNode {
    /// Convert a raw C `struct device_node` pointer to a `&'a DeviceNode`.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null. for the duration of this function call
    /// and the entire duration when the returned reference exists.
    pub unsafe fn as_ref<'a>(ptr: *mut bindings::device_node) -> &'a Self {
        // SAFETY: Guaranteed by the safety requirements of the function.
        unsafe { &*ptr.cast() }
    }

    /// Obtain the raw `struct device_node *`.
    pub fn as_raw(&self) -> *mut bindings::device_node {
        self.0.get()
    }
}

/// Devicetree overlay present in livetree.
///
/// The overlay is removed on Drop
pub struct OvcsId(kernel::ffi::c_int);

impl OvcsId {
    /// Create and apply an overlay changeset to the live tree.
    pub fn of_overlay_fdt_apply(overlay: &[u8], base: &DeviceNode) -> Result<Self> {
        let mut ovcs_id: kernel::ffi::c_int = 0;

        crate::error::to_result(unsafe {
            bindings::of_overlay_fdt_apply(
                overlay.as_ptr().cast(),
                overlay.len().try_into().unwrap(),
                &mut ovcs_id,
                base.as_raw(),
            )
        })?;

        Ok(Self(ovcs_id))
    }
}

impl Drop for OvcsId {
    fn drop(&mut self) {
        unsafe { bindings::of_overlay_remove(&mut self.0) };
    }
}
