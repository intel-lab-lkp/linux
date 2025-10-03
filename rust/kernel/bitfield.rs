// SPDX-License-Identifier: GPL-2.0

//! Bitfield library for Rust structures
//!
//! Support for defining bitfields in Rust structures. Also used by the [`register!`] macro.

/// Defines a struct with accessors to access bits within an inner unsigned integer.
///
/// # Syntax
///
/// ```rust
/// use kernel::bitfield;
///
/// #[derive(Debug, Clone, Copy, Default)]
/// enum Mode {
///     #[default]
///     Low = 0,
///     High = 1,
///     Auto = 2,
/// }
///
/// impl TryFrom<u8> for Mode {
///     type Error = u8;
///     fn try_from(value: u8) -> Result<Self, Self::Error> {
///         match value {
///             0 => Ok(Mode::Low),
///             1 => Ok(Mode::High),
///             2 => Ok(Mode::Auto),
///             _ => Err(value),
///         }
///     }
/// }
///
/// impl From<Mode> for u32 {
///     fn from(mode: Mode) -> u32 {
///         mode as u32
///     }
/// }
///
/// #[derive(Debug, Clone, Copy, Default)]
/// enum State {
///     #[default]
///     Inactive = 0,
///     Active = 1,
/// }
///
/// impl From<bool> for State {
///     fn from(value: bool) -> Self {
///         if value { State::Active } else { State::Inactive }
///     }
/// }
///
/// impl From<State> for u32 {
///     fn from(state: State) -> u32 {
///         state as u32
///     }
/// }
///
/// bitfield! {
///     pub struct ControlReg(u32) {
///         3:0 mode as u8 ?=> Mode;
///         7:7 state as bool => State;
///     }
/// }
/// ```
///
/// This generates a struct with:
/// - Field accessors: `mode()`, `state()`, etc.
/// - Field setters: `set_mode()`, `set_state()`, etc. (supports chaining with builder pattern).
///   Note that the compiler will error out if the size of the setter's arg exceeds the
///   struct's storage size.
/// - Debug and Default implementations.
///
/// Note: Field accessors and setters inherit the same visibility as the struct itself.
/// In the example above, both `mode()` and `set_mode()` methods will be `pub`.
///
/// Fields are defined as follows:
///
/// - `as <type>` simply returns the field value casted to <type>, typically `u32`, `u16`, `u8` or
///   `bool`. Note that `bool` fields must have a range of 1 bit.
/// - `as <type> => <into_type>` calls `<into_type>`'s `From::<<type>>` implementation and returns
///   the result.
/// - `as <type> ?=> <try_into_type>` calls `<try_into_type>`'s `TryFrom::<<type>>` implementation
///   and returns the result. This is useful with fields for which not all values are valid.
#[macro_export]
macro_rules! bitfield {
    // Main entry point - defines the bitfield struct with fields
    ($vis:vis struct $name:ident($storage:ty) $(, $comment:literal)? { $($fields:tt)* }) => {
        ::kernel::bitfield!(@core $vis $name $storage $(, $comment)? { $($fields)* });
    };

    // All rules below are helpers.

    // Defines the wrapper `$name` type, as well as its relevant implementations (`Debug`,
    // `Default`, `BitOr`, and conversion to the value type) and field accessor methods.
    (@core $vis:vis $name:ident $storage:ty $(, $comment:literal)? { $($fields:tt)* }) => {
        $(
        #[doc=$comment]
        )?
        #[repr(transparent)]
        #[derive(Clone, Copy)]
        $vis struct $name($storage);

        impl ::core::ops::BitOr for $name {
            type Output = Self;

            fn bitor(self, rhs: Self) -> Self::Output {
                Self(self.0 | rhs.0)
            }
        }

        impl ::core::convert::From<$name> for $storage {
            fn from(val: $name) -> $storage {
                val.0
            }
        }

        ::kernel::bitfield!(@fields_dispatcher $vis $name $storage { $($fields)* });
    };

    // Captures the fields and passes them to all the implementers that require field information.
    //
    // Used to simplify the matching rules for implementers, so they don't need to match the entire
    // complex fields rule even though they only make use of part of it.
    (@fields_dispatcher $vis:vis $name:ident $storage:ty {
        $($hi:tt:$lo:tt $field:ident as $type:tt
            $(?=> $try_into_type:ty)?
            $(=> $into_type:ty)?
            $(, $comment:literal)?
        ;
        )*
    }
    ) => {
        ::kernel::bitfield!(@field_accessors $vis $name $storage {
            $(
                $hi:$lo $field as $type
                $(?=> $try_into_type)?
                $(=> $into_type)?
                $(, $comment)?
            ;
            )*
        });
        ::kernel::bitfield!(@debug $name { $($field;)* });
        ::kernel::bitfield!(@default $name { $($field;)* });
    };

    // Defines all the field getter/setter methods for `$name`.
    (
        @field_accessors $vis:vis $name:ident $storage:ty {
        $($hi:tt:$lo:tt $field:ident as $type:tt
            $(?=> $try_into_type:ty)?
            $(=> $into_type:ty)?
            $(, $comment:literal)?
        ;
        )*
        }
    ) => {
        $(
            ::kernel::bitfield!(@check_field_bounds $hi:$lo $field as $type);
        )*

        #[allow(dead_code)]
        impl $name {
            $(
            ::kernel::bitfield!(@field_accessor $vis $name $storage, $hi:$lo $field as $type
                $(?=> $try_into_type)?
                $(=> $into_type)?
                $(, $comment)?
                ;
            );
            )*
        }
    };

    // Boolean fields must have `$hi == $lo`.
    (@check_field_bounds $hi:tt:$lo:tt $field:ident as bool) => {
        #[allow(clippy::eq_op)]
        const _: () = {
            ::kernel::build_assert!(
                $hi == $lo,
                concat!("boolean field `", stringify!($field), "` covers more than one bit")
            );
        };
    };

    // Non-boolean fields must have `$hi >= $lo`.
    (@check_field_bounds $hi:tt:$lo:tt $field:ident as $type:tt) => {
        #[allow(clippy::eq_op)]
        const _: () = {
            ::kernel::build_assert!(
                $hi >= $lo,
                concat!("field `", stringify!($field), "`'s MSB is smaller than its LSB")
            );
        };
    };

    // Catches fields defined as `bool` and convert them into a boolean value.
    (
        @field_accessor $vis:vis $name:ident $storage:ty, $hi:tt:$lo:tt $field:ident as bool => $into_type:ty
            $(, $comment:literal)?;
    ) => {
        ::kernel::bitfield!(
            @leaf_accessor $vis $name $storage, $hi:$lo $field
            { |f| <$into_type>::from(if f != 0 { true } else { false }) }
            $into_type => $into_type $(, $comment)?;
        );
    };

    // Shortcut for fields defined as `bool` without the `=>` syntax.
    (
        @field_accessor $vis:vis $name:ident $storage:ty, $hi:tt:$lo:tt $field:ident as bool $(, $comment:literal)?;
    ) => {
        ::kernel::bitfield!(@field_accessor $vis $name $storage, $hi:$lo $field as bool => bool $(, $comment)?;);
    };

    // Catches the `?=>` syntax for non-boolean fields.
    (
        @field_accessor $vis:vis $name:ident $storage:ty, $hi:tt:$lo:tt $field:ident as $type:tt ?=> $try_into_type:ty
            $(, $comment:literal)?;
    ) => {
        ::kernel::bitfield!(@leaf_accessor $vis $name $storage, $hi:$lo $field
            { |f| <$try_into_type>::try_from(f as $type) } $try_into_type =>
            ::core::result::Result<
                $try_into_type,
                <$try_into_type as ::core::convert::TryFrom<$type>>::Error
            >
            $(, $comment)?;);
    };

    // Catches the `=>` syntax for non-boolean fields.
    (
        @field_accessor $vis:vis $name:ident $storage:ty, $hi:tt:$lo:tt $field:ident as $type:tt => $into_type:ty
            $(, $comment:literal)?;
    ) => {
        ::kernel::bitfield!(@leaf_accessor $vis $name $storage, $hi:$lo $field
            { |f| <$into_type>::from(f as $type) } $into_type => $into_type $(, $comment)?;);
    };

    // Shortcut for non-boolean fields defined without the `=>` or `?=>` syntax.
    (
        @field_accessor $vis:vis $name:ident $storage:ty, $hi:tt:$lo:tt $field:ident as $type:tt
            $(, $comment:literal)?;
    ) => {
        ::kernel::bitfield!(@field_accessor $vis $name $storage, $hi:$lo $field as $type => $type $(, $comment)?;);
    };

    // Generates the accessor methods for a single field.
    (
        @leaf_accessor $vis:vis $name:ident $storage:ty, $hi:tt:$lo:tt $field:ident
            { $process:expr } $to_type:ty => $res_type:ty $(, $comment:literal)?;
    ) => {
        ::kernel::macros::paste!(
        const [<$field:upper _RANGE>]: ::core::ops::RangeInclusive<u8> = $lo..=$hi;
        const [<$field:upper _MASK>]: $storage = {
            // Generate mask for shifting
            match ::core::mem::size_of::<$storage>() {
                1 => ::kernel::bits::genmask_u8($lo..=$hi) as $storage,
                2 => ::kernel::bits::genmask_u16($lo..=$hi) as $storage,
                4 => ::kernel::bits::genmask_u32($lo..=$hi) as $storage,
                8 => ::kernel::bits::genmask_u64($lo..=$hi) as $storage,
                _ => ::kernel::build_error!("Unsupported storage type size")
            }
        };
        const [<$field:upper _SHIFT>]: u32 = Self::[<$field:upper _MASK>].trailing_zeros();
        );

        $(
        #[doc="Returns the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn $field(self) -> $res_type {
            ::kernel::macros::paste!(
            const MASK: $storage = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];
            );
            let field = ((self.0 & MASK) >> SHIFT);

            $process(field)
        }

        ::kernel::macros::paste!(
        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>](mut self, value: $to_type) -> Self {
            const MASK: $storage = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];
            let value = (<$storage>::from(value) << SHIFT) & MASK;
            self.0 = (self.0 & !MASK) | value;

            self
        }
        );
    };

    // Generates the `Debug` implementation for `$name`.
    (@debug $name:ident { $($field:ident;)* }) => {
        impl ::core::fmt::Debug for $name {
            fn fmt(&self, f: &mut ::core::fmt::Formatter<'_>) -> ::core::fmt::Result {
                f.debug_struct(stringify!($name))
                    .field("<raw>", &format_args!("{:#x}", &self.0))
                $(
                    .field(stringify!($field), &self.$field())
                )*
                    .finish()
            }
        }
    };

    // Generates the `Default` implementation for `$name`.
    (@default $name:ident { $($field:ident;)* }) => {
        /// Returns a value for the bitfield where all fields are set to their default value.
        impl ::core::default::Default for $name {
            fn default() -> Self {
                #[allow(unused_mut)]
                let mut value = Self(Default::default());

                ::kernel::macros::paste!(
                $(
                value.[<set_ $field>](Default::default());
                )*
                );

                value
            }
        }
    };
}

#[::kernel::macros::kunit_tests(kernel_bitfield)]
mod tests {
    use core::convert::TryFrom;

    // Enum types for testing => and ?=> conversions
    #[derive(Debug, Default, Clone, Copy, PartialEq)]
    enum MemoryType {
        #[default]
        Unmapped = 0,
        Normal = 1,
        Device = 2,
        Reserved = 3,
    }

    impl TryFrom<u8> for MemoryType {
        type Error = u8;
        fn try_from(value: u8) -> Result<Self, Self::Error> {
            match value {
                0 => Ok(MemoryType::Unmapped),
                1 => Ok(MemoryType::Normal),
                2 => Ok(MemoryType::Device),
                3 => Ok(MemoryType::Reserved),
                _ => Err(value),
            }
        }
    }

    impl From<MemoryType> for u64 {
        fn from(mt: MemoryType) -> u64 {
            mt as u64
        }
    }

    #[derive(Debug, Default, Clone, Copy, PartialEq)]
    enum Priority {
        #[default]
        Low = 0,
        Medium = 1,
        High = 2,
        Critical = 3,
    }

    impl From<u8> for Priority {
        fn from(value: u8) -> Self {
            match value & 0x3 {
                0 => Priority::Low,
                1 => Priority::Medium,
                2 => Priority::High,
                _ => Priority::Critical,
            }
        }
    }

    impl From<Priority> for u16 {
        fn from(p: Priority) -> u16 {
            p as u16
        }
    }

    bitfield! {
        struct TestPageTableEntry(u64) {
            0:0       present     as bool;
            1:1       writable    as bool;
            11:9      available   as u8;
            13:12     mem_type    as u8 ?=> MemoryType;
            17:14     extended_type as u8 ?=> MemoryType;  // For testing failures
            51:12     pfn         as u64;
            51:12     pfn_overlap as u64;
            61:52     available2  as u16;
        }
    }

    bitfield! {
        struct TestControlRegister(u16) {
            0:0       enable      as bool;
            3:1       mode        as u8;
            5:4       priority    as u8 => Priority;
            7:4       priority_nibble as u8;
            15:8      channel     as u8;
        }
    }

    bitfield! {
        struct TestStatusRegister(u8) {
            0:0       ready       as bool;
            1:1       error       as bool;
            3:2       state       as u8;
            7:4       reserved    as u8;
            7:0       full_byte   as u8;  // For entire register
        }
    }

    #[test]
    fn test_single_bits() {
        let mut pte = TestPageTableEntry::default();

        assert!(!pte.present());
        assert!(!pte.writable());
        assert_eq!(u64::from(pte), 0x0);

        pte = pte.set_present(true);
        assert!(pte.present());
        assert_eq!(u64::from(pte), 0x1);

        pte = pte.set_writable(true);
        assert!(pte.writable());
        assert_eq!(u64::from(pte), 0x3);

        pte = pte.set_writable(false);
        assert!(!pte.writable());
        assert_eq!(u64::from(pte), 0x1);

        assert_eq!(pte.available(), 0);
        pte = pte.set_available(0x5);
        assert_eq!(pte.available(), 0x5);
        assert_eq!(u64::from(pte), 0xA01);
    }

    #[test]
    fn test_range_fields() {
        let mut pte = TestPageTableEntry::default();
        assert_eq!(u64::from(pte), 0x0);

        pte = pte.set_pfn(0x123456);
        assert_eq!(pte.pfn(), 0x123456);
        // Test overlapping field reads same value
        assert_eq!(pte.pfn_overlap(), 0x123456);
        assert_eq!(u64::from(pte), 0x123456000);

        pte = pte.set_available(0x7);
        assert_eq!(pte.available(), 0x7);
        assert_eq!(u64::from(pte), 0x123456E00);

        pte = pte.set_available2(0x3FF);
        assert_eq!(pte.available2(), 0x3FF);
        assert_eq!(u64::from(pte), 0x3FF0000123456E00);

        // Test TryFrom with ?=> for MemoryType
        pte = pte.set_mem_type(MemoryType::Device);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Device));
        assert_eq!(u64::from(pte), 0x3FF0000123456E00);

        pte = pte.set_mem_type(MemoryType::Normal);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Normal));
        assert_eq!(u64::from(pte), 0x3FF0000123455E00);

        // Test all valid values for mem_type
        pte = pte.set_mem_type(MemoryType::Reserved);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Reserved));
        assert_eq!(u64::from(pte), 0x3FF0000123457E00);

        // Test failure case using extended_type field which has 4 bits (0-15)
        // MemoryType only handles 0-3, so values 4-15 should return Err
        let mut raw = pte.into();
        // Set bits 17:14 to 7 (invalid for MemoryType)
        raw = (raw & !::kernel::bits::genmask_u64(14..=17)) | (0x7 << 14);
        let invalid_pte = TestPageTableEntry(raw);
        // Should return Err with the invalid value
        assert_eq!(invalid_pte.extended_type(), Err(0x7));

        // Test a valid value after testing invalid to ensure both cases work
        // Set bits 17:14 to 2 (valid: Device)
        raw = (raw & !::kernel::bits::genmask_u64(14..=17)) | (0x2 << 14);
        let valid_pte = TestPageTableEntry(raw);
        assert_eq!(valid_pte.extended_type(), Ok(MemoryType::Device));

        let max_pfn = ::kernel::bits::genmask_u64(0..=39);
        pte = pte.set_pfn(max_pfn);
        assert_eq!(pte.pfn(), max_pfn);
        assert_eq!(pte.pfn_overlap(), max_pfn);
    }

    #[test]
    fn test_builder_pattern() {
        let pte = TestPageTableEntry::default()
            .set_present(true)
            .set_writable(true)
            .set_available(0x7)
            .set_pfn(0xABCDEF)
            .set_mem_type(MemoryType::Reserved)
            .set_available2(0x3FF);

        assert!(pte.present());
        assert!(pte.writable());
        assert_eq!(pte.available(), 0x7);
        assert_eq!(pte.pfn(), 0xABCDEF);
        assert_eq!(pte.pfn_overlap(), 0xABCDEF);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Reserved));
        assert_eq!(pte.available2(), 0x3FF);
    }

    #[test]
    fn test_raw_operations() {
        let raw_value = 0x3FF0000003123E03u64;

        let pte = TestPageTableEntry(raw_value);
        assert_eq!(u64::from(pte), raw_value);

        assert!(pte.present());
        assert!(pte.writable());
        assert_eq!(pte.available(), 0x7);
        assert_eq!(pte.pfn(), 0x3123);
        assert_eq!(pte.pfn_overlap(), 0x3123);
        assert_eq!(pte.mem_type(), Ok(MemoryType::Reserved));
        assert_eq!(pte.available2(), 0x3FF);

        // Test using direct constructor syntax TestStruct(value)
        let pte2 = TestPageTableEntry(raw_value);
        assert_eq!(u64::from(pte2), raw_value);
    }

    #[test]
    fn test_u16_bitfield() {
        let mut ctrl = TestControlRegister::default();

        assert!(!ctrl.enable());
        assert_eq!(ctrl.mode(), 0);
        assert_eq!(ctrl.priority(), Priority::Low);
        assert_eq!(ctrl.priority_nibble(), 0);
        assert_eq!(ctrl.channel(), 0);

        ctrl = ctrl.set_enable(true);
        assert!(ctrl.enable());

        ctrl = ctrl.set_mode(0x5);
        assert_eq!(ctrl.mode(), 0x5);

        // Test From conversion with =>
        ctrl = ctrl.set_priority(Priority::High);
        assert_eq!(ctrl.priority(), Priority::High);
        assert_eq!(ctrl.priority_nibble(), 0x2); // High = 2 in bits 5:4

        ctrl = ctrl.set_channel(0xAB);
        assert_eq!(ctrl.channel(), 0xAB);

        // Test overlapping fields
        ctrl = ctrl.set_priority_nibble(0xF);
        assert_eq!(ctrl.priority_nibble(), 0xF);
        assert_eq!(ctrl.priority(), Priority::Critical); // bits 5:4 = 0x3

        let ctrl2 = TestControlRegister::default()
            .set_enable(true)
            .set_mode(0x3)
            .set_priority(Priority::Medium)
            .set_channel(0x42);

        assert!(ctrl2.enable());
        assert_eq!(ctrl2.mode(), 0x3);
        assert_eq!(ctrl2.priority(), Priority::Medium);
        assert_eq!(ctrl2.channel(), 0x42);

        let raw_value: u16 = 0x4217;
        let ctrl3 = TestControlRegister(raw_value);
        assert_eq!(u16::from(ctrl3), raw_value);
        assert!(ctrl3.enable());
        assert_eq!(ctrl3.priority(), Priority::Medium);
        assert_eq!(ctrl3.priority_nibble(), 0x1);
        assert_eq!(ctrl3.channel(), 0x42);
    }

    #[test]
    fn test_u8_bitfield() {
        let mut status = TestStatusRegister::default();

        assert!(!status.ready());
        assert!(!status.error());
        assert_eq!(status.state(), 0);
        assert_eq!(status.reserved(), 0);
        assert_eq!(status.full_byte(), 0);

        status = status.set_ready(true);
        assert!(status.ready());
        assert_eq!(status.full_byte(), 0x01);

        status = status.set_error(true);
        assert!(status.error());
        assert_eq!(status.full_byte(), 0x03);

        status = status.set_state(0x3);
        assert_eq!(status.state(), 0x3);
        assert_eq!(status.full_byte(), 0x0F);

        status = status.set_reserved(0xA);
        assert_eq!(status.reserved(), 0xA);
        assert_eq!(status.full_byte(), 0xAF);

        // Test overlapping field
        status = status.set_full_byte(0x55);
        assert_eq!(status.full_byte(), 0x55);
        assert!(status.ready());
        assert!(!status.error());
        assert_eq!(status.state(), 0x1);
        assert_eq!(status.reserved(), 0x5);

        let status2 = TestStatusRegister::default()
            .set_ready(true)
            .set_state(0x2)
            .set_reserved(0x5);

        assert!(status2.ready());
        assert!(!status2.error());
        assert_eq!(status2.state(), 0x2);
        assert_eq!(status2.reserved(), 0x5);
        assert_eq!(status2.full_byte(), 0x59);

        let raw_value: u8 = 0x59;
        let status3 = TestStatusRegister(raw_value);
        assert_eq!(u8::from(status3), raw_value);
        assert!(status3.ready());
        assert!(!status3.error());
        assert_eq!(status3.state(), 0x2);
        assert_eq!(status3.reserved(), 0x5);
        assert_eq!(status3.full_byte(), 0x59);

        let status4 = TestStatusRegister(0xFF);
        assert!(status4.ready());
        assert!(status4.error());
        assert_eq!(status4.state(), 0x3);
        assert_eq!(status4.reserved(), 0xF);
        assert_eq!(status4.full_byte(), 0xFF);
    }
}
