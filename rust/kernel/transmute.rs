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
pub unsafe trait FromBytes {
    ///Converts a slice of Bytes into a Reference to Self
    ///
    /// # Examples
    /// ```
    ///    pub unsafe trait FromBytes {
    ///        unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self;
    ///        unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
    ///        where
    ///            Self: ToBytes;
    ///    }
    ///
    ///unsafe impl FromBytes for u32 {
    ///    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
    ///        let slice_ptr = slice_of_bytes.as_ptr() as *const Self;
    ///        &*slice_ptr
    ///    }
    ///
    ///    unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
    ///    where
    ///        Self: ToBytes,
    ///    {
    ///        let slice_ptr = slice_of_bytes.as_mut_ptr() as *mut Self;
    ///        &mut *slice_ptr
    ///    }
    ///}
    ///
    ///let slice_of_bytes : &[u8] = &[1, 2, 3, 4];
    ///let result = u32::from_bytes(slice_of_bytes);
    ///assert_eq!(*result, 0x4030201);
    ///```
    ///# Safety
    ///
    ///Guarantee that all values are initiliazed
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self;
    ///Converts a mutabble slice of Bytes into a mutable Reference to Self
    /// # Safety
    ///
    /// ToBytes in order to allow only types that implements ToBytes
    unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
    where
        Self: ToBytes;
}

// SAFETY: All bit patterns are acceptable values of the types below.
unsafe impl FromBytes for u8 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for u16 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for u32 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for u64 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for usize {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for i8 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for i16 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for i32 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for i64 {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}

unsafe impl FromBytes for isize {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
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
}
// SAFETY: If all bit patterns are acceptable for individual values in an array, then all bit
// patterns are also acceptable for arrays of that type.
unsafe impl<T: FromBytes> FromBytes for [T] {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
        unsafe {
            let slice_ptr = slice_of_bytes.as_ptr() as *const T;
            let slice_len = slice_of_bytes.len() / core::mem::size_of::<T>();
            core::slice::from_raw_parts(slice_ptr, slice_len)
        }
    }

    unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
    where
        Self: ToBytes,
    {
        unsafe {
            let slice_ptr = slice_of_bytes.as_mut_ptr() as *mut T;
            let slice_len = slice_of_bytes.len() / core::mem::size_of::<T>();
            core::slice::from_raw_parts_mut(slice_ptr, slice_len)
        }
    }
}

/// # Examples
///```
///let slice_of_bytes: &[u8] = &[
///    1, 0, 0, 0,
///    2, 0, 0, 0,
///    3, 0, 0, 0,
///    4, 0, 0, 0,
///    5, 0, 0, 0,
///    6, 0, 0, 0,
///    7, 0, 0, 0,
///    8, 0, 0, 0,
///];
///
///let foo = <[u32; 8]>::from_bytes(slice_of_bytes);
///let expected: [u32; 8] = [1, 2, 3, 4, 5, 6, 7, 8];
///
///assert_eq!(*foo, expected);
///```
unsafe impl<T: FromBytes, const N: usize> FromBytes for [T; N] {
    unsafe fn from_bytes(slice_of_bytes: &[u8]) -> &Self {
        unsafe {
            let slice_ptr = slice_of_bytes.as_ptr() as *const T;
            &*(slice_ptr as *const [T; N])
        }
    }

    unsafe fn from_bytes_mut(slice_of_bytes: &mut [u8]) -> &mut Self
    where
        Self: ToBytes,
    {
        unsafe {
            let slice_ptr = slice_of_bytes.as_ptr() as *mut T;
            &mut *(slice_ptr as *mut [T; N])
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
