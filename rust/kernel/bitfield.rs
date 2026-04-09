// SPDX-License-Identifier: GPL-2.0

//! Support for defining bitfields as Rust structures.
//!
//! The [`bitfield!`](kernel::bitfield!) macro declares integer types that are split into distinct
//! bit fields of arbitrary length. Each field is typed using [`Bounded`](kernel::num::Bounded) to
//! ensure values are properly validated and to avoid implicit data loss.
//!
//! # Example
//!
//! ```rust
//! use kernel::bitfield;
//! use kernel::num::Bounded;
//!
//! bitfield! {
//!     pub struct Rgb(u16) {
//!         15:11 blue;
//!         10:5 green;
//!         4:0 red;
//!     }
//! }
//!
//! // Valid value for the `blue` field.
//! let blue = Bounded::<u16, 5>::new::<0x18>();
//!
//! // Setters can be chained. Values ranges are checked at compile-time.
//! let color = Rgb::zeroed()
//!     // Compile-time bounds check of constant value.
//!     .with_const_red::<0x10>()
//!     .with_const_green::<0x1f>()
//!     // A `Bounded` can also be passed.
//!     .with_blue(blue);
//!
//! assert_eq!(color.red(), 0x10);
//! assert_eq!(color.green(), 0x1f);
//! assert_eq!(color.blue(), 0x18);
//! assert_eq!(
//!     color.into_raw(),
//!     (0x18 << Rgb::BLUE_SHIFT) + (0x1f << Rgb::GREEN_SHIFT) + 0x10,
//! );
//!
//! // Convert to/from the backing storage type.
//! let raw: u16 = color.into();
//! assert_eq!(Rgb::from(raw), color);
//! ```
//!
//! # Syntax
//!
//! ```text
//! bitfield! {
//!     #[attributes]
//!     // Documentation for `Name`.
//!     pub struct Name(storage_type) {
//!         // `field_1` documentation.
//!         hi:lo field_1;
//!         // `field_2` documentation.
//!         hi:lo field_2 => ConvertedType;
//!         // `field_3` documentation.
//!         hi:lo field_3 ?=> ConvertedType;
//!         ...
//!     }
//! }
//! ```
//!
//! - `storage_type`: The underlying integer type (`u8`, `u16`, `u32`, `u64`).
//! - `hi:lo`: Bit range (inclusive), where `hi >= lo`.
//! - `=> Type`: Optional infallible conversion (see [below](#infallible-conversion-)).
//! - `?=> Type`: Optional fallible conversion (see [below](#fallible-conversion-)).
//! - Documentation strings and attributes are optional.
//!
//! # Generated code
//!
//! Each field is internally represented as a [`Bounded`] parameterized by its bit width. Field
//! values can either be set/retrieved directly, or converted from/to another type.
//!
//! The use of `Bounded` for each field enforces bounds-checking (at build time or runtime) of every
//! value assigned to a field. This ensures that data is never accidentally truncated.
//!
//! The macro generates the bitfield type, [`From`] and [`Into`] implementations for its storage
//! type, as well as [`Debug`] and [`Zeroable`](pin_init::Zeroable) implementations.
//!
//! For each field, it also generates:
//!
//! - `with_field(value)` — infallible setter; the argument type must be statically known to fit
//!   the field width.
//! - `with_const_field::<VALUE>()` — const setter; the value is validated at compile time.
//!   Usually shorter to use than `with_field` for constant values as it doesn't require
//!   constructing a `Bounded`.
//! - `try_with_field(value)` — fallible setter. Returns an error if the value is out of range.
//! - `FIELD_MASK`, `FIELD_SHIFT`, `FIELD_RANGE` - constants for manual bit manipulation.
//!
//! # Implicit conversions
//!
//! Types that fit entirely within a field's bit width can be used directly with setters. For
//! example, `bool` works with single-bit fields, and `u8` works with 8-bit fields:
//!
//! ```rust
//! use kernel::bitfield;
//!
//! bitfield! {
//!     pub struct Flags(u32) {
//!         15:8 byte_field;
//!         0:0 flag;
//!     }
//! }
//!
//! let flags = Flags::zeroed()
//!     .with_byte_field(0x42_u8)
//!     .with_flag(true);
//!
//! assert_eq!(flags.into_raw(), (0x42 << Flags::BYTE_FIELD_SHIFT) | 1);
//! ```
//!
//! # Runtime bounds checking
//!
//! When a value is not known at compile time, use `try_with_field()` to check bounds at runtime:
//!
//! ```rust
//! use kernel::bitfield;
//!
//! bitfield! {
//!     pub struct Config(u8) {
//!         3:0 nibble;
//!     }
//! }
//!
//! fn set_nibble(config: Config, value: u8) -> Result<Config, Error> {
//!     // Returns `EOVERFLOW` if `value > 0xf`.
//!     config.try_with_nibble(value)
//! }
//! # Ok::<(), Error>(())
//! ```
//!
//! # Type conversion
//!
//! Fields can be automatically converted to/from a custom type using `=>` (infallible) or `?=>`
//! (fallible). The custom type must implement the appropriate `From` or `TryFrom` traits with
//! `Bounded`.
//!
//! ## Infallible conversion (`=>`)
//!
//! Use this when all possible bit patterns of a field map to valid values:
//!
//! ```rust
//! use kernel::bitfield;
//! use kernel::num::Bounded;
//!
//! #[derive(Debug, Clone, Copy, PartialEq)]
//! enum Power {
//!     Off,
//!     On,
//! }
//!
//! impl From<Bounded<u32, 1>> for Power {
//!     fn from(v: Bounded<u32, 1>) -> Self {
//!         match *v {
//!             0 => Power::Off,
//!             _ => Power::On,
//!         }
//!     }
//! }
//!
//! impl From<Power> for Bounded<u32, 1> {
//!     fn from(p: Power) -> Self {
//!         (p as u32 != 0).into()
//!     }
//! }
//!
//! bitfield! {
//!     pub struct Control(u32) {
//!         0:0 power => Power;
//!     }
//! }
//!
//! let ctrl = Control::zeroed().with_power(Power::On);
//! assert_eq!(ctrl.power(), Power::On);
//! ```
//!
//! ## Fallible conversion (`?=>`)
//!
//! Use this when some bit patterns of a field are invalid. The getter returns a [`Result`]:
//!
//! ```rust
//! use kernel::bitfield;
//! use kernel::num::Bounded;
//!
//! #[derive(Debug, Clone, Copy, PartialEq)]
//! enum Mode {
//!     Low = 0,
//!     High = 1,
//!     Auto = 2,
//!     // 3 is invalid
//! }
//!
//! impl TryFrom<Bounded<u32, 2>> for Mode {
//!     type Error = u32;
//!
//!     fn try_from(v: Bounded<u32, 2>) -> Result<Self, u32> {
//!         match *v {
//!             0 => Ok(Mode::Low),
//!             1 => Ok(Mode::High),
//!             2 => Ok(Mode::Auto),
//!             n => Err(n),
//!         }
//!     }
//! }
//!
//! impl From<Mode> for Bounded<u32, 2> {
//!     fn from(m: Mode) -> Self {
//!         match m {
//!             Mode::Low => Bounded::<u32, _>::new::<0>(),
//!             Mode::High => Bounded::<u32, _>::new::<1>(),
//!             Mode::Auto => Bounded::<u32, _>::new::<2>(),
//!         }
//!     }
//! }
//!
//! bitfield! {
//!     pub struct Config(u32) {
//!         1:0 mode ?=> Mode;
//!     }
//! }
//!
//! let cfg = Config::zeroed().with_mode(Mode::Auto);
//! assert_eq!(cfg.mode(), Ok(Mode::Auto));
//!
//! // Invalid bit pattern returns an error.
//! assert_eq!(Config::from(0b11).mode(), Err(3));
//! ```
//!
//! [`Bounded`]: kernel::num::Bounded

/// Defines a bitfield struct with bounds-checked accessors for individual bit ranges.
///
/// See the [`mod@kernel::bitfield`] module for full documentation and examples.
#[macro_export]
macro_rules! bitfield {
    // Entry point defining the bitfield struct, its implementations and its field accessors.
    (
        $(#[$attr:meta])* $vis:vis struct $name:ident($storage:ty) { $($fields:tt)* }
    ) => {
        $crate::bitfield!(@core
            #[allow(non_camel_case_types)]
            $(#[$attr])* $vis $name $storage
        );
        $crate::bitfield!(@fields $vis $name $storage { $($fields)* });
    };

    // All rules below are helpers.

    // Defines the wrapper `$name` type and its conversions from/to the storage type.
    (@core $(#[$attr:meta])* $vis:vis $name:ident $storage:ty) => {
        $(#[$attr])*
        #[repr(transparent)]
        #[derive(Clone, Copy, PartialEq, Eq)]
        $vis struct $name {
            inner: $storage,
        }

        #[allow(dead_code)]
        impl $name {
            /// Creates a bitfield from a raw value.
            #[inline(always)]
            $vis const fn from_raw(value: $storage) -> Self {
                Self{ inner: value }
            }

            /// Turns this bitfield into its raw value.
            ///
            /// This is similar to the [`From`] implementation, but is shorter to invoke in
            /// most cases.
            #[inline(always)]
            $vis const fn into_raw(self) -> $storage {
                self.inner
            }
        }

        // SAFETY: `$storage` is `Zeroable` and `$name` is transparent.
        unsafe impl ::pin_init::Zeroable for $name {}

        impl ::core::convert::From<$name> for $storage {
            #[inline(always)]
            fn from(val: $name) -> $storage {
                val.into_raw()
            }
        }

        impl ::core::convert::From<$storage> for $name {
            #[inline(always)]
            fn from(val: $storage) -> $name {
                Self::from_raw(val)
            }
        }
    };

    // Definitions requiring knowledge of individual fields: private and public field accessors,
    // and `Debug` implementation.
    (@fields $vis:vis $name:ident $storage:ty {
        $($(#[doc = $doc:expr])* $hi:literal:$lo:literal $field:ident
            $(?=> $try_into_type:ty)?
            $(=> $into_type:ty)?
        ;
        )*
    }
    ) => {
        #[allow(dead_code)]
        impl $name {
        $(
        $crate::bitfield!(@private_field_accessors $vis $name $storage : $hi:$lo $field);
        $crate::bitfield!(
            @public_field_accessors $(#[doc = $doc])* $vis $name $storage : $hi:$lo $field
            $(?=> $try_into_type)?
            $(=> $into_type)?
        );
        )*
        }

        $crate::bitfield!(@debug $name { $($field;)* });
    };

    // Private field accessors working with the exact `Bounded` type for the field.
    (
        @private_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
    ) => {
        ::kernel::macros::paste!(
        $vis const [<$field:upper _RANGE>]: ::core::ops::RangeInclusive<u8> = $lo..=$hi;
        $vis const [<$field:upper _MASK>]: $storage =
            ((((1 << $hi) - 1) << 1) + 1) - ((1 << $lo) - 1);
        $vis const [<$field:upper _SHIFT>]: u32 = $lo;
        );

        ::kernel::macros::paste!(
        fn [<__ $field>](self) ->
            ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }> {
            // Left shift to align the field's MSB with the storage MSB.
            const ALIGN_TOP: u32 = $storage::BITS - ($hi + 1);
            // Right shift to move the top-aligned field to bit 0 of the storage.
            const ALIGN_BOTTOM: u32 = ALIGN_TOP + $lo;

            // Extract the field using two shifts. `Bounded::shr` produces the correctly-sized
            // output type.
            let val = ::kernel::num::Bounded::<$storage, { $storage::BITS }>::from(
                self.inner << ALIGN_TOP
            );
            val.shr::<ALIGN_BOTTOM, { $hi + 1 - $lo } >()
        }

        const fn [<__with_ $field>](
            mut self,
            value: ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>,
        ) -> Self
        {
            const MASK: $storage = <$name>::[<$field:upper _MASK>];
            const SHIFT: u32 = <$name>::[<$field:upper _SHIFT>];

            let value = value.get() << SHIFT;
            self.inner = (self.inner & !MASK) | value;

            self
        }
        );
    };

    // Public accessors for fields infallibly (`=>`) converted to a type.
    (
        @public_field_accessors $(#[doc = $doc:expr])* $vis:vis $name:ident $storage:ty :
            $hi:literal:$lo:literal $field:ident => $into_type:ty
    ) => {
        ::kernel::macros::paste!(

        $(#[doc = $doc])*
        #[doc = "Returns the value of this field."]
        #[inline(always)]
        $vis fn $field(self) -> $into_type
        {
            self.[<__ $field>]().into()
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the given `value`."]
        #[inline(always)]
        $vis fn [<with_ $field>](self, value: $into_type) -> Self
        {
            self.[<__with_ $field>](value.into())
        }

        );
    };

    // Public accessors for fields fallibly (`?=>`) converted to a type.
    (
        @public_field_accessors $(#[doc = $doc:expr])* $vis:vis $name:ident $storage:ty :
            $hi:tt:$lo:tt $field:ident ?=> $try_into_type:ty
    ) => {
        ::kernel::macros::paste!(

        $(#[doc = $doc])*
        #[doc = "Returns the value of this field."]
        #[inline(always)]
        $vis fn $field(self) ->
            Result<
                $try_into_type,
                <$try_into_type as ::core::convert::TryFrom<
                    ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>
                >>::Error
            >
        {
            self.[<__ $field>]().try_into()
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the given `value`."]
        #[inline(always)]
        $vis fn [<with_ $field>](self, value: $try_into_type) -> Self
        {
            self.[<__with_ $field>](value.into())
        }

        );
    };

    // Public accessors for fields not converted to a type.
    (
        @public_field_accessors $(#[doc = $doc:expr])* $vis:vis $name:ident $storage:ty :
            $hi:tt:$lo:tt $field:ident
    ) => {
        ::kernel::macros::paste!(

        $(#[doc = $doc])*
        #[doc = "Returns the value of this field."]
        #[inline(always)]
        $vis fn $field(self) ->
            ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>
        {
            self.[<__ $field>]()
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the compile-time constant `VALUE`."]
        #[inline(always)]
        $vis const fn [<with_const_ $field>]<const VALUE: $storage>(self) -> Self {
            self.[<__with_ $field>](
                ::kernel::num::Bounded::<$storage, { $hi + 1 - $lo }>::new::<VALUE>()
            )
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the given `value`."]
        #[inline(always)]
        $vis fn [<with_ $field>]<T>(
            self,
            value: T,
        ) -> Self
            where T: Into<::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>>,
        {
            self.[<__with_ $field>](value.into())
        }

        $(#[doc = $doc])*
        #[doc = "Tries to set this field to `value`, returning an error if it is out of range."]
        #[inline(always)]
        $vis fn [<try_with_ $field>]<T>(
            self,
            value: T,
        ) -> ::kernel::error::Result<Self>
            where T: ::kernel::num::TryIntoBounded<$storage, { $hi + 1 - $lo }>,
        {
            Ok(
                self.[<__with_ $field>](
                    value.try_into_bounded().ok_or(::kernel::error::code::EOVERFLOW)?
                )
            )
        }

        );
    };

    // `Debug` implementation.
    (@debug $name:ident { $($field:ident;)* }) => {
        impl ::kernel::fmt::Debug for $name {
            fn fmt(&self, f: &mut ::kernel::fmt::Formatter<'_>) -> ::kernel::fmt::Result {
                f.debug_struct(stringify!($name))
                    .field("<raw>", &::kernel::prelude::fmt!("{:#x}", self.inner))
                $(
                    .field(stringify!($field), &self.$field())
                )*
                    .finish()
            }
        }
    };
}

#[::kernel::macros::kunit_tests(kernel_bitfield)]
mod tests {
    use core::convert::TryFrom;

    use pin_init::Zeroable;

    use kernel::num::Bounded;

    // Enum types for testing => and ?=> conversions
    #[derive(Debug, Clone, Copy, PartialEq)]
    enum MemoryType {
        Unmapped = 0,
        Normal = 1,
        Device = 2,
        Reserved = 3,
    }

    impl TryFrom<Bounded<u64, 4>> for MemoryType {
        type Error = u64;
        fn try_from(value: Bounded<u64, 4>) -> Result<Self, Self::Error> {
            match value.get() {
                0 => Ok(MemoryType::Unmapped),
                1 => Ok(MemoryType::Normal),
                2 => Ok(MemoryType::Device),
                3 => Ok(MemoryType::Reserved),
                _ => Err(value.get()),
            }
        }
    }

    impl From<MemoryType> for Bounded<u64, 4> {
        fn from(mt: MemoryType) -> Bounded<u64, 4> {
            Bounded::from_expr(mt as u64)
        }
    }

    #[derive(Debug, Clone, Copy, PartialEq)]
    enum Priority {
        Low = 0,
        Medium = 1,
        High = 2,
        Critical = 3,
    }

    impl From<Bounded<u16, 2>> for Priority {
        fn from(value: Bounded<u16, 2>) -> Self {
            match value & 0x3 {
                0 => Priority::Low,
                1 => Priority::Medium,
                2 => Priority::High,
                _ => Priority::Critical,
            }
        }
    }

    impl From<Priority> for Bounded<u16, 2> {
        fn from(p: Priority) -> Bounded<u16, 2> {
            Bounded::from_expr(p as u16)
        }
    }

    bitfield! {
        struct TestPageTableEntry(u64) {
            0:0       present;
            1:1       writable;
            11:9      available;
            15:12     mem_type ?=> MemoryType;
            51:16     pfn;
            61:52     available2;
        }
    }

    bitfield! {
        struct TestControlRegister(u16) {
            0:0       enable;
            3:1       mode;
            5:4       priority => Priority;
            7:4       priority_nibble;
            15:8      channel;
        }
    }

    bitfield! {
        struct TestStatusRegister(u8) {
            0:0       ready;
            1:1       error;
            3:2       state;
            7:4       reserved;
            7:0       full_byte;  // For entire register
        }
    }

    #[test]
    fn test_single_bits() {
        let mut pte = TestPageTableEntry::zeroed();

        assert!(!pte.present().into_bool());
        assert!(!pte.writable().into_bool());
        assert_eq!(u64::from(pte), 0x0);

        pte = pte.with_present(true);
        assert!(pte.present().into_bool());
        assert_eq!(u64::from(pte), 0x1);

        pte = pte.with_writable(true);
        assert!(pte.writable().into_bool());
        assert_eq!(u64::from(pte), 0x3);

        pte = pte.with_writable(false);
        assert!(!pte.writable().into_bool());
        assert_eq!(u64::from(pte), 0x1);

        assert_eq!(pte.available(), 0);
        pte = pte.with_const_available::<0x5>();
        assert_eq!(pte.available(), 0x5);
        assert_eq!(u64::from(pte), 0xA01);
    }

    #[test]
    fn test_range_fields() {
        let mut pte = TestPageTableEntry::zeroed();
        assert_eq!(u64::from(pte), 0x0);

        pte = pte.with_const_pfn::<0x123456>();
        assert_eq!(pte.pfn(), 0x123456);
        assert_eq!(u64::from(pte), 0x1234560000);

        pte = pte.with_const_available::<0x7>();
        assert_eq!(pte.available(), 0x7);
        assert_eq!(u64::from(pte), 0x1234560E00);

        pte = pte.with_const_available2::<0x3FF>();
        assert_eq!(pte.available2(), 0x3FF);
        assert_eq!(u64::from(pte), 0x3FF0_0012_3456_0E00u64);

        // Test TryFrom with ?=> for MemoryType
        pte = pte.with_mem_type(MemoryType::Device);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Device));
        assert_eq!(u64::from(pte), 0x3FF0_0012_3456_2E00u64);

        pte = pte.with_mem_type(MemoryType::Normal);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Normal));
        assert_eq!(u64::from(pte), 0x3FF0_0012_3456_1E00u64);

        // Test all valid values for mem_type
        pte = pte.with_mem_type(MemoryType::Reserved);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Reserved));
        assert_eq!(u64::from(pte), 0x3FF0_0012_3456_3E00u64);

        // Test failure case using mem_type field which has 4 bits (0-15)
        // MemoryType only handles 0-3, so values 4-15 should return Err
        let mut raw = pte.into_raw();
        // Set bits 15:12 to 7 (invalid for MemoryType)
        raw = (raw & !::kernel::bits::genmask_u64(12..=15)) | (0x7 << 12);
        let invalid_pte = TestPageTableEntry::from_raw(raw);
        // Should return Err with the invalid value
        assert_eq!(invalid_pte.mem_type(), Err(0x7));

        // Test a valid value after testing invalid to ensure both cases work
        // Set bits 15:12 to 2 (valid: Device)
        raw = (raw & !::kernel::bits::genmask_u64(12..=15)) | (0x2 << 12);
        let valid_pte = TestPageTableEntry::from_raw(raw);
        assert_eq!(valid_pte.mem_type(), Ok(MemoryType::Device));

        const MAX_PFN: u64 = ::kernel::bits::genmask_u64(0..=35);
        pte = pte.with_const_pfn::<{ MAX_PFN }>();
        assert_eq!(pte.pfn(), MAX_PFN);
    }

    #[test]
    fn test_builder_pattern() {
        let pte = TestPageTableEntry::zeroed()
            .with_present(true)
            .with_writable(true)
            .with_const_available::<0x7>()
            .with_const_pfn::<0xABCDEF>()
            .with_mem_type(MemoryType::Reserved)
            .with_const_available2::<0x3FF>();

        assert!(pte.present().into_bool());
        assert!(pte.writable().into_bool());
        assert_eq!(pte.available(), 0x7);
        assert_eq!(pte.pfn(), 0xABCDEF);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Reserved));
        assert_eq!(pte.available2(), 0x3FF);
    }

    #[test]
    fn test_raw_operations() {
        let raw_value = 0x3FF0000031233E03u64;

        let pte = TestPageTableEntry::from_raw(raw_value);
        assert_eq!(u64::from(pte), raw_value);

        assert!(pte.present().into_bool());
        assert!(pte.writable().into_bool());
        assert_eq!(pte.available(), 0x7);
        assert_eq!(pte.pfn(), 0x3123);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Reserved));
        assert_eq!(pte.available2(), 0x3FF);

        // Test using direct constructor syntax TestStruct(value)
        let pte2 = TestPageTableEntry::from_raw(raw_value);
        assert_eq!(u64::from(pte2), raw_value);
    }

    #[test]
    fn test_u16_bitfield() {
        let mut ctrl = TestControlRegister::zeroed();

        assert!(!ctrl.enable().into_bool());
        assert_eq!(ctrl.mode(), 0);
        assert_eq!(ctrl.priority(), Priority::Low);
        assert_eq!(ctrl.priority_nibble(), 0);
        assert_eq!(ctrl.channel(), 0);

        ctrl = ctrl.with_enable(true);
        assert!(ctrl.enable().into_bool());

        ctrl = ctrl.with_const_mode::<0x5>();
        assert_eq!(ctrl.mode(), 0x5);

        // Test From conversion with =>
        ctrl = ctrl.with_priority(Priority::High);
        assert_eq!(ctrl.priority(), Priority::High);
        assert_eq!(ctrl.priority_nibble(), 0x2); // High = 2 in bits 5:4

        ctrl = ctrl.with_channel(0xAB);
        assert_eq!(ctrl.channel(), 0xAB);

        // Test overlapping fields
        ctrl = ctrl.with_const_priority_nibble::<0xF>();
        assert_eq!(ctrl.priority_nibble(), 0xF);
        assert_eq!(ctrl.priority(), Priority::Critical); // bits 5:4 = 0x3

        let ctrl2 = TestControlRegister::zeroed()
            .with_enable(true)
            .with_const_mode::<0x3>()
            .with_priority(Priority::Medium)
            .with_channel(0x42);

        assert!(ctrl2.enable().into_bool());
        assert_eq!(ctrl2.mode(), 0x3);
        assert_eq!(ctrl2.priority(), Priority::Medium);
        assert_eq!(ctrl2.channel(), 0x42);

        let raw_value: u16 = 0x4217;
        let ctrl3 = TestControlRegister::from_raw(raw_value);
        assert_eq!(u16::from(ctrl3), raw_value);
        assert!(ctrl3.enable().into_bool());
        assert_eq!(ctrl3.priority(), Priority::Medium);
        assert_eq!(ctrl3.priority_nibble(), 0x1);
        assert_eq!(ctrl3.channel(), 0x42);
    }

    #[test]
    fn test_u8_bitfield() {
        let mut status = TestStatusRegister::zeroed();

        assert!(!status.ready().into_bool());
        assert!(!status.error().into_bool());
        assert_eq!(status.state(), 0);
        assert_eq!(status.reserved(), 0);
        assert_eq!(status.full_byte(), 0);

        status = status.with_ready(true);
        assert!(status.ready().into_bool());
        assert_eq!(status.full_byte(), 0x01);

        status = status.with_error(true);
        assert!(status.error().into_bool());
        assert_eq!(status.full_byte(), 0x03);

        status = status.with_const_state::<0x3>();
        assert_eq!(status.state(), 0x3);
        assert_eq!(status.full_byte(), 0x0F);

        status = status.with_const_reserved::<0xA>();
        assert_eq!(status.reserved(), 0xA);
        assert_eq!(status.full_byte(), 0xAF);

        // Test overlapping field
        status = status.with_full_byte(0x55);
        assert_eq!(status.full_byte(), 0x55);
        assert!(status.ready().into_bool());
        assert!(!status.error().into_bool());
        assert_eq!(status.state(), 0x1);
        assert_eq!(status.reserved(), 0x5);

        let status2 = TestStatusRegister::zeroed()
            .with_ready(true)
            .with_const_state::<0x2>()
            .with_const_reserved::<0x5>();

        assert!(status2.ready().into_bool());
        assert!(!status2.error().into_bool());
        assert_eq!(status2.state(), 0x2);
        assert_eq!(status2.reserved(), 0x5);
        assert_eq!(status2.full_byte(), 0x59);

        let raw_value: u8 = 0x59;
        let status3 = TestStatusRegister::from_raw(raw_value);
        assert_eq!(u8::from(status3), raw_value);
        assert!(status3.ready().into_bool());
        assert!(!status3.error().into_bool());
        assert_eq!(status3.state(), 0x2);
        assert_eq!(status3.reserved(), 0x5);
        assert_eq!(status3.full_byte(), 0x59);

        let status4 = TestStatusRegister::from_raw(0xFF);
        assert!(status4.ready().into_bool());
        assert!(status4.error().into_bool());
        assert_eq!(status4.state(), 0x3);
        assert_eq!(status4.reserved(), 0xF);
        assert_eq!(status4.full_byte(), 0xFF);
    }
}
