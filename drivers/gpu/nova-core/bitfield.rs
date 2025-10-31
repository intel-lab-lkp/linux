// SPDX-License-Identifier: GPL-2.0

//! Bitfield library for Rust structures
//!
//! Support for defining bitfields in Rust structures. Also used by the [`register!`] macro.

/// Defines a struct with accessors to access bits within an inner unsigned integer.
///
/// # Syntax
///
/// ```rust
/// use nova_core::bitfield;
///
/// #[derive(Debug, Clone, Copy, Default)]
/// enum Mode {
///     #[default]
///     Low = 0,
///     High = 1,
///     Auto = 2,
/// }
///
/// impl TryFrom<BitInt<u32, 4>> for Mode {
///     type Error = u32;
///     fn try_from(value: BitInt<u32, 4>) -> Result<Self, Self::Error> {
///         match *value {
///             0 => Ok(Mode::Low),
///             1 => Ok(Mode::High),
///             2 => Ok(Mode::Auto),
///             value => Err(value),
///         }
///     }
/// }
///
/// impl From<Mode> for BitInt<u32, 4> {
///     fn from(mode: Mode) -> BitInt<u32, 4> {
///         BitInt::from_expr(mode as u32)
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
/// impl From<BitInt<u32, 1>> for State {
///     fn from(value: BitInt<u32, 1>) -> Self {
///         if bool::from(value) {
///             State::Active
///         } else {
///             State::Inactive
///         }
///     }
/// }
///
/// impl From<State> for BitInt<u32, 1> {
///     fn from(state: State) -> BitInt<u32, 1> {
///         match state {
///             State::Inactive => false.into(),
///             State::Active => true.into(),
///         }
///     }
/// }
///
/// bitfield! {
///     pub struct ControlReg(u32) {
///         7:7 state => State;
///         3:0 mode ?=> Mode;
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
macro_rules! bitfield {
    // Main entry point - defines the bitfield struct with fields
    ($vis:vis struct $name:ident($storage:ty) $(, $comment:literal)? { $($fields:tt)* }) => {
        bitfield!(@core $vis $name $storage $(, $comment)? { $($fields)* });
    };

    // All rules below are helpers.

    // Defines the wrapper `$name` type, as well as its relevant implementations (`Debug`,
    // `Default`, and conversion to the value type) and field accessor methods.
    (@core $vis:vis $name:ident $storage:ty $(, $comment:literal)? { $($fields:tt)* }) => {
        $(
        #[doc=$comment]
        )?
        #[repr(transparent)]
        #[derive(Clone, Copy)]
        $vis struct $name($storage);

        impl ::core::convert::From<$name> for $storage {
            fn from(val: $name) -> $storage {
                val.0
            }
        }

        bitfield!(@fields_dispatcher $vis $name $storage { $($fields)* });
    };

    // Dispatch fields depending on the syntax used.
    (@fields_dispatcher $vis:vis $name:ident $storage:ty {
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
        bitfield!(@private_field_accessors $name $storage : $hi:$lo $field);
        bitfield!(@public_field_accessors $vis $name $storage : $hi:$lo $field
            $(?=> $try_into_type)?
            $(=> $into_type)?
            $(, $comment)?
        );
        )*
        }

        bitfield!(@debug $name { $($field;)* });
        bitfield!(@default $name { $($field;)* });

    };

    (
        @private_field_accessors $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
    ) => {
        ::kernel::macros::paste!(
        const [<$field:upper _RANGE>]: ::core::ops::RangeInclusive<u8> = $lo..=$hi;
        const [<$field:upper _MASK>]: u32 = ((((1 << $hi) - 1) << 1) + 1) - ((1 << $lo) - 1);
        const [<$field:upper _SHIFT>]: u32 = $lo;
        );

        ::kernel::macros::paste!(
        fn [<$field _internal>](self) ->
            ::kernel::num::BitInt<$storage, { $hi + 1 - $lo }> {
            const MASK: u32 = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];

            let field = ((self.0 & MASK) >> SHIFT);

            ::kernel::num::BitInt::<$storage, { $hi + 1 - $lo }>::from_expr(field)
        }

        fn [<set_ $field _internal>](
            mut self,
            value: ::kernel::num::BitInt<$storage, { $hi + 1 - $lo }>,
        ) -> Self
        {
            const MASK: u32 = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];

            let value = (value.get() << SHIFT) & MASK;
            self.0 = (self.0 & !MASK) | value;

            self
        }

        fn [<try_set_ $field _internal>]<T>(
            mut self,
            value: T,
        ) -> ::kernel::error::Result<Self>
            where T: ::kernel::num::TryIntoBitInt<$storage, { $hi + 1 - $lo }>,
        {
            const MASK: u32 = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];

            let value = (
                value.try_into_bitint().ok_or(::kernel::error::code::EOVERFLOW)?.get() << SHIFT
            ) & MASK;

            self.0 = (self.0 & !MASK) | value;

            Ok(self)
        }
        );
    };

    // Generates the public accessors for fields infallibly (`=>`) converted to a type.
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
            self.[<$field _internal>]().into()
        }

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>](self, value: $into_type) -> Self
        {
            self.[<set_ $field _internal>](value.into())
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> $into_type {
            Default::default()
        }

        );
    };

    // Generates the public accessors for fields fallibly (`?=>`) converted to a type.
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
                    ::kernel::num::BitInt<$storage, { $hi + 1 - $lo }>
                >>::Error
            >
        {
            self.[<$field _internal>]().try_into()
        }

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>](self, value: $try_into_type) -> Self
        {
            self.[<set_ $field _internal>](value.into())
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> $try_into_type {
            Default::default()
        }

        );
    };

    // Generates the public accessors for fields not converted to a type.
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
            ::kernel::num::BitInt<$storage, { $hi + 1 - $lo }>
        {
            self.[<$field _internal>]()
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
            where T: Into<::kernel::num::BitInt<$storage, { $hi + 1 - $lo }>>,
        {
            self.[<set_ $field _internal>](value.into())
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
            where T: ::kernel::num::TryIntoBitInt<$storage, { $hi + 1 - $lo }>,
        {
            Ok(
                self.[<set_ $field _internal>](
                    value.try_into_bitint().ok_or(::kernel::error::code::EOVERFLOW)?
                )
            )
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> ::kernel::num::BitInt<$storage, { $hi + 1 - $lo }> {
            Default::default()
        }

        );
    };

    // Generates the `Debug` implementation for `$name`.
    (@debug $name:ident { $($field:ident;)* }) => {
        impl ::kernel::fmt::Debug for $name {
            fn fmt(&self, f: &mut ::kernel::fmt::Formatter<'_>) -> ::kernel::fmt::Result {
                f.debug_struct(stringify!($name))
                    .field("<raw>", &::kernel::prelude::fmt!("{:#x}", &self.0))
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
                value.[<set_ $field>](Self::[<$field _default>]());
                )*
                );

                value
            }
        }
    };
}
