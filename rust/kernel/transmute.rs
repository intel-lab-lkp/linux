// SPDX-License-Identifier: GPL-2.0

//! Traits for transmuting types.

/// Types for which any bit pattern is valid.
///
/// Not all types are valid for all values. For example, a `bool` must be either zero or one, so
/// reading arbitrary bytes into something that contains a `bool` is not okay.
///
/// It's okay for the type to have padding, as initializing those bytes has no effect.
///
/// # Example
/// ```
/// let arr = [1, 2, 3, 4];
///
/// let result = u32::from_bytes(&arr);
///
/// #[cfg(target_endian = "little")]
/// match result {
///     Some(x) => assert_eq!(*x, 0x4030201),
///     None => unreachable!()
/// }
///
/// #[cfg(target_endian = "big")]
/// match result {
///     Some(x) => assert_eq!(*x, 0x1020304),
///     None => unreachable!()
/// }
/// ```
///
/// # Safety
///
/// All bit-patterns must be valid for this type. This type must not have interior mutability.
pub unsafe trait FromBytes {
    /// Converts a slice of bytes to a reference to `Self` when possible.
    fn from_bytes(bytes: &[u8]) -> Option<&Self>;

    /// Converts a mutable slice of bytes to a reference to `Self` when possible.
    fn from_mut_bytes(bytes: &mut [u8]) -> Option<&mut Self>
    where
        Self: AsBytes;
}

macro_rules! impl_frombytes {
    ($($({$($generics:tt)*})? $t:ty, )*) => {
        // SAFETY: Safety comments written in the macro invocation.
        $(unsafe impl$($($generics)*)? FromBytes for $t {
            fn from_bytes(bytes: &[u8]) -> Option<&$t> {
                if bytes.len() == core::mem::size_of::<$t>()
                    && (bytes.as_ptr() as usize) % core::mem::align_of::<$t>() == 0
                {
                    let slice_ptr = bytes.as_ptr().cast::<$t>();
                    unsafe { Some(&*slice_ptr) }
                } else {
                    None
                }
            }

            fn from_mut_bytes(bytes: &mut [u8]) -> Option<&mut $t>
            where
            Self: AsBytes,
            {
                if bytes.len() == core::mem::size_of::<$t>()
                    && (bytes.as_mut_ptr() as usize) % core::mem::align_of::<$t>() == 0
                {
                    let slice_ptr = bytes.as_mut_ptr().cast::<$t>();
                    unsafe { Some(&mut *slice_ptr) }
                } else {
                    None
                }
            }
        })*
    };
}

impl_frombytes! {
    // SAFETY: All bit patterns are acceptable values of the types below.
    // Checking the pointer size and alignment makes this operation safe and it's necessary
    // to dereference to get the value and return it as a reference to `Self`.
    u8, u16, u32, u64, usize,
    i8, i16, i32, i64, isize,
    {<T: FromBytes, const N: usize>} [T; N],
}

// SAFETY: If all bit patterns are acceptable for individual values in an array, then all bit
// patterns are also acceptable for arrays of that type.
unsafe impl<T: FromBytes> FromBytes for [T] {
    fn from_bytes(bytes: &[u8]) -> Option<&Self> {
        if bytes.len() % core::mem::size_of::<T>() == 0
            && (bytes.as_ptr() as usize) % core::mem::align_of::<T>() == 0
        {
            let slice_ptr = bytes.as_ptr().cast::<T>();
            let slice_len = bytes.len() / core::mem::size_of::<T>();
            // SAFETY: Since the code checks the size and alignment, the slice is valid.
            unsafe { Some(core::slice::from_raw_parts(slice_ptr, slice_len)) }
        } else {
            None
        }
    }

    fn from_mut_bytes(bytes: &mut [u8]) -> Option<&mut Self>
    where
        Self: AsBytes,
    {
        if bytes.len() % core::mem::size_of::<T>() == 0
            && (bytes.as_mut_ptr() as usize) % core::mem::align_of::<T>() == 0
        {
            let slice_ptr = bytes.as_mut_ptr().cast::<T>();
            let slice_len = bytes.len() / core::mem::size_of::<T>();
            // SAFETY: Since the code checks the size and alignment, the slice is valid.
            unsafe { Some(core::slice::from_raw_parts_mut(slice_ptr, slice_len)) }
        } else {
            None
        }
    }
}

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

macro_rules! impl_asbytes {
    ($($({$($generics:tt)*})? $t:ty, )*) => {
        // SAFETY: Safety comments written in the macro invocation.
        $(unsafe impl$($($generics)*)? AsBytes for $t {})*
    };
}

impl_asbytes! {
    // SAFETY: Instances of the following types have no uninitialized portions.
    u8, u16, u32, u64, usize,
    i8, i16, i32, i64, isize,
    bool,
    char,
    str,

    // SAFETY: If individual values in an array have no uninitialized portions, then the array
    // itself does not have any uninitialized portions either.
    {<T: AsBytes>} [T],
    {<T: AsBytes, const N: usize>} [T; N],
}
