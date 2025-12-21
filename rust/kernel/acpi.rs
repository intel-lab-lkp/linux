// SPDX-License-Identifier: GPL-2.0

//! Advanced Configuration and Power Interface abstractions.

use crate::{
    bindings,
    device_id::{RawDeviceId, RawDeviceIdIndex},
    prelude::*,
};

/// IdTable type for ACPI drivers.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// An ACPI device id.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::acpi_device_id);

// SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `acpi_device_id` and does not add
// additional invariants, so it's safe to transmute to `RawType`.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::acpi_device_id;
}

// SAFETY: `DRIVER_DATA_OFFSET` is the offset to the `driver_data` field.
unsafe impl RawDeviceIdIndex for DeviceId {
    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::acpi_device_id, driver_data);

    fn index(&self) -> usize {
        self.0.driver_data
    }
}

impl DeviceId {
    const ACPI_ID_LEN: usize = 16;

    /// Create a new device id from an ACPI 'id' string.
    #[inline(always)]
    pub const fn new(id: &'static CStr) -> Self {
        let src = id.to_bytes_with_nul();
        build_assert!(src.len() <= Self::ACPI_ID_LEN, "ID exceeds 16 bytes");
        let mut acpi: bindings::acpi_device_id = pin_init::zeroed();
        let mut i = 0;
        while i < src.len() {
            acpi.id[i] = src[i];
            i += 1;
        }

        Self(acpi)
    }
}

/// Create an ACPI `IdTable` with an "alias" for modpost.
#[macro_export]
macro_rules! acpi_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::acpi::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("acpi", $module_table_name, $table_name);
    };
}

/// An ACPI object.
///
/// This structure represents the Rust abstraction for a C [`struct acpi_object`].
/// You probably want to convert it into actual object type.
///
/// # Example
/// ```
/// # use kernel::prelude::*
/// use kernel::acpi::{AcpiObject};
///
/// fn read_first_acpi_byte(obj: &AcpiObject) -> Result<u8> {
///     if obj.type_id() != AcpiBuffer::ACPI_TYPE {
///         return Err(EINVAL);
///     }
///
///     let obj: &AcpiBuffer = obj.try_into()?;
///
///     Ok(obj.payload()[0])
/// }
/// ```
#[repr(transparent)]
pub struct AcpiObject(bindings::acpi_object);

impl AcpiObject {
    /// Returns object type (see `acpitypes.h`)
    pub fn type_id(&self) -> u32 {
        // SAFETY: `type` field is valid in all union variants
        unsafe { self.0.type_ }
    }
}

/// Generate AcpiObject subtype
///
/// For given subtype implements
/// - `TryFrom<&AcpiObject> for &SubType` trait
/// - unsafe try_from_unchecked() with same semantics, but without type check
macro_rules! acpi_object_subtype {
    ($subtype_name:ident <- ($acpi_type:ident, $field_name:ident, $union_type:ty)) => {
        /// Wraps `acpi_object` subtype
        #[repr(transparent)]
        pub struct $subtype_name($union_type);

        impl TryFrom<&AcpiObject> for &$subtype_name {
            type Error = Error;

            fn try_from(value: &AcpiObject) -> core::result::Result<Self, Self::Error> {
                // SAFETY: type_ field present in all union types and is always valid
                let real_type = unsafe { value.0.type_ };

                if (real_type != $subtype_name::ACPI_TYPE) {
                    return Err(EINVAL);
                }

                // SAFETY: We validated union subtype
                Ok(unsafe {
                    ::core::mem::transmute::<&$union_type, &$subtype_name>(&value.0.$field_name)
                })
            }
        }

        impl $subtype_name {
            /// This ACPI type int value (see `acpitypes.h`)
            pub const ACPI_TYPE: u32 = bindings::$acpi_type;

            /// Converts AcpiObject reference into exact ACPI type wrapper
            ///
            /// # Safety
            ///
            /// Assumes that value is correct (`Self`) subtype
            pub unsafe fn try_from_unchecked(value: &AcpiObject) -> &Self {
                // SAFETY: Only unsafety comes from unchecked transformation and
                // we transfered
                unsafe {
                    ::core::mem::transmute::<&$union_type, &$subtype_name>(&value.0.$field_name)
                }
            }
        }
    };
}

acpi_object_subtype!(AcpiInteger
    <- (ACPI_TYPE_INTEGER, integer, bindings::acpi_object__bindgen_ty_1));
acpi_object_subtype!(AcpiString
    <- (ACPI_TYPE_STRING, string, bindings::acpi_object__bindgen_ty_2));
acpi_object_subtype!(AcpiBuffer
    <- (ACPI_TYPE_BUFFER, buffer, bindings::acpi_object__bindgen_ty_3));
acpi_object_subtype!(AcpiPackage
    <- (ACPI_TYPE_PACKAGE, package, bindings::acpi_object__bindgen_ty_4));
acpi_object_subtype!(AcpiReference
    <- (ACPI_TYPE_LOCAL_REFERENCE, reference, bindings::acpi_object__bindgen_ty_5));
acpi_object_subtype!(AcpiProcessor
    <- (ACPI_TYPE_PROCESSOR, processor, bindings::acpi_object__bindgen_ty_6));
acpi_object_subtype!(AcpiPowerResource
    <- (ACPI_TYPE_POWER, power_resource, bindings::acpi_object__bindgen_ty_7));

impl AcpiBuffer {
    /// Get Buffer's content
    pub fn payload(&self) -> &[u8] {
        // SAFETY: (pointer, length) indeed represents byte slice
        unsafe { ::core::slice::from_raw_parts(self.0.pointer, self.0.length as usize) }
    }
}
