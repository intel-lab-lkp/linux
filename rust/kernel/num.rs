// SPDX-License-Identifier: GPL-2.0

//! Numerical types for the kernel.

/// Integer type for which only the bits `0..NUM_BITS` are valid.
///
/// TODO: Find a better name? Bounded sounds like we also have a lower bound...
///
/// # Invariants
///
/// Only bits `0..NUM_BITS` can be set on this type.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct BoundedInt<T, const NUM_BITS: u32>(T);

// TODO: this should be implemented by a macro for all integer types.
impl<const NUM_BITS: u32> BoundedInt<u32, NUM_BITS> {
    /// Mask of the valid bits for this type.
    pub const MASK: u32 = crate::bits::genmask_u32(0..=(NUM_BITS - 1));

    /// Validates that `value` is within the bounds supported by this type.
    const fn is_in_bounds(value: u32) -> bool {
        value & !Self::MASK == 0
    }

    /// Checks that `value` is valid for this type at compile-time and build a new value.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::num::BoundedInt;
    ///
    /// assert_eq!(BoundedInt::<u32, 1>::new(1).get(), 1);
    /// assert_eq!(BoundedInt::<u32, 8>::new(0xff).get(), 0xff);
    /// assert_eq!(BoundedInt::<u32, { 10 - 3 }>::new(1).get(), 1);
    pub const fn new(value: u32) -> Self {
        crate::build_assert!(
            Self::is_in_bounds(value),
            "Provided parameter is larger than maximal supported value"
        );

        Self(value)
    }

    /// Returns the contained value as a primitive type.
    pub const fn get(self) -> u32 {
        if !Self::is_in_bounds(self.0) {
            // SAFETY: Per the invariants, `self.0` cannot have bits set outside of `MASK`, so
            // this block will
            // never be reached.
            unsafe { core::hint::unreachable_unchecked() }
        }
        self.0
    }
}

impl<const NUM_BITS: u32> From<BoundedInt<u32, NUM_BITS>> for u32 {
    fn from(value: BoundedInt<u32, NUM_BITS>) -> Self {
        value.get()
    }
}

/// Attempt to convert a non-bounded integer to a bounded equivalent.
impl<const NUM_BITS: u32> TryFrom<u32> for BoundedInt<u32, NUM_BITS> {
    type Error = crate::error::Error;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        if !Self::is_in_bounds(value) {
            Err(crate::prelude::EINVAL)
        } else {
            Ok(Self(value))
        }
    }
}

/// Allow comparison with non-bounded values.
impl<const NUM_BITS: u32, T> PartialEq<T> for BoundedInt<T, NUM_BITS>
where
    T: PartialEq,
{
    fn eq(&self, other: &T) -> bool {
        self.0 == *other
    }
}

impl<const NUM_BITS: u32, T> core::fmt::Debug for BoundedInt<T, NUM_BITS>
where
    T: core::fmt::Debug,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.0.fmt(f)
    }
}

impl<const NUM_BITS: u32, T> core::fmt::Display for BoundedInt<T, NUM_BITS>
where
    T: core::fmt::Display,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.0.fmt(f)
    }
}

impl<const NUM_BITS: u32, T> core::fmt::LowerHex for BoundedInt<T, NUM_BITS>
where
    T: core::fmt::LowerHex,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.0.fmt(f)
    }
}

impl<const NUM_BITS: u32, T> core::fmt::UpperHex for BoundedInt<T, NUM_BITS>
where
    T: core::fmt::UpperHex,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.0.fmt(f)
    }
}
