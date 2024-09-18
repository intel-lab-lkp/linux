// SPDX-License-Identifier: GPL-2.0

//! Traits for transmuting types.

/// Types for which any bit pattern is valid.
///
/// Not all types are valid for all values. For example, a `bool` must be either zero or one, so
/// reading arbitrary bytes into something that contains a `bool` is not okay.
///
/// It's okay for the type to have padding, as initializing those bytes has no effect.
///
/// # Safety
///
/// All bit-patterns must be valid for this type. This type must not have interior mutability.
pub unsafe trait FromBytes {}

// SAFETY: All bit patterns are acceptable values of the types below.
unsafe impl FromBytes for u8 {}
unsafe impl FromBytes for u16 {}
unsafe impl FromBytes for u32 {}
unsafe impl FromBytes for u64 {}
unsafe impl FromBytes for usize {}
unsafe impl FromBytes for i8 {}
unsafe impl FromBytes for i16 {}
unsafe impl FromBytes for i32 {}
unsafe impl FromBytes for i64 {}
unsafe impl FromBytes for isize {}
// SAFETY: If all bit patterns are acceptable for individual values in an array, then all bit
// patterns are also acceptable for arrays of that type.
unsafe impl<T: FromBytes> FromBytes for [T] {}
unsafe impl<T: FromBytes, const N: usize> FromBytes for [T; N] {}

/// Types that can be viewed as an immutable slice of initialized bytes.
///
/// If a struct implements this trait, then it is okay to copy it byte-for-byte to userspace. This
/// means that it should not have any padding, as padding bytes are uninitialized. Reading
/// uninitialized memory is not just undefined behavior, it may even lead to leaking sensitive
/// information on the stack to userspace.
///
/// The struct should also not hold kernel pointers, as kernel pointer addresses are also considered
/// sensitive. However, leaking kernel pointers is not considered undefined behavior by Rust, so
/// this is a correctness requirement, but not a safety requirement.
///
/// # Safety
///
/// Values of this type may not contain any uninitialized bytes. This type must not have interior
/// mutability.
pub unsafe trait AsBytes {}

// SAFETY: Instances of the following types have no uninitialized portions.
unsafe impl AsBytes for u8 {}
unsafe impl AsBytes for u16 {}
unsafe impl AsBytes for u32 {}
unsafe impl AsBytes for u64 {}
unsafe impl AsBytes for usize {}
unsafe impl AsBytes for i8 {}
unsafe impl AsBytes for i16 {}
unsafe impl AsBytes for i32 {}
unsafe impl AsBytes for i64 {}
unsafe impl AsBytes for isize {}
unsafe impl AsBytes for bool {}
unsafe impl AsBytes for char {}
unsafe impl AsBytes for str {}
// SAFETY: If individual values in an array have no uninitialized portions, then the array itself
// does not have any uninitialized portions either.
unsafe impl<T: AsBytes> AsBytes for [T] {}
unsafe impl<T: AsBytes, const N: usize> AsBytes for [T; N] {}
