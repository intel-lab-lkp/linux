// SPDX-License-Identifier: GPL-2.0

//! Additional numerical features for the kernel.

use crate::const_assert;
use core::ops;

pub mod bounded;
pub use bounded::*;

/// Creates a value from an integer constant expression, with validity checked at build time.
///
/// This works for any type that implements [`FromConst`], with the target type inferred from
/// the context.
///
/// # Examples
///
/// ```
/// use core::num::NonZero;
/// use kernel::num::Bounded;
/// use kernel::num::cv;
/// use kernel::ptr::Alignment;
///
/// let v: NonZero<usize> = cv!(8);
/// assert_eq!(v.get(), 8);
///
/// // Any integer constant expression works, not only literals.
/// let m: NonZero<usize> = cv!(usize::MAX);
/// assert_eq!(m.get(), usize::MAX);
///
/// let b: Bounded<u32, 4> = cv!(15);
/// assert_eq!(b.get(), 15);
///
/// let a: Alignment = cv!(4096);
/// assert_eq!(a.as_usize(), 4096);
/// ```
#[macro_export]
#[doc(hidden)]
macro_rules! cv {
    ($v:expr) => {
        $crate::num::FromConst::from_const::<
            {
                #[allow(unused_comparisons, unused_assignments, clippy::as_underscore)]
                {
                    let v = $v;
                    let r = v as i128;
                    // Pin `back` to `v`'s type so `as _` casts back to the source type.
                    let mut back = v;
                    back = r as _;

                    ::core::assert!(
                        back == v && (v < 0) == (r < 0),
                        "value cannot be losslessly widened to `i128`"
                    );

                    r
                }
            },
        >()
    };
}
#[doc(inline)]
pub use cv;

/// Types that can be created from an integer constant expression validated at build time.
// TODO: make this a `const` trait once they are stable. This will let cv! be used in const
// contexts.
pub trait FromConst: Sized {
    /// Creates the value that corresponds to the constant `V`.
    ///
    /// Fails the build if `V` is not a valid value for `Self`.
    fn from_const<const V: i128>() -> Self;
}

/// Implements [`FromConst`] for [`NonZero`](core::num::NonZero).
macro_rules! impl_from_const_nonzero {
    ($($type:ty)*) => {
        $(
        impl FromConst for core::num::NonZero<$type> {
            #[inline]
            fn from_const<const V: i128>() -> Self {
                const_assert!(
                    V >= <$type>::MIN as i128 && V <= <$type>::MAX as i128,
                    "Constant cannot be represented by the underlying type."
                );

                const { core::num::NonZero::new(V as $type).unwrap() }
            }
        }
        )*
    };
}

impl_from_const_nonzero!(
    u8 u16 u32 u64 usize
    i8 i16 i32 i64 isize
);

/// Designates unsigned primitive types.
pub enum Unsigned {}

/// Designates signed primitive types.
pub enum Signed {}

/// Describes core properties of integer types.
pub trait Integer:
    Sized
    + Copy
    + Clone
    + PartialEq
    + Eq
    + PartialOrd
    + Ord
    + ops::Add<Output = Self>
    + ops::AddAssign
    + ops::Sub<Output = Self>
    + ops::SubAssign
    + ops::Mul<Output = Self>
    + ops::MulAssign
    + ops::Div<Output = Self>
    + ops::DivAssign
    + ops::Rem<Output = Self>
    + ops::RemAssign
    + ops::BitAnd<Output = Self>
    + ops::BitAndAssign
    + ops::BitOr<Output = Self>
    + ops::BitOrAssign
    + ops::BitXor<Output = Self>
    + ops::BitXorAssign
    + ops::Shl<u32, Output = Self>
    + ops::ShlAssign<u32>
    + ops::Shr<u32, Output = Self>
    + ops::ShrAssign<u32>
    + ops::Not
{
    /// Whether this type is [`Signed`] or [`Unsigned`].
    type Signedness;

    /// Number of bits used for value representation.
    const BITS: u32;
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
    u128: Unsigned,
    usize: Unsigned,
    i8: Signed,
    i16: Signed,
    i32: Signed,
    i64: Signed,
    i128: Signed,
    isize: Signed
);
