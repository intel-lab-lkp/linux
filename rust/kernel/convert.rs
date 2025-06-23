// SPDX-License-Identifier: GPL-2.0

//! Traits for type conversion.

/// A trait for fallible conversions from primitive types.
///
/// [`FromPrimitive`] allows converting from various built-in primitive types
/// (such as integers and `bool`) into a user-defined type, typically an `enum`.
///
/// At least [`from_i64`] and [`from_u64`] should be implemented. All other methods
/// have default implementations that convert to `i64` or `u64` using fallible casts and
/// delegate to those two core methods.
///
/// Enums with wide representations such as `#[repr(i128)]` or `#[repr(u128)]` may lose
/// information through narrowing in the default implementations. In such cases, override
/// [`from_i128`] and [`from_u128`] explicitly.
///
/// This trait can be used with `#[derive]`.
/// See [`FromPrimitive`](../../macros/derive.FromPrimitive.html) derive macro for more
/// information.
///
/// [`from_i64`]: FromPrimitive::from_i64
/// [`from_i128`]: FromPrimitive::from_i128
/// [`from_u64`]: FromPrimitive::from_u64
/// [`from_u128`]: FromPrimitive::from_u128
///
/// # Examples
///
/// ```rust
/// use kernel::convert::FromPrimitive;
///
/// #[derive(PartialEq)]
/// enum Foo {
///     A,
///     B = 0x17,
///     C = -2,
/// }
///
/// impl FromPrimitive for Foo {
///     fn from_i64(n: i64) -> Option<Self> {
///         match n {
///             0 => Some(Self::A),
///             0x17 => Some(Self::B),
///             -2 => Some(Self::C),
///             _ => None,
///         }
///     }
///
///     fn from_u64(n: u64) -> Option<Self> {
///         i64::try_from(n).ok().and_then(Self::from_i64)
///     }
/// }
///
/// assert_eq!(Foo::from_u64(0), Some(Foo::A));
/// assert_eq!(Foo::from_u64(0x17), Some(Foo::B));
/// assert_eq!(Foo::from_i64(-2), Some(Foo::C));
/// assert_eq!(Foo::from_i64(-3), None);
/// ```
pub trait FromPrimitive: Sized {
    /// Attempts to convert a `bool` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_bool(b: bool) -> Option<Self> {
        Self::from_u64(u64::from(b))
    }

    /// Attempts to convert an `isize` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_isize(n: isize) -> Option<Self> {
        i64::try_from(n).ok().and_then(Self::from_i64)
    }

    /// Attempts to convert an `i8` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_i8(n: i8) -> Option<Self> {
        Self::from_i64(i64::from(n))
    }

    /// Attempts to convert an `i16` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_i16(n: i16) -> Option<Self> {
        Self::from_i64(i64::from(n))
    }

    /// Attempts to convert an `i32` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_i32(n: i32) -> Option<Self> {
        Self::from_i64(i64::from(n))
    }

    /// Attempts to convert an `i64` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    fn from_i64(n: i64) -> Option<Self>;

    /// Attempts to convert an `i128` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    ///
    /// The default implementation delegates to [`from_i64`](FromPrimitive::from_i64)
    /// by downcasting from `i128` to `i64`, which may result in information loss.
    /// Consider overriding this method if `Self` can represent values outside the
    /// `i64` range.
    #[inline]
    fn from_i128(n: i128) -> Option<Self> {
        i64::try_from(n).ok().and_then(Self::from_i64)
    }

    /// Attempts to convert a `usize` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_usize(n: usize) -> Option<Self> {
        u64::try_from(n).ok().and_then(Self::from_u64)
    }

    /// Attempts to convert a `u8` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_u8(n: u8) -> Option<Self> {
        Self::from_u64(u64::from(n))
    }

    /// Attempts to convert a `u16` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_u16(n: u16) -> Option<Self> {
        Self::from_u64(u64::from(n))
    }

    /// Attempts to convert a `u32` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    #[inline]
    fn from_u32(n: u32) -> Option<Self> {
        Self::from_u64(u64::from(n))
    }

    /// Attempts to convert a `u64` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    fn from_u64(n: u64) -> Option<Self>;

    /// Attempts to convert a `u128` to `Self`. Returns `Some(Self)` if the input
    /// corresponds to a known value; otherwise, `None`.
    ///
    /// The default implementation delegates to [`from_u64`](FromPrimitive::from_u64)
    /// by downcasting from `u128` to `u64`, which may result in information loss.
    /// Consider overriding this method if `Self` can represent values outside the
    /// `u64` range.
    #[inline]
    fn from_u128(n: u128) -> Option<Self> {
        u64::try_from(n).ok().and_then(Self::from_u64)
    }
}
