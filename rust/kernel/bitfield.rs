// SPDX-License-Identifier: GPL-2.0

//! Support for defining bitfields as Rust structures.

/// Defines a bitfield struct with bounds-checked accessors for individual bit ranges.
///
/// # Example
///
/// ```rust
/// use kernel::bitfield;
/// use kernel::num::Bounded;
///
/// bitfield! {
///     pub struct Rgb(u16) {
///         15:11 blue;
///         10:5 green;
///         4:0 red;
///     }
/// }
///
/// // Setters can be chained. Bounded::new::<N>() does compile-time bounds checking.
/// let color = Rgb::default()
///     .set_red(Bounded::<u16, _>::new::<0x10>())
///     .set_green(Bounded::<u16, _>::new::<0x1f>())
///     .set_blue(Bounded::<u16, _>::new::<0x18>());
///
/// assert_eq!(color.red(), 0x10);
/// assert_eq!(color.green(), 0x1f);
/// assert_eq!(color.blue(), 0x18);
/// assert_eq!(
///     color.as_raw(),
///     (0x18 << Rgb::BLUE_SHIFT) + (0x1f << Rgb::GREEN_SHIFT) + 0x10,
/// );
///
/// // Convert to/from the backing storage type.
/// let raw: u16 = color.into();
/// assert_eq!(Rgb::from(raw), color);
/// ```
///
/// # Syntax
///
/// ```text
/// bitfield! {
///     #[attributes]
///     pub struct Name(storage_type), "Struct documentation." {
///         hi:lo field_1, "Field documentation.";
///         hi:lo field_2 => ConvertedType, "Field documentation.";
///         hi:lo field_3 ?=> ConvertedType, "Field documentation.";
///         ...
///     }
/// }
/// ```
///
/// - `storage_type`: The underlying integer type (`u8`, `u16`, `u32`, `u64`).
/// - `hi:lo`: Bit range (inclusive), where `hi >= lo`.
/// - `=> Type`: Optional infallible conversion (see [below](#infallible-conversion-)).
/// - `?=> Type`: Optional fallible conversion (see [below](#fallible-conversion-)).
/// - Documentation strings and attributes are optional.
///
/// # Generated code
///
/// Each field is internally represented as a [`Bounded`] parameterized by its bit width.
/// Field values can either be set/retrieved directly, or converted from/to another type.
///
/// The use of [`Bounded`] for each field enforces bounds-checking (at build time or runtime)
/// of every value assigned to a field. This ensures that data is never accidentally truncated.
///
/// The macro generates the bitfield type, [`From`] and [`Into`] implementations for its
/// storage type, and [`Default`] and [`Debug`] implementations.
///
/// For each field, it also generates:
/// - `field()` - getter returning a [`Bounded`] (or converted type) for the field,
/// - `set_field(value)` - setter with compile-time bounds checking,
/// - `try_set_field(value)` - setter with runtime bounds checking (for fields without type
///   conversion),
/// - `FIELD_MASK`, `FIELD_SHIFT`, `FIELD_RANGE` - constants for manual bit manipulation.
///
/// # Implicit conversions
///
/// Types that fit entirely within a field's bit width can be used directly with setters.
/// For example, `bool` works with single-bit fields, and `u8` works with 8-bit fields:
///
/// ```rust
/// use kernel::bitfield;
///
/// bitfield! {
///     pub struct Flags(u32) {
///         15:8 byte_field;
///         0:0 flag;
///     }
/// }
///
/// let flags = Flags::default()
///     .set_byte_field(0x42_u8)
///     .set_flag(true);
///
/// assert_eq!(flags.as_raw(), (0x42 << Flags::BYTE_FIELD_SHIFT) | 1);
/// ```
///
/// # Runtime bounds checking
///
/// When a value is not known at compile time, use `try_set_field()` to check bounds at runtime:
///
/// ```rust
/// use kernel::bitfield;
///
/// bitfield! {
///     pub struct Config(u8) {
///         3:0 nibble;
///     }
/// }
///
/// fn set_nibble(config: Config, value: u8) -> Result<Config, Error> {
///     // Returns `EOVERFLOW` if `value > 0xf`.
///     config.try_set_nibble(value)
/// }
/// # Ok::<(), Error>(())
/// ```
///
/// # Type conversion
///
/// Fields can be automatically converted to/from a custom type using `=>` (infallible) or
/// `?=>` (fallible). The custom type must implement the appropriate `From` or `TryFrom` traits
/// with [`Bounded`].
///
/// ## Infallible conversion (`=>`)
///
/// Use when all bit patterns map to valid values:
///
/// ```rust
/// use kernel::bitfield;
/// use kernel::num::Bounded;
///
/// #[derive(Debug, Clone, Copy, Default, PartialEq)]
/// enum Power {
///     #[default]
///     Off,
///     On,
/// }
///
/// impl From<Bounded<u32, 1>> for Power {
///     fn from(v: Bounded<u32, 1>) -> Self {
///         match *v {
///             0 => Power::Off,
///             _ => Power::On,
///         }
///     }
/// }
///
/// impl From<Power> for Bounded<u32, 1> {
///     fn from(p: Power) -> Self {
///         (p as u32 != 0).into()
///     }
/// }
///
/// bitfield! {
///     pub struct Control(u32) {
///         0:0 power => Power;
///     }
/// }
///
/// let ctrl = Control::default().set_power(Power::On);
/// assert_eq!(ctrl.power(), Power::On);
/// ```
///
/// ## Fallible conversion (`?=>`)
///
/// Use when some bit patterns are invalid. The getter returns a [`Result`]:
///
/// ```rust
/// use kernel::bitfield;
/// use kernel::num::Bounded;
///
/// #[derive(Debug, Clone, Copy, Default, PartialEq)]
/// enum Mode {
///     #[default]
///     Low = 0,
///     High = 1,
///     Auto = 2,
///     // 3 is invalid
/// }
///
/// impl TryFrom<Bounded<u32, 2>> for Mode {
///     type Error = u32;
///
///     fn try_from(v: Bounded<u32, 2>) -> Result<Self, u32> {
///         match *v {
///             0 => Ok(Mode::Low),
///             1 => Ok(Mode::High),
///             2 => Ok(Mode::Auto),
///             n => Err(n),
///         }
///     }
/// }
///
/// impl From<Mode> for Bounded<u32, 2> {
///     fn from(m: Mode) -> Self {
///         match m {
///             Mode::Low => Bounded::<u32, _>::new::<0>(),
///             Mode::High => Bounded::<u32, _>::new::<1>(),
///             Mode::Auto => Bounded::<u32, _>::new::<2>(),
///         }
///     }
/// }
///
/// bitfield! {
///     pub struct Config(u32) {
///         1:0 mode ?=> Mode;
///     }
/// }
///
/// let cfg = Config::default().set_mode(Mode::Auto);
/// assert_eq!(cfg.mode(), Ok(Mode::Auto));
///
/// // Invalid bit pattern returns an error.
/// assert_eq!(Config::from(0b11).mode(), Err(3));
/// ```
///
/// [`Bounded`]: kernel::num::Bounded
#[macro_export]
macro_rules! bitfield {
    // Entry point defining the bitfield struct, its implementations and its field accessors.
    (
        $(#[$attr:meta])* $vis:vis struct $name:ident($storage:ty)
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::bitfield!(@core $(#[$attr])* $vis $name $storage $(, $comment)?);
        ::kernel::bitfield!(@fields $vis $name $storage { $($fields)* });
    };

    // All rules below are helpers.

    // Defines the wrapper `$name` type and its conversions from/to the storage type.
    (@core $(#[$attr:meta])* $vis:vis $name:ident $storage:ty $(, $comment:literal)?) => {
        $(
        #[doc=$comment]
        )?
        $(#[$attr])*
        #[repr(transparent)]
        #[derive(Clone, Copy, PartialEq, Eq)]
        $vis struct $name($storage);

        #[allow(dead_code)]
        impl $name {
            /// Returns the raw value of this bitfield.
            ///
            /// This is similar to the [`From`] implementation, but is shorter to invoke in
            /// most cases.
            $vis fn as_raw(self) -> $storage {
                self.0
            }
        }

        impl ::core::convert::From<$name> for $storage {
            fn from(val: $name) -> $storage {
                val.0
            }
        }

        impl ::core::convert::From<$storage> for $name {
            fn from(val: $storage) -> $name {
                Self(val)
            }
        }
    };

    // Definitions requiring knowledge of individual fields: private and public field accessors,
    // and `Debug` and `Default` implementations.
    (@fields $vis:vis $name:ident $storage:ty {
        $($hi:tt:$lo:tt $field:ident
            $(?=> $try_into_type:ty)?
            $(=> $into_type:ty)?
            $(, $comment:literal)?
        ;
        )*
    }
    ) => {
        #[allow(dead_code)]
        impl $name {
        $(
        ::kernel::bitfield!(@private_field_accessors $vis $name $storage : $hi:$lo $field);
        ::kernel::bitfield!(@public_field_accessors $vis $name $storage : $hi:$lo $field
            $(?=> $try_into_type)?
            $(=> $into_type)?
            $(, $comment)?
        );
        )*
        }

        ::kernel::bitfield!(@debug $name { $($field;)* });
        ::kernel::bitfield!(@default $name { $($field;)* });
    };

    // Private field accessors working with the correct `Bounded` type for the field.
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
                self.0 << ALIGN_TOP
            );
            val.shr::<ALIGN_BOTTOM, _>()
        }

        fn [<__set_ $field>](
            mut self,
            value: ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>,
        ) -> Self
        {
            const MASK: $storage = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];

            let value = value.get() << SHIFT;
            self.0 = (self.0 & !MASK) | value;

            self
        }
        );
    };

    // Public accessors for fields infallibly (`=>`) converted to a type.
    (
        @public_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
            => $into_type:ty $(, $comment:literal)?
    ) => {
        ::kernel::macros::paste!(

        $(
        #[doc="Returns the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn $field(self) -> $into_type
        {
            self.[<__ $field>]().into()
        }

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>](self, value: $into_type) -> Self
        {
            self.[<__set_ $field>](value.into())
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> $into_type {
            Default::default()
        }

        );
    };

    // Public accessors for fields fallibly (`?=>`) converted to a type.
    (
        @public_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
            ?=> $try_into_type:ty $(, $comment:literal)?
    ) => {
        ::kernel::macros::paste!(

        $(
        #[doc="Returns the value of this field:"]
        #[doc=$comment]
        )?
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

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>](self, value: $try_into_type) -> Self
        {
            self.[<__set_ $field>](value.into())
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> $try_into_type {
            Default::default()
        }

        );
    };

    // Public accessors for fields not converted to a type.
    (
        @public_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
            $(, $comment:literal)?
    ) => {
        ::kernel::macros::paste!(

        $(
        #[doc="Returns the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn $field(self) ->
            ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>
        {
            self.[<__ $field>]()
        }

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>]<T>(
            self,
            value: T,
        ) -> Self
            where T: Into<::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>>,
        {
            self.[<__set_ $field>](value.into())
        }

        $(
        #[doc="Attempts to set the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<try_set_ $field>]<T>(
            self,
            value: T,
        ) -> ::kernel::error::Result<Self>
            where T: ::kernel::num::TryIntoBounded<$storage, { $hi + 1 - $lo }>,
        {
            Ok(
                self.[<__set_ $field>](
                    value.try_into_bounded().ok_or(::kernel::error::code::EOVERFLOW)?
                )
            )
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }> {
            Default::default()
        }

        );
    };

    // `Debug` implementation.
    (@debug $name:ident { $($field:ident;)* }) => {
        impl ::kernel::fmt::Debug for $name {
            fn fmt(&self, f: &mut ::kernel::fmt::Formatter<'_>) -> ::kernel::fmt::Result {
                f.debug_struct(stringify!($name))
                    .field("<raw>", &::kernel::prelude::fmt!("{:#x}", self.0))
                $(
                    .field(stringify!($field), &self.$field())
                )*
                    .finish()
            }
        }
    };

    // `Default` implementation.
    (@default $name:ident { $($field:ident;)* }) => {
        /// Returns a value for the bitfield where all fields are set to their default value.
        impl ::core::default::Default for $name {
            fn default() -> Self {
                #[allow(unused_mut)]
                let mut value = Self(Default::default());

                ::kernel::macros::paste!(
                $(
                value = value.[<set_ $field>](Self::[<$field _default>]());
                )*
                );

                value
            }
        }
    };
}
