// SPDX-License-Identifier: GPL-2.0

//! Numerical helpers functions and traits.
//!
//! This is essentially a staging module for code to mature until it can be moved to the `kernel`
//! crate.

/// Infallibly converts a `usize` to `u64`.
///
/// This conversion is always lossless as Linux only supports 32-bit and 64-bit platforms, thus a
/// `usize` is always smaller than or of the same size as a `u64`.
///
/// Prefer this over the `as` keyword to ensure no lossy conversions are performed.
///
/// This is for use from a `const` context. For non `const` use, prefer the [`FromAs`] and
/// [`IntoAs`] traits.
pub(crate) const fn usize_as_u64(value: usize) -> u64 {
    kernel::static_assert!(size_of::<u64>() >= size_of::<usize>());

    value as u64
}

#[cfg(CONFIG_32BIT)]
/// Infallibly converts a `usize` to `u32` on 32-bit platforms.
///
/// This conversion is always lossless on 32-bit platforms, where a `usize` is the same size as a
/// `u32`.
///
/// Prefer this over the `as` keyword to ensure no lossy conversions are performed.
///
/// This is for use from a `const` context. For non `const` use, prefer the [`FromAs`] and
/// [`IntoAs`] traits.
pub(crate) const fn usize_as_u32(value: usize) -> u32 {
    kernel::static_assert!(size_of::<u32>() >= size_of::<usize>());

    value as u32
}

/// Infallibly converts a `u32` to `usize`.
///
/// This conversion is always lossless as Linux only supports 32-bit and 64-bit platforms, thus a
/// `u32` is always smaller than or of the same size as a `usize`.
///
/// Prefer this over the `as` keyword to ensure no lossy conversions are performed.
///
/// This is for use from a `const` context. For non `const` use, prefer the [`FromAs`] and
/// [`IntoAs`] traits.
pub(crate) const fn u32_as_usize(value: u32) -> usize {
    kernel::static_assert!(size_of::<usize>() >= size_of::<u32>());

    value as usize
}

#[cfg(CONFIG_64BIT)]
/// Infallibly converts a `u64` to `usize` on 64-bit platforms.
///
/// This conversion is always lossless on 64-bit platforms, where a `usize` is the same size as a
/// `u64`.
///
/// Prefer this over the `as` keyword to ensure no lossy conversions are performed.
///
/// This is for use from a `const` context. For non `const` use, prefer the [`FromAs`] and
/// [`IntoAs`] traits.
pub(crate) const fn u64_as_usize(value: u64) -> usize {
    kernel::static_assert!(size_of::<usize>() >= size_of::<u64>());

    value as usize
}

/// Extension trait providing guaranteed lossless conversion to `Self` from `T`.
///
/// The standard library's `From` implementations do not cover conversions that are not portable or
/// future-proof. For instance, even though it is safe today, `From<usize>` is not implemented for
/// `u64` because of the possibility to support larger-than-64bit architectures in the future.
///
/// The workaround is to either deal with the error handling of `TryFrom` for an operation that
/// technically cannot fail, or to use the `as` keyword, which can silently strip data if the
/// destination type is smaller than the source.
///
/// Both options are hardly acceptable for the kernel. It is also a much more architecture
/// dependent environment, supporting only 32 and 64 bit architectures, with some modules
/// explicitly depending on a specific bus witdth that could greatly benefit from infallible
/// conversion operations.
///
/// Thus this extension trait that provides, for the architecture the kernel is built for, safe
/// conversion between types for which such conversion is lossless.
///
/// In other words, this trait is implemented if, for the current build target and with `t: T`, the
/// `t as Self` operation is completely lossless.
///
/// Prefer this over the `as` keyword to ensure no lossy conversions are performed.
///
/// If you need to perform a conversion in `const` context, use [`u64_as_usize`],
/// [`u32_as_usize`], [`usize_as_u64`], or [`usize_as_u32`].
///
/// # Examples
///
/// ```
/// use crate::num::FromAs;
///
/// assert_eq!(usize::from_as(0xf00u32), 0xf00u32 as usize);
/// ```
pub(crate) trait FromAs<T> {
    /// Create a `Self` from `value`. This operation is guaranteed to be lossless.
    fn from_as(value: T) -> Self;
}

impl FromAs<usize> for u64 {
    fn from_as(value: usize) -> Self {
        usize_as_u64(value)
    }
}

#[cfg(CONFIG_32BIT)]
impl FromAs<usize> for u32 {
    fn from_as(value: usize) -> Self {
        usize_as_u32(value)
    }
}

impl FromAs<u32> for usize {
    fn from_as(value: u32) -> Self {
        u32_as_usize(value)
    }
}

#[cfg(CONFIG_64BIT)]
impl FromAs<u64> for usize {
    fn from_as(value: u64) -> Self {
        u64_as_usize(value)
    }
}

/// Counterpart to the [`FromAs`] trait, i.e. this trait is to [`FromAs`] what [`Into`] is to
/// [`From`].
///
/// See the documentation of [`FromAs`] for the motivation.
///
/// # Examples
///
/// ```
/// use crate::num::IntoAs;
///
/// assert_eq!(0xf00u32.into_as(), 0xf00u32 as usize);
/// ```
pub(crate) trait IntoAs<T> {
    /// Convert `self` into a `T`. This operation is guaranteed to be lossless.
    fn into_as(self) -> T;
}

/// Reverse operation for types implementing [`FromAs`].
impl<S, T> IntoAs<T> for S
where
    T: FromAs<S>,
{
    fn into_as(self) -> T {
        T::from_as(self)
    }
}
