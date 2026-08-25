// SPDX-License-Identifier: GPL-2.0

//! Numerical helpers functions and traits.
//!
//! This is essentially a staging module for code to mature until it can be moved to the `kernel`
//! crate.

/// Creates an enum type associated to a [`Bounded`](kernel::num::Bounded), with a [`From`]
/// conversion to the associated `Bounded` and either a [`TryFrom`] or `From` conversion from the
/// associated `Bounded`.
// TODO[FPRI]: This is a temporary solution to be replaced with the corresponding derive macros
// once they land.
#[macro_export]
macro_rules! bounded_enum {
    (
        $(#[$enum_meta:meta])*
        $vis:vis enum $enum_type:ident with $from_impl:ident<Bounded<$width:ty, $length:literal>> {
            $( $(#[doc = $variant_doc:expr])* $variant:ident = $value:expr),* $(,)*
        }
    ) => {
        $(#[$enum_meta])*
        $vis enum $enum_type {
            $(
                $(#[doc = $variant_doc])*
                $variant = $value
            ),*
        }

        impl core::convert::From<$enum_type> for kernel::num::Bounded<$width, $length> {
            fn from(value: $enum_type) -> Self {
                match value {
                    $($enum_type::$variant =>
                        kernel::num::Bounded::<$width, _>::new::<{ $value }>()),*
                }
            }
        }

        bounded_enum!(@impl_from $enum_type with $from_impl<Bounded<$width, $length>> {
            $($variant = $value),*
        });
    };

    // `TryFrom` implementation from associated `Bounded` to enum type.
    (@impl_from $enum_type:ident with TryFrom<Bounded<$width:ty, $length:literal>> {
        $($variant:ident = $value:expr),* $(,)*
    }) => {
        impl core::convert::TryFrom<kernel::num::Bounded<$width, $length>> for $enum_type {
            type Error = kernel::error::Error;

            fn try_from(
                value: kernel::num::Bounded<$width, $length>
            ) -> kernel::error::Result<Self> {
                match value.get() {
                    $(
                        $value => Ok($enum_type::$variant),
                    )*
                    _ => Err(kernel::error::code::EINVAL),
                }
            }
        }
    };

    // `From` implementation from associated `Bounded` to enum type. Triggers a build-time error if
    // all possible values of the `Bounded` are not covered by the enum type.
    (@impl_from $enum_type:ident with From<Bounded<$width:ty, $length:literal>> {
        $($variant:ident = $value:expr),* $(,)*
    }) => {
        impl core::convert::From<kernel::num::Bounded<$width, $length>> for $enum_type {
            fn from(value: kernel::num::Bounded<$width, $length>) -> Self {
                const MAX: $width = 1 << $length;

                // Makes the compiler optimizer aware of the possible range of values.
                let value = value.get() & ((1 << $length) - 1);
                match value {
                    $(
                        $value => $enum_type::$variant,
                    )*
                    // PANIC: we cannot reach this arm as all possible variants are handled by the
                    // match arms above. It is here to make the compiler complain if `$enum_type`
                    // does not cover all values of the `0..MAX` range.
                    MAX.. => unreachable!(),
                }
            }
        }
    }
}
