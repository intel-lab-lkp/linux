// SPDX-License-Identifier: GPL-2.0

//! Traits for transmuting types.

use core::simd::ToBytes;
/// Types for which any bit pattern is valid.
///
/// Not all types are valid for all values. For example, a `bool` must be either zero or one, so
/// reading arbitrary bytes into something that contains a `bool` is not okay.
///
/// It's okay for the type to have padding, as initializing those bytes has no effect.
///
/// # Example
///
/// This example is how to use the FromBytes trait
/// ```
/// // Initialize a slice of bytes
/// let foo = &[1, 2, 3, 4];
///
/// //Use the function implemented by trait in integer type
/// let result = u8::from_bytes(foo);
///
/// assert_eq!(*result, 0x4030201);
/// ```
/// # Safety
///
/// All bit-patterns must be valid for this type. This type must not have interior mutability.
pub unsafe trait FromBytes {
    ///Get an imutable slice of bytes and converts to a reference to Self
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self;
    /// Get a mutable slice of bytes and converts to a reference to Self
    ///
    /// # Safety
    ///
    ///  Bound ToBytes in order to avoid use with disallowed bit patterns
    unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
    where
        Self: ToBytes;
}

//Get a reference of slice of bytes and converts into a reference of integer or a slice with a defined size
macro_rules! impl_frombytes {
    ($($({$($generics:tt)*})? $t:ty, )*) => {
        // SAFETY: Safety comments written in the macro invocation.
        $(unsafe impl$($($generics)*)? FromBytes for $t {
            unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self
            {
                unsafe {
                    let slice_ptr = slice_of_bytes.as_ptr() as *const Self;
                    &*slice_ptr
                }
            }

            unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
            where
                Self: ToBytes,
            {
                unsafe {
                    let slice_ptr = slice_of_bytes.as_mut_ptr() as *mut Self;
                    &mut *slice_ptr
                }

            }
        })*
    };
}

impl_frombytes! {
    // SAFETY: All bit patterns are acceptable values of the types below.
    u8, u16, u32, u64, usize,
    i8, i16, i32, i64, isize,

    // SAFETY: If all bit patterns are acceptable for individual values in an array, then all bit
    // patterns are also acceptable for arrays of that type.
    {<T: FromBytes, const N: usize>} [T; N],
}

/// Get a reference of slice of bytes and converts into a reference of an array of integers
///
/// Types for which any bit pattern is valid.
///
/// Not all types are valid for all values. For example, a `bool` must be either zero or one, so
/// reading arbitrary bytes into something that contains a `bool` is not okay.
///
/// It's okay for the type to have padding, as initializing those bytes has no effect.
///
/// # Example
///
/// This example is how to use the FromBytes trait
/// ```
/// // Initialize a slice of bytes
/// let foo = &[1, 2, 3, 4];
///
/// //Use the function implemented by trait in integer type
/// let result = <[u32]>::from_bytes(slice_of_bytes);
///
/// assert_eq!(*result, 0x4030201);
/// ```
// SAFETY: If all bit patterns are acceptable for individual values in an array, then all bit
// patterns are also acceptable for arrays of that type.
unsafe impl<T: FromBytes> FromBytes for [T] {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
        //Safety: Guarantee that all values are initiliazed
        unsafe {
            let slice_ptr = slice_of_bytes.as_ptr() as *const T;
            let slice_len = slice_of_bytes.len() / core::mem::size_of::<T>();
            core::slice::from_raw_parts(slice_ptr, slice_len)
        }
    }

    //Safety: Guarantee that all values are initiliazed
    unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
    where
        Self: ToBytes,
    {
        //Safety: Guarantee that all values are initiliazed
        unsafe {
            let slice_ptr = slice_of_bytes.as_mut_ptr() as *mut T;
            let slice_len = slice_of_bytes.len() / core::mem::size_of::<T>();
            core::slice::from_raw_parts_mut(slice_ptr, slice_len)
        }
    }
}
