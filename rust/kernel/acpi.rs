// SPDX-License-Identifier: GPL-2.0

//! Advanced Configuration and Power Interface abstractions.

use core::{
    marker::PhantomData,
    ops::Deref, //
};

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
/// You probably want to convert it into actual object type (e.g [`AcpiBuffer`]).
///
/// # Example
/// ```
/// # use kernel::prelude::*;
/// use kernel::acpi::{AcpiObject, AcpiBuffer};
///
/// fn read_first_acpi_byte(obj: &AcpiObject) -> Result<u8> {
///     let buf: &AcpiBuffer = obj.try_into()?;
///
///     Ok(buf[0])
/// }
/// ```
///
/// [`struct acpi_object`]: srctree/include/acpi/actypes.h
#[repr(transparent)]
pub struct AcpiObject<'a> {
    inner: bindings::acpi_object,
    _p: PhantomData<&'a bindings::acpi_object>,
}

impl AcpiObject<'_> {
    /// Returns object type id (see [`actypes.h`](srctree/include/acpi/actypes.h)).
    pub fn type_id(&self) -> u32 {
        // SAFETY: `type` field is valid in all union variants.
        unsafe { self.inner.type_ }
    }
}

/// Generate wrapper type for AcpiObject subtype.
///
/// For given subtype implements
/// - `#[repr(transparent)]` type wrapper,
/// - `TryFrom<&AcpiObject> for &SubType` trait,
/// - unsafe from_unchecked() for 'trusted' conversion.
macro_rules! acpi_object_subtype {
    ($subtype_name:ident <- ($acpi_type:ident, $field_name:ident, $union_type:ty)) => {
        /// Wraps `acpi_object` subtype.
        #[repr(transparent)]
        pub struct $subtype_name($union_type);

        impl<'a> TryFrom<&'a AcpiObject<'a>> for &'a $subtype_name {
            type Error = Error;

            fn try_from(value: &'a AcpiObject<'a>) -> core::result::Result<Self, Self::Error> {
                if (value.type_id() != $subtype_name::ACPI_TYPE) {
                    return Err(EINVAL);
                }

                // SAFETY: Requested cast is valid because we validated type_id
                Ok(unsafe { $subtype_name::from_unchecked(&value) })
            }
        }

        impl $subtype_name {
            /// Int value, representing this ACPI type (see [`acpitypes.h`]).
            ///
            /// [`acpitypes.h`]: srctree/include/linux/acpitypes.h
            pub const ACPI_TYPE: u32 = bindings::$acpi_type;

            /// Converts opaque AcpiObject reference into exact ACPI type reference.
            ///
            /// # Safety
            ///
            /// - Requested cast should be valid (value.type_id() is `Self::ACPI_TYPE`).
            pub unsafe fn from_unchecked<'a>(value: &'a AcpiObject<'a>) -> &'a Self {
                // SAFETY:
                // - $field_name is currently active union's field due to external safety contract,
                // - Transmuting to `repr(transparent)` wrapper is safe.
                unsafe {
                    ::core::mem::transmute::<&$union_type, &$subtype_name>(&value.inner.$field_name)
                }
            }
        }
    };
}

acpi_object_subtype!(AcpiBuffer
    <- (ACPI_TYPE_BUFFER, buffer, bindings::acpi_object__bindgen_ty_3));

impl Deref for AcpiBuffer {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        // SAFETY: (pointer, length) indeed represents byte slice.
        unsafe { ::core::slice::from_raw_parts(self.0.pointer, self.0.length as usize) }
    }
}
