// SPDX-License-Identifier: GPL-2.0

//! Device Tree / Open Firmware abstractions.

use crate::{
    bindings,
    device_id::{RawDeviceId, RawDeviceIdIndex},
    prelude::*,
    sync::aref::AlwaysRefCounted,
    types::Opaque,
};
use core::ptr::NonNull;

/// IdTable type for OF drivers.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// An open firmware device id.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::of_device_id);

// SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `struct of_device_id` and
// does not add additional invariants, so it's safe to transmute to `RawType`.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::of_device_id;
}

// SAFETY: `DRIVER_DATA_OFFSET` is the offset to the `data` field.
unsafe impl RawDeviceIdIndex for DeviceId {
    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::of_device_id, data);

    fn index(&self) -> usize {
        self.0.data as usize
    }
}

impl DeviceId {
    /// Create a new device id from an OF 'compatible' string.
    pub const fn new(compatible: &'static CStr) -> Self {
        let src = compatible.to_bytes_with_nul();
        // Replace with `bindings::of_device_id::default()` once stabilized for `const`.
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut of: bindings::of_device_id = unsafe { core::mem::zeroed() };

        // TODO: Use `copy_from_slice` once stabilized for `const`.
        let mut i = 0;
        while i < src.len() {
            of.compatible[i] = src[i];
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

/// A device tree node (`struct device_node`).
///
/// # Invariants
///
/// The inner pointer is always a valid, non-null pointer to a `struct device_node`
/// with a positive reference count.
#[repr(transparent)]
pub struct Node(Opaque<bindings::device_node>);

impl Node {
    /// Creates a reference from a raw pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid, non-null `struct device_node` pointer that remains
    /// valid for the lifetime `'a`.
    pub unsafe fn from_raw<'a>(ptr: *const bindings::device_node) -> &'a Self {
        // SAFETY: Caller guarantees `ptr` is valid and lives for `'a`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the raw pointer to the underlying `struct device_node`.
    pub fn as_raw(&self) -> *const bindings::device_node {
        self.0.get() as _
    }
}

// SAFETY: By the type invariants, this type is always refcounted.
unsafe impl AlwaysRefCounted for Node {
    fn inc_ref(&self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::of_node_get(self.as_raw().cast_mut()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::of_node_put(obj.cast().as_ptr()) };
    }
}
