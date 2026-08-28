// SPDX-License-Identifier: GPL-2.0

//! Additional numerical features for the kernel.

use core::{
    marker::PhantomData,
    num::NonZero,
    ops, //
};

use crate::{
    build_assert::const_eval,
    prelude::*, //
};

pub mod bounded;
pub mod casts;

pub use bounded::*;

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

/// Types that can be created from an integer constant expression validated during const evaluation.
#[diagnostic::on_unimplemented(message = "`{Self}` cannot be converted from constant")]
pub trait FromConst: Sized {
    /// Create `Self` from constant `v`, fails the build if `v` is not valid for `Self`.
    ///
    /// This function must only be called during const evaluation.
    ///
    /// # Note
    ///
    /// This function is for documentation purpose only, showing what the trait would look like when
    /// const trait implementation is available. Use [`cv!`] macro instead of calling this function.
    #[cfg(doc)]
    fn from_const(v: i128) -> Self {
        build_error!("For documentation purpose only");
    }
}

// Helper for type inference in the `cv!` macro.
#[doc(hidden)]
pub struct FromConstInferHelper<T>(PhantomData<T>);

impl<T> FromConstInferHelper<T> {
    #[expect(clippy::new_without_default)]
    #[const_eval]
    pub const fn new() -> Self
    where
        // This is on function and not on the impl block so the function always exist. Otherwise we
        // can "function exists but trait was not satisfied" error instead of "trait not
        // implemented" error.
        T: FromConst,
    {
        Self(PhantomData)
    }

    // A helper to help type inference to let type inference know that the return type of
    // `__from_const` is exactly `T`.
    #[const_eval]
    pub const fn infer(self) -> T {
        panic!("For type inference only");
    }

    // Convert to the `FromConstMethod`, so `__from_const` can be called.
    //
    // We have to split helpers because once a type implements `Deref`, the type must be known when
    // calling method on it because Rust needs to walk the deref chain. Thus, `infer` is on the
    // `FromConstInferHelper` which does not implement `Deref`, while `__from_const` is on
    // `FromConstMethod` which we can use deref to dispatch.
    #[const_eval]
    pub const fn method(self) -> FromConstMethod<T> {
        FromConstMethod(PhantomData)
    }
}

// Helper type that we define `__from_const` inherent method on, so they can be marked as const.
#[doc(hidden)]
pub struct FromConstMethod<T>(PhantomData<T>);

impl<T> FromConstMethod<T> {
    // A fallback method that is selected if no `__from_const` can be found on concrete types. This
    // is needed to avoid "__from_const" doesn't exist error, and have a proper "trait not
    // implemented" error instead.
    //
    // This is selected after concrete `__from_const` because it takes `&self` as receiver, and not
    // `self`; auto-ref has lower priority in method resolution, so methods that take
    // `FromConstMethod<T>` is selected first.
    //
    // This method may also be selected without accompanying "trait not implemented" error if the
    // type implementing `FromConst` is generic (e.g. `cv!(foo => T)`) in a function with `T:
    // FromConst` bound; so it also produce a proper error message for that scenario.
    #[const_eval]
    pub const fn __from_const(&self, _: i128) -> T {
        panic!("`cv!()` cannot be used with generic types yet");
    }
}

// Enable the use of `FromConstMethod<T>` as receiver type on `FromConstMethodWrap<T>`.
impl<T> ops::Deref for FromConstMethod<T> {
    type Target = FromConstMethodWrap<T>;

    #[inline(always)]
    fn deref(&self) -> &Self::Target {
        build_error!("For receiver only");
    }
}

// Helper type that we define inherent method on for foreign types.
#[doc(hidden)]
pub struct FromConstMethodWrap<T>(PhantomData<T>);

// Enable the use of `FromConstMethod<T>` as receiver type on `T`.
impl<T> ops::Deref for FromConstMethodWrap<T> {
    type Target = T;

    #[inline(always)]
    fn deref(&self) -> &Self::Target {
        build_error!("For receiver only");
    }
}

macro_rules! impl_from_const_primitive {
    ($($ty:ty)*) => {$(
        impl FromConst for $ty {}

        impl FromConstMethodWrap<$ty> {
            #[const_eval]
            pub const fn __from_const(self: FromConstMethod<$ty>, v: i128) -> $ty {
                assert!(
                     v >= <$ty>::MIN as i128 && v <= <$ty>::MAX as i128,
                     concat!("constant cannot be represented by `", stringify!($ty), "`"),
                );

                v as $ty
            }
        }

        impl FromConst for NonZero<$ty> {}

        impl FromConstMethodWrap<NonZero<$ty>> {
            #[const_eval]
            pub const fn __from_const(
                self: FromConstMethod<NonZero<$ty>>, v: i128
            ) -> NonZero<$ty> {
                assert!(
                     v >= <$ty>::MIN as i128 && v <= <$ty>::MAX as i128,
                     concat!("constant cannot be represented by `", stringify!($ty), "`"),
                );

                match NonZero::new(v as $ty) {
                    Some(v) => v,
                    None => panic!("constant is zero"),
                }
            }
        }
    )*};
}

impl_from_const_primitive!(
    u8 u16 u32 u64 usize
    i8 i16 i32 i64 isize
);

/// Creates a value from an integer constant expression, with validity checked at build time.
///
/// This works for any type that implements [`FromConst`]. If a target type is not specified, it is
/// inferred from the context.
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
///
/// // Explicit type specification.
/// assert_eq!(cv!(1 => NonZero<u8>).get(), 1);
/// ```
#[macro_export]
#[doc(hidden)]
macro_rules! cv {
    ($v:expr $(=> $ty:ty)?) => { const {
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

            let helper = $crate::num::FromConstInferHelper$(::<$ty>)?::new();
            if false {
                helper.infer()
            } else {
                helper.method().__from_const(r)
            }
        }
    }};
}
#[doc(inline)]
pub use cv;
