// SPDX-License-Identifier: GPL-2.0

//! Numerical features for the kernel.

pub mod bitint;
pub use bitint::*;

/// Type used to designate unsigned primitive types.
pub struct Unsigned;

/// Type used to designate signed primitive types.
pub struct Signed;

/// Trait describing properties of integer types.
pub trait Integer {
    /// Whether this type is [`Signed`] or [`Unsigned`].
    type Signedness;

    /// Number of bits used for value representation.
    const BITS: u32;
}

impl Integer for bool {
    type Signedness = Unsigned;

    const BITS: u32 = 1;
}

macro_rules! impl_integer {
    ($($type:ty: $signedness:ty), *) => {
        $(
        impl Integer for $type {
            type Signedness = $signedness;

            const BITS: u32 = <$type>::BITS;
        }
        )*
    };
}

impl_integer!(
    u8: Unsigned,
    u16: Unsigned,
    u32: Unsigned,
    u64: Unsigned,
    i8: Signed,
    i16: Signed,
    i32: Signed,
    i64: Signed
);
