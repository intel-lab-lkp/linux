// SPDX-License-Identifier: GPL-2.0

//! Types and macros to define register layout and accessors.
//!
//! A single register typically includes several fields, which are accessed through a combination
//! of bit-shift and mask operations that introduce a class of potential mistakes, notably because
//! not all possible field values are necessarily valid.
//!
//! The macros in this module allow to define, using an intruitive and readable syntax, a dedicated
//! type for each register with its own field accessors that can return an error is a field's value
//! is invalid. They also provide a builder type allowing to construct a register value to be
//! written by combining valid values for its fields.

/// Helper macro for the `reg_def` family of macros.
///
/// Defines the wrapper `$name` type, as well as its relevant implementations (`Debug`, `BitOr`,
/// and conversion to regular `u32`).
#[macro_export]
macro_rules! __reg_def_common {
    ($name:ident $(, $type_comment:expr)?) => {
        $(
        #[doc=$type_comment]
        )?
        #[repr(transparent)]
        #[derive(Clone, Copy, Default)]
        pub(crate) struct $name(u32);

        // TODO: should we display the raw hex value, then the value of all its fields?
        impl ::core::fmt::Debug for $name {
            fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                f.debug_tuple(stringify!($name))
                    .field(&format_args!("0x{0:x}", &self.0))
                    .finish()
            }
        }

        impl core::ops::BitOr for $name {
            type Output = Self;

            fn bitor(self, rhs: Self) -> Self::Output {
                Self(self.0 | rhs.0)
            }
        }

        impl From<$name> for u32 {
            fn from(reg: $name) -> u32 {
                reg.0
            }
        }
    };
}

/// Helper macro for the `reg_def` family of macros.
///
/// Defines the getter method for $field.
#[macro_export]
macro_rules! __reg_def_field_getter {
    (
        $hi:tt:$lo:tt $field:ident
            $(=> as $as_type:ty)?
            $(=> as_bit $bit_type:ty)?
            $(=> into $type:ty)?
            $(=> try_into $try_type:ty)?
        $(, $comment:expr)?
    ) => {
        $(
        #[doc=concat!("Returns the ", $comment)]
        )?
        #[inline]
        pub(crate) fn $field(self) -> $( $as_type )? $( $bit_type )? $( $type )? $( core::result::Result<$try_type, <$try_type as TryFrom<u32>>::Error> )? {
            const MASK: u32 = ((((1 << $hi) - 1) << 1) + 1) - ((1 << $lo) - 1);
            const SHIFT: u32 = MASK.trailing_zeros();
            let field = (self.0 & MASK) >> SHIFT;

            $( field as $as_type )?
            $(
            // TODO: it would be nice to throw a compile-time error if $hi != $lo as this means we
            // are considering more than one bit but returning a bool...
            <$bit_type>::from(if field != 0 { true } else { false }) as $bit_type
            )?
            $( <$type>::from(field) )?
            $( <$try_type>::try_from(field) )?
        }
    }
}

/// Helper macro for the `reg_def` family of macros.
///
/// Defines all the field getter methods for `$name`.
#[macro_export]
macro_rules! __reg_def_getters {
    (
        $name:ident
        $(; $hi:tt:$lo:tt $field:ident
            $(=> as $as_type:ty)?
            $(=> as_bit $bit_type:ty)?
            $(=> into $type:ty)?
            $(=> try_into $try_type:ty)?
        $(, $field_comment:expr)?)* $(;)?
    ) => {
        #[allow(dead_code)]
        impl $name {
            $(
            ::kernel::__reg_def_field_getter!($hi:$lo $field $(=> as $as_type)? $(=> as_bit $bit_type)? $(=> into $type)? $(=> try_into $try_type)? $(, $field_comment)?);
            )*
        }
    };
}

/// Helper macro for the `reg_def` family of macros.
///
/// Defines the setter method for $field.
#[macro_export]
macro_rules! __reg_def_field_setter {
    (
        $hi:tt:$lo:tt $field:ident
            $(=> as $as_type:ty)?
            $(=> as_bit $bit_type:ty)?
            $(=> into $type:ty)?
            $(=> try_into $try_type:ty)?
        $(, $comment:expr)?
    ) => {
        kernel::macros::paste! {
        $(
        #[doc=concat!("Sets the ", $comment)]
        )?
        #[inline]
        pub(crate) fn [<set_ $field>](mut self, value: $( $as_type)? $( $bit_type )? $( $type )? $( $try_type)? ) -> Self {
            const MASK: u32 = ((((1 << $hi) - 1) << 1) + 1) - ((1 << $lo) - 1);
            const SHIFT: u32 = MASK.trailing_zeros();

            let value = ((value as u32) << SHIFT) & MASK;
            self.0 = self.0 | value;
            self
        }
        }
    };
}

/// Helper macro for the `reg_def` family of macros.
///
/// Defines all the field setter methods for `$name`.
#[macro_export]
macro_rules! __reg_def_setters {
    (
        $name:ident
        $(; $hi:tt:$lo:tt $field:ident
            $(=> as $as_type:ty)?
            $(=> as_bit $bit_type:ty)?
            $(=> into $type:ty)?
            $(=> try_into $try_type:ty)?
        $(, $field_comment:expr)?)* $(;)?
    ) => {
        #[allow(dead_code)]
        impl $name {
            $(
            ::kernel::__reg_def_field_setter!($hi:$lo $field $(=> as $as_type)? $(=> as_bit $bit_type)? $(=> into $type)? $(=> try_into $try_type)? $(, $field_comment)?);
            )*
        }
    };
}

/// Defines a dedicated type for a register with an absolute offset, alongside with getter and
/// setter methods for its fields and methods to read and write it from an `Io` region.
///
/// Example:
///
/// ```no_run
/// reg_def!(Boot0@0x00000100, "Basic revision information about the chip";
///     3:0     minor_rev => as u8, "minor revision of the chip";
///     7:4     major_rev => as u8, "major revision of the chip";
///     28:20   chipset => try_into Chipset, "chipset model"
/// );
/// ```
///
/// This defines a `Boot0` type which can be read or written from offset `0x100` of an `Io` region.
/// It is composed of 3 fields, for instance `minor_rev` is made of the 4 less significant bits of
/// the register. Each field can be accessed and modified using helper methods:
///
/// ```no_run
/// // Read from offset 0x100.
/// let boot0 = Boot0.read(&bar);
/// pr_info!("chip revision: {}.{}", boot0.major_rev(), boot0.minor_rev());
///
/// // `Chipset::try_from` will be called with the value of the field and returns an error if the
/// // value is invalid.
/// let chipset = boot0.chipset()?;
///
/// // Update some fields and write the value back.
/// boot0.set_major_rev(3).set_minor_rev(10).write(&bar);
/// ```
///
/// Fields are made accessible using one of the following strategies:
///
/// - `as <type>` simply casts the field value to the requested type.
/// - `as_bit <type>` turns the field into a boolean and calls `<type>::from()` with the obtained
///   value. To be used with single-bit fields.
/// - `into <type>` calls `<type>::from()` on the value of the field. It is expected to handle all
///   the possible values for the bit range selected.
/// - `try_into <type>` calls `<type>::try_from()` on the value of the field and returns its
///   result.
///
/// The documentation strings are optional. If present, they will be added to the type or the field
/// getter and setter methods they are attached to.
#[macro_export]
macro_rules! reg_def {
    (
        $name:ident@$offset:expr $(, $type_comment:expr)?
        $(; $hi:tt:$lo:tt $field:ident
            $(=> as $as_type:ty)?
            $(=> as_bit $bit_type:ty)?
            $(=> into $type:ty)?
            $(=> try_into $try_type:ty)?
        $(, $field_comment:expr)?)* $(;)?
    ) => {
        ::kernel::__reg_def_common!($name);

        #[allow(dead_code)]
        impl $name {
            #[inline]
            pub(crate) fn read<const SIZE: usize, T: Deref<Target=Io<SIZE>>>(bar: &T) -> Self {
                Self(bar.readl($offset))
            }

            #[inline]
            pub(crate) fn write<const SIZE: usize, T: Deref<Target=Io<SIZE>>>(self, bar: &T) {
                bar.writel(self.0, $offset)
            }
        }

        ::kernel::__reg_def_getters!($name; $( $hi:$lo $field $(=> as $as_type)? $(=> as_bit $bit_type)? $(=> into $type)? $(=> try_into $try_type)? $(, $field_comment)? );*);

        ::kernel::__reg_def_setters!($name; $( $hi:$lo $field $(=> as $as_type)? $(=> as_bit $bit_type)? $(=> into $type)? $(=> try_into $try_type)? $(, $field_comment)? );*);
    };
}

/// Defines a dedicated type for a register with a relative offset, alongside with getter and
/// setter methods for its fields and methods to read and write it from an `Io` region.
///
/// See the documentation for [`reg_def`] for more details. This macro works similarly to
/// `reg_def`, with the exception that the `read` and `write` methods take a `base` argument that
/// is added to the offset of the register before access, and the `try_read` and `try_write`
/// methods are added to allow access with offsets unknown at compile-time.
#[macro_export]
macro_rules! reg_def_rel {
    (
        $name:ident@$offset:expr $(, $type_comment:expr)?
        $(; $hi:tt:$lo:tt $field:ident
            $(=> as $as_type:ty)?
            $(=> as_bit $bit_type:ty)?
            $(=> into $type:ty)?
            $(=> try_into $try_type:ty)?
        $(, $field_comment:expr)?)* $(;)?
    ) => {
        ::kernel::__reg_def_common!($name);

        #[allow(dead_code)]
        impl $name {
            #[inline]
            pub(crate) fn read<const SIZE: usize, T: Deref<Target=Io<SIZE>>>(bar: &T, base: usize) -> Self {
                Self(bar.readl(base + $offset))
            }

            #[inline]
            pub(crate) fn write<const SIZE: usize, T: Deref<Target=Io<SIZE>>>(self, bar: &T, base: usize) {
                bar.writel(self.0, base + $offset)
            }

            #[inline]
            pub(crate) fn try_read<const SIZE: usize, T: Deref<Target=Io<SIZE>>>(bar: &T, base: usize) -> ::kernel::error::Result<Self> {
                bar.try_readl(base + $offset).map(Self)
            }

            #[inline]
            pub(crate) fn try_write<const SIZE: usize, T: Deref<Target=Io<SIZE>>>(self, bar: &T, base: usize) -> ::kernel::error::Result<()> {
                bar.try_writel(self.0, base + $offset)
            }
        }

        ::kernel::__reg_def_getters!($name; $( $hi:$lo $field $(=> as $as_type)? $(=> as_bit $bit_type)? $(=> into $type)? $(=> try_into $try_type)? $(, $field_comment)? );*);

        ::kernel::__reg_def_setters!($name; $( $hi:$lo $field $(=> as $as_type)? $(=> as_bit $bit_type)? $(=> into $type)? $(=> try_into $try_type)? $(, $field_comment)? );*);
    };
}
