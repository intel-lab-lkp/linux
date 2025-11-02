// SPDX-License-Identifier: GPL-2.0

//! [`BitInt`], a primitive integer type with a limited set of bits usable to represent values.

use core::ops::Deref;

use kernel::num::Integer;
use kernel::prelude::*;

/// Evaluates to `true` if `$value` can be represented using at most `$num_bits` on `$type`.
///
/// Can be used in const context.
macro_rules! fits_within {
    ($value:expr, $type:ty, $num_bits:expr) => {{
        let shift: u32 = <$type>::BITS - $num_bits;

        // The value fits within `NUM_BITS` if shifting it left by the number of unused bits,
        // then right by the same number, doesn't change the value.
        //
        // This method has the benefit of working with both unsigned and signed integers.
        ($value << shift) >> shift == $value
    }};
}

/// Trait for primitive integer types that can be used to back a [`BitInt`].
///
/// This is mostly used to lock all the operations we need for [`BitInt`] in a single trait.
pub trait Boundable
where
    Self: Integer
        + Sized
        + Copy
        + core::ops::Shl<u32, Output = Self>
        + core::ops::Shr<u32, Output = Self>
        + core::cmp::PartialEq,
    Self: TryInto<u8> + TryInto<u16> + TryInto<u32> + TryInto<u64>,
    Self: TryInto<i8> + TryInto<i16> + TryInto<i32> + TryInto<i64>,
{
    /// Returns `true` if `value` can be represented with at most `NUM_BITS` on `T`.
    fn fits_within(value: Self, num_bits: u32) -> bool {
        fits_within!(value, Self, num_bits)
    }
}

/// Implement `Boundable` for all integer types.
impl<T> Boundable for T
where
    T: Integer
        + Sized
        + Copy
        + core::ops::Shl<u32, Output = Self>
        + core::ops::Shr<u32, Output = Self>
        + core::cmp::PartialEq,
    Self: TryInto<u8> + TryInto<u16> + TryInto<u32> + TryInto<u64>,
    Self: TryInto<i8> + TryInto<i16> + TryInto<i32> + TryInto<i64>,
{
}

/// Integer type for which only the `NUM_BITS` less significant bits can ever be set.
///
/// # Invariants
///
/// - `NUM_BITS` is greater than `0`.
/// - `NUM_BITS` is less or equal to `T::BITS`.
/// - Stored values are represented with at most `NUM_BITS` bits.
///
/// # Examples
///
/// The preferred way to create values is through constants and the [`BitInt::new`] family of
/// constructors, as they trigger a build error if the type invariants cannot be withheld.
///
/// ```
/// use kernel::num::BitInt;
///
/// // An unsigned 8-bit integer, of which only the 4 LSBs can ever be set.
/// // The value `15` is statically validated to fit that constraint at build time.
/// let v = BitInt::<u8, 4>::new::<15>();
/// assert_eq!(v.get(), 15);
///
/// // Same using signed values.
/// let v = BitInt::<i8, 4>::new::<-8>();
/// assert_eq!(v.get(), -8);
///
/// // This doesn't build: a `u8` is smaller than the requested 9 bits.
/// // let _ = BitInt::<u8, 9>::new::<10>();
///
/// // This also doesn't build: the requested value doesn't fit within 4 signed bits.
/// // let _ = BitInt::<i8, 4>::new::<8>();
/// ```
/// Values can also be validated at runtime with [`BitInt::try_new`].
///
/// ```
/// use kernel::num::BitInt;
///
/// //  This succeeds because `15` can be represented with 4 unsigned bits.
/// assert!(BitInt::<u8, 4>::try_new(15).is_some());
/// // This fails because `16` cannot be represented with 4 unsigned bits.
/// assert!(BitInt::<u8, 4>::try_new(16).is_none());
/// ```
///
/// Non-constant expressions can be validated at build-time thanks to compiler optimizations. This
/// should be used as a last resort though.
///
/// ```
/// use kernel::num::BitInt;
/// # fn some_number() -> u32 { 0xffffffff }
///
/// // Here the compiler can infer from the mask that the type invariants are not violated, even
/// // though the value returned by `some_number` is not known.
/// let v = BitInt::<u32, 4>::from_expr(some_number() & 0xf);
/// ```
///
/// [`BitInt`]s can be compared regardless of their number of valid bits, as long as their backing
/// types can be compared.
///
/// ```
/// use kernel::num::BitInt;
///
/// let v1 = BitInt::<u32, 8>::new::<4>();
/// let v2 = BitInt::<u32, 4>::new::<15>();
///
/// assert!(v1 != v2);
/// assert!(v1 < v2);
/// ```
///
/// Common integer operations are supported between a [`BitInt`] and its backing type.
///
/// ```
/// use kernel::num::BitInt;
///
/// let v = BitInt::<u8, 4>::new::<15>();
///
/// assert_eq!(v + 5, 20);
/// assert_eq!(v / 3, 5);
/// assert!(v == 15);
/// assert!(v > 12);
/// ```
///
/// Conversion is possible between backing types using [`BitInt::cast`], and the number of valid
/// bits can be extended or reduced with [`BitInt::extend`] and [`BitInt::try_shrink`].
///
/// ```
/// use kernel::num::BitInt;
///
/// let v = BitInt::<u32, 12>::new::<127>();
///
/// // Changes backing type from `u32` to `u16`.
/// let _: BitInt<u16, 12> = v.cast();
///
/// // This does not build, as `u8` is smaller than 12 bits.
/// // let _: BitInt<u8, 12> = v.cast();
///
/// // We can safely extend the number of bits...
/// let _ = v.extend::<15>();
///
/// // ... to the limits of the backing type. This doesn't build as a `u32` cannot contain 33 bits.
/// // let _ = v.extend::<33>();
///
/// // Reducing the number of bits is validated at runtime. This works because `127` can be
/// // represented with 8 bits.
/// assert!(v.try_shrink::<8>().is_some());
///
/// // ... but not with 6, so this fails.
/// assert!(v.try_shrink::<6>().is_none());
/// ```
///
/// Infallible conversions from a primitive integer to a large-enough [`BitInt`] are supported.
///
/// ```
/// use kernel::num::BitInt;
///
/// // This unsigned `BitInt` has 8 bits, so it can represent any `u8`.
/// let v = BitInt::<u32, 8>::from(128u8);
/// assert_eq!(v.get(), 128);
///
/// // This signed `BitInt` has 8 bits, so it can represent any `i8`.
/// let v = BitInt::<i32, 8>::from(-128i8);
/// assert_eq!(v.get(), -128);
///
/// // This doesn't build, as this 6-bit `BitInt` does not have enough capacity to represent a
/// // `u8` (regardless of the passed value).
/// // let _ = BitInt::<u32, 6>::from(10u8);
///
/// // Booleans can be converted into single-bit `BitInt`s.
///
/// let v = BitInt::<u64, 1>::from(false);
/// assert_eq!(v.get(), 0);
///
/// let v = BitInt::<u64, 1>::from(true);
/// assert_eq!(v.get(), 1);
/// ```
///
/// Infallible conversions from a [`BitInt`] to a primitive integer is also supported, and
/// dependent on the number of bits used for value representation, not on the backing type.
///
/// ```
/// use kernel::num::BitInt;
///
/// // Even though its backing type is `u32`, this `BitInt` only uses 6 bits and thus can safely
/// // be converted to a `u8`.
/// let v = BitInt::<u32, 6>::new::<63>();
/// assert_eq!(u8::from(v), 63);
///
/// // Same using signed values.
/// let v = BitInt::<i32, 8>::new::<-128>();
/// assert_eq!(i8::from(v), -128);
///
/// // This however does not build, as 10 bits won't fit into a `u8` (regardless of the actually
/// // contained value).
/// let _v = BitInt::<u32, 10>::new::<10>();
/// // assert_eq!(u8::from(_v), 10);
///
/// // Single-bit `BitInt`s can be converted into a boolean.
/// let v = BitInt::<u8, 1>::new::<1>();
/// assert_eq!(bool::from(v), true);
///
/// let v = BitInt::<u8, 1>::new::<0>();
/// assert_eq!(bool::from(v), false);
/// ```
///
/// Fallible conversions from any primitive integer to any [`BitInt`] are also supported using the
/// [`TryIntoBitInt`] trait.
///
/// ```
/// use kernel::num::{BitInt, TryIntoBitInt};
///
/// // Succeeds because `128` fits into 8 bits.
/// let v: Option<BitInt<u16, 8>> = 128u32.try_into_bitint();
/// assert_eq!(v.as_deref().copied(), Some(128));
///
/// // Fails because `128` doesn't fits into 6 bits.
/// let v: Option<BitInt<u16, 6>> = 128u32.try_into_bitint();
/// assert_eq!(v, None);
/// ```
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, Hash)]
pub struct BitInt<T: Boundable, const NUM_BITS: u32>(T);

/// Validating the value as a const expression cannot be done as a regular method, as the
/// arithmetic operations we rely on to check the bounds are not const. Thus, implement
/// [`BitInt::new`] using a macro.
macro_rules! impl_const_new {
    ($($type:ty)*) => {
        $(
        impl<const NUM_BITS: u32> BitInt<$type, NUM_BITS> {
            /// Creates a [`BitInt`] for the constant `VALUE`.
            ///
            /// Fails at build time if `VALUE` cannot be represented with `NUM_BITS`.
            ///
            /// This method should be preferred to [`Self::from_expr`] whenever possible.
            ///
            /// # Examples
            /// ```
            /// use kernel::num::BitInt;
            ///
            #[doc = ::core::concat!(
                "let v = BitInt::<",
                ::core::stringify!($type),
                ", 4>::new::<7>();")]
            /// assert_eq!(v.get(), 7);
            /// ```
            pub const fn new<const VALUE: $type>() -> Self {
                // Statically assert that `VALUE` fits within the set number of bits.
                const {
                    build_assert!(fits_within!(VALUE, $type, NUM_BITS));
                }

                // INVARIANT: `fits_within` confirmed that `value` can be represented within
                // `NUM_BITS`.
                Self::__new(VALUE)
            }
        }
        )*
    };
}

impl_const_new!(u8 u16 u32 u64);
impl_const_new!(i8 i16 i32 i64);

impl<T, const NUM_BITS: u32> BitInt<T, NUM_BITS>
where
    T: Boundable,
{
    /// Private constructor enforcing the type invariants.
    ///
    /// All instances of [`BitInt`] must be created through this method as it enforces most of the
    /// type invariants.
    ///
    /// The caller remains responsible for checking, either statically or dynamically, that `value`
    /// can be represented as a `T` using at most `NUM_BITS` bits.
    const fn __new(value: T) -> Self {
        // Enforce the type invariants.
        const {
            // `NUM_BITS` cannot be zero.
            build_assert!(NUM_BITS != 0);
            // The backing type is at least as large as `NUM_BITS`.
            build_assert!(NUM_BITS <= T::BITS);
        }

        Self(value)
    }

    /// Attempts to turn `value` into a `BitInt` using `NUM_BITS`.
    ///
    /// Returns [`None`] if `value` doesn't fit within `NUM_BITS`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::num::BitInt;
    ///
    /// let v = BitInt::<u8, 1>::try_new(1);
    /// assert_eq!(v.as_deref().copied(), Some(1));
    ///
    /// let v = BitInt::<i8, 4>::try_new(-2);
    /// assert_eq!(v.as_deref().copied(), Some(-2));
    ///
    /// // `0x1ff` doesn't fit into 8 unsigned bits.
    /// let v = BitInt::<u32, 8>::try_new(0x1ff);
    /// assert_eq!(v, None);
    ///
    /// // `8` doesn't fit into 4 signed bits.
    /// let v = BitInt::<i8, 4>::try_new(8);
    /// assert_eq!(v, None);
    /// ```
    pub fn try_new(value: T) -> Option<Self> {
        T::fits_within(value, NUM_BITS).then(|| {
            // INVARIANT: `fits_within` confirmed that `value` can be represented within `NUM_BITS`.
            Self::__new(value)
        })
    }

    /// Checks that `expr` is valid for this type at compile-time and build a new value.
    ///
    /// This relies on [`build_assert!`] and guaranteed optimization to perform validation at
    /// compile-time. If `expr` cannot be proved to be within the requested bounds at compile-time,
    /// use the fallible [`Self::try_new`] instead.
    ///
    /// Whenever possible, use one of the [`Self::new`] constructors instead of this one as it
    /// statically validates `expr` instead of relying on compiler optimizations.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::num::BitInt;
    ///
    /// # fn some_number() -> u32 { 0xffffffff }
    ///
    /// // Some undefined number.
    /// let v: u32 = some_number();
    ///
    /// // Triggers a build error as `v` cannot be asserted to fit within 4 bits...
    /// // let _ = BitInt::<u32, 4>::from_expr(v);
    ///
    /// // ... but this works as the compiler can assert the range from the mask.
    /// let _ = BitInt::<u32, 4>::from_expr(v & 0xf);
    ///
    /// // These expressions are simple enough to be proven correct, but since they are static the
    /// // `new` constructor should be preferred.
    /// assert_eq!(BitInt::<u8, 1>::from_expr(1).get(), 1);
    /// assert_eq!(BitInt::<u16, 8>::from_expr(0xff).get(), 0xff);
    /// ```
    pub fn from_expr(expr: T) -> Self {
        crate::build_assert!(
            T::fits_within(expr, NUM_BITS),
            "Requested value larger than maximal representable value."
        );

        // INVARIANT: `fits_within` confirmed that `expr` can be represented within `NUM_BITS`.
        Self::__new(expr)
    }

    /// Returns the contained value as the backing type.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::num::BitInt;
    ///
    /// let v = BitInt::<u32, 4>::new::<7>();
    /// assert_eq!(v.get(), 7u32);
    /// ```
    pub fn get(self) -> T {
        *self.deref()
    }

    /// Increases the number of bits usable for `self`.
    ///
    /// This operation cannot fail.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::num::BitInt;
    ///
    /// let v = BitInt::<u32, 4>::new::<7>();
    /// let larger_v = v.extend::<12>();
    /// // The contained values are equal even though `larger_v` has a bigger capacity.
    /// assert_eq!(larger_v, v);
    /// ```
    pub const fn extend<const NEW_NUM_BITS: u32>(self) -> BitInt<T, NEW_NUM_BITS> {
        const {
            build_assert!(
                NEW_NUM_BITS >= NUM_BITS,
                "Requested number of bits is less than the current representation."
            );
        }

        // INVARIANT: the value did fit within `NUM_BITS`, so it will all the more fit within
        // the larger `NEW_NUM_BITS`.
        BitInt::__new(self.0)
    }

    /// Attempts to shrink the number of bits usable for `self`.
    ///
    /// Returns [`None`] if the value of `self` cannot be represented within `NEW_NUM_BITS`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::num::BitInt;
    ///
    /// let v = BitInt::<u32, 12>::new::<7>();
    ///
    /// // `7` can be represented using 3 unsigned bits...
    /// let smaller_v = v.try_shrink::<3>();
    /// assert_eq!(smaller_v.as_deref().copied(), Some(7));
    ///
    /// // ... but doesn't fit within `2` bits.
    /// assert_eq!(v.try_shrink::<2>(), None);
    /// ```
    pub fn try_shrink<const NEW_NUM_BITS: u32>(self) -> Option<BitInt<T, NEW_NUM_BITS>> {
        BitInt::<T, NEW_NUM_BITS>::try_new(self.get())
    }

    /// Casts `self` into a [`BitInt`] backed by a different storage type, but using the same
    /// number of bits for value representation.
    ///
    /// Both `T` and `U` must be of same signedness, and `U` must be at least as large as
    /// `NUM_BITS`, or a build error will occur.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::num::BitInt;
    ///
    /// let v = BitInt::<u32, 12>::new::<127>();
    ///
    /// let u16_v: BitInt<u16, 12> = v.cast();
    /// assert_eq!(u16_v.get(), 127);
    ///
    /// // This won't build: a `u8` is smaller than the required 12 bits.
    /// // let _: BitInt<u8, 12> = v.cast();
    /// ```
    pub fn cast<U>(self) -> BitInt<U, NUM_BITS>
    where
        U: TryFrom<T> + Boundable,
        T: Integer,
        U: Integer<Signedness = T::Signedness>,
    {
        // SAFETY: the converted value is represented using `NUM_BITS`, `U` is larger than
        // `NUM_BITS`, and `U` and `T` have the same sign, hence this conversion cannot fail.
        let value = unsafe { U::try_from(self.get()).unwrap_unchecked() };

        // INVARIANT: although the storage type has changed, the value is still represented within
        // `NUM_BITS`, and with the same signedness.
        BitInt::__new(value)
    }
}

impl<T, const NUM_BITS: u32> core::ops::Deref for BitInt<T, NUM_BITS>
where
    T: Boundable,
{
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // Enforce the invariant to inform the compiler of the bounds of the value.
        if !T::fits_within(self.0, NUM_BITS) {
            // SAFETY: Per the `BitInt` invariants, `fits_within` can never return `false` on the
            // value of a valid instance
            unsafe { core::hint::unreachable_unchecked() }
        }

        &self.0
    }
}

/// Trait similar to [`TryInto`] but for `BitInt`, to avoid conflicting implementations errors.
///
/// # Examples
///
/// ```
/// use kernel::num::{BitInt, TryIntoBitInt};
///
/// // Succeeds because `128` fits into 8 bits.
/// let v: Option<BitInt<u16, 8>> = 128u32.try_into_bitint();
/// assert_eq!(v.as_deref().copied(), Some(128));
///
/// // Fails because `128` doesn't fits into 6 bits.
/// let v: Option<BitInt<u16, 6>> = 128u32.try_into_bitint();
/// assert_eq!(v, None);
/// ```
pub trait TryIntoBitInt<T: Boundable, const NUM_BITS: u32> {
    /// Attempts to convert `self` into a [`BitInt`] using `NUM_BITS`.
    fn try_into_bitint(self) -> Option<BitInt<T, NUM_BITS>>;
}

/// Any value can be attempted to be converted into a [`BitInt`] of any size.
impl<T, U, const NUM_BITS: u32> TryIntoBitInt<T, NUM_BITS> for U
where
    T: Boundable,
    U: TryInto<T>,
{
    fn try_into_bitint(self) -> Option<BitInt<T, NUM_BITS>> {
        self.try_into().ok().and_then(BitInt::try_new)
    }
}

/// Compares between two [`BitInt`]s, even if their number of valid bits differ.
///
/// # Examples
///
/// ```
/// use kernel::num::BitInt;
///
/// let v1 = BitInt::<u32, 8>::new::<15>();
/// let v2 = BitInt::<u32, 4>::new::<15>();
/// assert_eq!(v1, v2);
/// ```
impl<T, U, const NUM_BITS: u32, const NUM_BITS_U: u32> PartialEq<BitInt<U, NUM_BITS_U>>
    for BitInt<T, NUM_BITS>
where
    T: Boundable,
    U: Boundable,
    T: PartialEq<U>,
{
    fn eq(&self, other: &BitInt<U, NUM_BITS_U>) -> bool {
        self.get() == other.get()
    }
}

impl<T, const NUM_BITS: u32> Eq for BitInt<T, NUM_BITS> where T: Boundable {}

/// Does partial ordering between [`BitInt`]s, even if their number of valid bits differ.
///
/// # Examples
///
/// ```
/// use kernel::num::BitInt;
///
/// let v1 = BitInt::<u32, 8>::new::<4>();
/// let v2 = BitInt::<u32, 4>::new::<15>();
/// assert!(v1 < v2);
/// ```
impl<T, U, const NUM_BITS: u32, const NUM_BITS_U: u32> PartialOrd<BitInt<U, NUM_BITS_U>>
    for BitInt<T, NUM_BITS>
where
    T: Boundable,
    U: Boundable,
    T: PartialOrd<U>,
{
    fn partial_cmp(&self, other: &BitInt<U, NUM_BITS_U>) -> Option<core::cmp::Ordering> {
        self.get().partial_cmp(&other.get())
    }
}

/// Does full ordering between [`BitInt`]s.
///
/// # Examples
///
/// ```
/// use core::cmp::Ordering;
/// use kernel::num::BitInt;
///
/// let v1 = BitInt::<u32, 8>::new::<4>();
/// let v2 = BitInt::<u32, 8>::new::<15>();
/// assert_eq!(v1.cmp(&v2), Ordering::Less);
/// ```
impl<T, const NUM_BITS: u32> Ord for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: Ord,
{
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.get().cmp(&other.get())
    }
}

/// Compares between a [`BitInt`] and its backing type.
///
/// # Examples
///
/// ```
/// use kernel::num::BitInt;
///
/// let v = BitInt::<u32, 8>::new::<15>();
/// assert_eq!(v, 15);
/// ```
impl<T, const NUM_BITS: u32> PartialEq<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: PartialEq,
{
    fn eq(&self, other: &T) -> bool {
        self.get() == *other
    }
}

/// Does partial ordering between a [`BitInt`] and its backing type.
///
/// # Examples
///
/// ```
/// use kernel::num::BitInt;
///
/// let v = BitInt::<u32, 8>::new::<4>();
/// assert!(v < 15);
/// ```
impl<T, const NUM_BITS: u32> PartialOrd<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: PartialOrd,
{
    fn partial_cmp(&self, other: &T) -> Option<core::cmp::Ordering> {
        self.get().partial_cmp(other)
    }
}

// Implementations of `core::ops` between a `BitInt` and its backing type.

impl<T, const NUM_BITS: u32> core::ops::Add<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::Add<Output = T>,
{
    type Output = T;

    fn add(self, rhs: T) -> Self::Output {
        self.get() + rhs
    }
}

impl<T, const NUM_BITS: u32> core::ops::BitAnd<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::BitAnd<Output = T>,
{
    type Output = T;

    fn bitand(self, rhs: T) -> Self::Output {
        self.get() & rhs
    }
}

impl<T, const NUM_BITS: u32> core::ops::BitOr<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::BitOr<Output = T>,
{
    type Output = T;

    fn bitor(self, rhs: T) -> Self::Output {
        self.get() | rhs
    }
}

impl<T, const NUM_BITS: u32> core::ops::BitXor<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::BitXor<Output = T>,
{
    type Output = T;

    fn bitxor(self, rhs: T) -> Self::Output {
        self.get() ^ rhs
    }
}

impl<T, const NUM_BITS: u32> core::ops::Div<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::Div<Output = T>,
{
    type Output = T;

    fn div(self, rhs: T) -> Self::Output {
        self.get() / rhs
    }
}

impl<T, const NUM_BITS: u32> core::ops::Mul<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::Mul<Output = T>,
{
    type Output = T;

    fn mul(self, rhs: T) -> Self::Output {
        self.get() * rhs
    }
}

impl<T, const NUM_BITS: u32> core::ops::Neg for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::Neg<Output = T>,
{
    type Output = T;

    fn neg(self) -> Self::Output {
        -self.get()
    }
}

impl<T, const NUM_BITS: u32> core::ops::Not for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::Not<Output = T>,
{
    type Output = T;

    fn not(self) -> Self::Output {
        !self.get()
    }
}

impl<T, const NUM_BITS: u32> core::ops::Rem<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::Rem<Output = T>,
{
    type Output = T;

    fn rem(self, rhs: T) -> Self::Output {
        self.get() % rhs
    }
}

impl<T, const NUM_BITS: u32> core::ops::Sub<T> for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::ops::Sub<Output = T>,
{
    type Output = T;

    fn sub(self, rhs: T) -> Self::Output {
        self.get() - rhs
    }
}

// Proxy implementations of `core::fmt`.

impl<T, const NUM_BITS: u32> core::fmt::Display for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::fmt::Display,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

impl<T, const NUM_BITS: u32> core::fmt::Binary for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::fmt::Binary,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

impl<T, const NUM_BITS: u32> core::fmt::LowerExp for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::fmt::LowerExp,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

impl<T, const NUM_BITS: u32> core::fmt::LowerHex for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::fmt::LowerHex,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

impl<T, const NUM_BITS: u32> core::fmt::Octal for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::fmt::Octal,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

impl<T, const NUM_BITS: u32> core::fmt::UpperExp for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::fmt::UpperExp,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

impl<T, const NUM_BITS: u32> core::fmt::UpperHex for BitInt<T, NUM_BITS>
where
    T: Boundable,
    T: core::fmt::UpperHex,
{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.get().fmt(f)
    }
}

/// Implements `$trait` for all [`BitInt`] types represented using `$num_bits`.
///
/// This is used to declare size properties as traits that we can constrain against in impl blocks.
macro_rules! impl_size_rule {
    ($trait:ty, $($num_bits:literal)*) => {
        $(
        impl<T> $trait for BitInt<T, $num_bits> where T: Boundable {}
        )*
    };
}

/// Local trait expressing the fact that a given [`BitInt`] has at least `N` bits used for value
/// representation.
trait AtLeastXBits<const N: usize> {}

/// Implementations for infallibly converting a primitive type into a [`BitInt`] that can contain
/// it.
///
/// Put into their own module for readability, and to avoid cluttering the rustdoc of the parent
/// module.
mod atleast_impls {
    use super::*;

    // Number of bits at least as large as 64.
    impl_size_rule!(AtLeastXBits<64>, 64);

    // Anything 64 bits or more is also larger than 32.
    impl<T> AtLeastXBits<32> for T where T: AtLeastXBits<64> {}
    // Other numbers of bits at least as large as 32.
    impl_size_rule!(AtLeastXBits<32>,
        32 33 34 35 36 37 38 39
        40 41 42 43 44 45 46 47
        48 49 50 51 52 53 54 55
        56 57 58 59 60 61 62 63
    );

    // Anything 32 bits or more is also larger than 16.
    impl<T> AtLeastXBits<16> for T where T: AtLeastXBits<32> {}
    // Other numbers of bits at least as large as 16.
    impl_size_rule!(AtLeastXBits<16>,
        16 17 18 19 20 21 22 23
        24 25 26 27 28 29 30 31
    );

    // Anything 16 bits or more is also larger than 8.
    impl<T> AtLeastXBits<8> for T where T: AtLeastXBits<16> {}
    // Other numbers of bits at least as large as 8.
    impl_size_rule!(AtLeastXBits<8>, 8 9 10 11 12 13 14 15);

    // Anything 8 bits or more is also larger than 1.
    impl<T> AtLeastXBits<1> for T where T: AtLeastXBits<8> {}
    // Other numbers of bits at least as large as 1.
    impl_size_rule!(AtLeastXBits<1>, 1 2 3 4 5 6 7);
}

/// Generates `From` implementations from a primitive type into a [`BitInt`] with
/// enough bits to store any value of that type.
///
/// Note: The only reason for having this macro is that if we pass `$type` as a generic
/// parameter, we cannot use it in the const context of [`AtLeastXBits`]'s generic parameter. This
/// can be fixed once the `generic_const_exprs` feature is usable, and this macro replaced by a
/// regular `impl` block.
macro_rules! impl_from_primitive {
    ($($type:ty),*) => {
        $(
        #[doc = ::core::concat!(
            "Conversion from a [`",
            ::core::stringify!($type),
            "`] into a [`BitInt`] of same signedness with enough bits to store it.")]
        impl<T, const NUM_BITS: u32> From<$type> for BitInt<T, NUM_BITS>
        where
            $type: Integer,
            T: From<$type> + Boundable + Integer<Signedness = <$type as Integer>::Signedness>,
            Self: AtLeastXBits<{ <$type as Integer>::BITS as usize }>,
        {
            fn from(value: $type) -> Self {
                // INVARIANT: The trait bound on `Self` guarantees that `NUM_BITS` is large
                // enough to hold any value of the source type.
                Self::__new(T::from(value))
            }
        }
        )*
    }
}

impl_from_primitive!(bool, u8, i8, u16, i16, u32, i32, u64, i64);

/// Local trait expressing the fact that a given [`BitInt`] fits into a primitive type of `N` bits,
/// provided they have the same signedness.
trait FitsInXBits<const N: usize> {}

/// Implementations for infallibly converting a [`BitInt`] into a primitive type that can contain
/// it.
///
/// Put into their own module for readability, and to avoid cluttering the rustdoc of the parent
/// module.
mod fits_impls {
    use super::*;

    // Number of bits that fit into a primitive with 1 bit.
    impl_size_rule!(FitsInXBits<1>, 1);

    // Anything that fits into 1 bit also fits into 8.
    impl<T> FitsInXBits<8> for T where T: FitsInXBits<1> {}
    // Other numbers of bits that fit into a 8-bits primitive.
    impl_size_rule!(FitsInXBits<8>, 2 3 4 5 6 7 8);

    // Anything that fits into 8 bits also fits into 16.
    impl<T> FitsInXBits<16> for T where T: FitsInXBits<8> {}
    // Other numbers of bits that fit into a 16-bits primitive.
    impl_size_rule!(FitsInXBits<16>, 9 10 11 12 13 14 15 16);

    // Anything that fits into 16 bits also fits into 32.
    impl<T> FitsInXBits<32> for T where T: FitsInXBits<16> {}
    // Other numbers of bits that fit into a 32-bits primitive.
    impl_size_rule!(FitsInXBits<32>,
        17 18 19 20 21 22 23 24
        25 26 27 28 29 30 31 32
    );

    // Anything that fits into 32 bits also fits into 64.
    impl<T> FitsInXBits<64> for T where T: FitsInXBits<32> {}
    // Other numbers of bits that fit into a 64-bits primitive.
    impl_size_rule!(FitsInXBits<64>,
        33 34 35 36 37 38 39 40
        41 42 43 44 45 46 47 48
        49 50 51 52 53 54 55 56
        57 58 59 60 61 62 63 64
    );
}

/// Generates [`From`] implementations from a [`BitInt`] into a primitive type that is
/// guaranteed to contain it.
///
/// Note: The only reason for having this macro is that if we pass `$type` as a generic
/// parameter, we cannot use it in the const context of `AtLeastXBits`'s generic parameter. This
/// can be fixed once the `generic_const_exprs` feature is usable, and this macro replaced by a
/// regular `impl` block.
macro_rules! impl_into_primitive {
    ($($type:ty),*) => {
        $(
        #[doc = ::core::concat!(
            "Conversion from a [`BitInt`] with no more bits than a [`",
            ::core::stringify!($type),
            "`] and of same signedness into [`",
            ::core::stringify!($type),
            "`]")]
        impl<T, const NUM_BITS: u32> From<BitInt<T, NUM_BITS>> for $type
        where
            $type: Integer,
            T: Boundable + Integer<Signedness = <$type as Integer>::Signedness>,
            BitInt<T, NUM_BITS>: FitsInXBits<{ <$type as Integer>::BITS as usize }>,
        {
            fn from(value: BitInt<T, NUM_BITS>) -> $type {
                // SAFETY: The trait bound on `BitInt` ensures that any value it holds (which
                // is constrained to `NUM_BITS`) can fit into the destination type, so this
                // conversion cannot fail.
                unsafe { value.get().try_into().unwrap_unchecked() }
            }
        }
        )*
    }
}

impl_into_primitive!(u8, i8, u16, i16, u32, i32, u64, i64);

/// Conversion to boolean is handled separately as it does not have a [`TryFrom`] implementation
/// from integers.
impl<T> From<BitInt<T, 1>> for bool
where
    T: Boundable,
    BitInt<T, 1>: FitsInXBits<1>,
    T: PartialEq + Zeroable,
{
    fn from(value: BitInt<T, 1>) -> Self {
        value.get() != Zeroable::zeroed()
    }
}
