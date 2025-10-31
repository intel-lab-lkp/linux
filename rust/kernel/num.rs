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

impl Integer for u8 {
    type Signedness = Unsigned;

    const BITS: u32 = u8::BITS;
}

impl Integer for u16 {
    type Signedness = Unsigned;

    const BITS: u32 = u16::BITS;
}

impl Integer for u32 {
    type Signedness = Unsigned;

    const BITS: u32 = u32::BITS;
}

impl Integer for u64 {
    type Signedness = Unsigned;

    const BITS: u32 = u64::BITS;
}

impl Integer for i8 {
    type Signedness = Signed;

    const BITS: u32 = i8::BITS;
}

impl Integer for i16 {
    type Signedness = Signed;

    const BITS: u32 = i16::BITS;
}

impl Integer for i32 {
    type Signedness = Signed;

    const BITS: u32 = i32::BITS;
}

impl Integer for i64 {
    type Signedness = Signed;

    const BITS: u32 = i64::BITS;
}
