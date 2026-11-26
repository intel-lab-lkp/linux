// SPDX-License-Identifier: GPL-2.0

//! Time related primitives.
//!
//! This module contains the kernel APIs related to time and timers that
//! have been ported or wrapped for usage by Rust code in the kernel.
//!
//! There are two types in this module:
//!
//! - The [`Instant`] type represents a specific point in time.
//! - The [`Delta`] type represents a span of time.
//!
//! Note that the C side uses `ktime_t` type to represent both. However, timestamp
//! and timedelta are different. To avoid confusion, we use two different types.
//!
//! A [`Instant`] object can be created by calling the [`Instant::now()`] function.
//! It represents a point in time at which the object was created.
//! By calling the [`Instant::elapsed()`] method, a [`Delta`] object representing
//! the elapsed time can be created. The [`Delta`] object can also be created
//! by subtracting two [`Instant`] objects.
//!
//! A [`Delta`] type supports methods to retrieve the duration in various units.
//!
//! C header: [`include/linux/jiffies.h`](srctree/include/linux/jiffies.h).
//! C header: [`include/linux/ktime.h`](srctree/include/linux/ktime.h).

use core::marker::PhantomData;
use core::ops;

pub mod delay;
pub mod hrtimer;

/// The number of nanoseconds per microsecond.
pub const NSEC_PER_USEC: i64 = bindings::NSEC_PER_USEC as i64;

/// The number of nanoseconds per millisecond.
pub const NSEC_PER_MSEC: i64 = bindings::NSEC_PER_MSEC as i64;

/// The number of nanoseconds per second.
pub const NSEC_PER_SEC: i64 = bindings::NSEC_PER_SEC as i64;

/// The C side `MAX_JIFFY_OFFSET`, i.e. `((LONG_MAX >> 1) - 1)`. It is the upper
/// bound the kernel uses for a jiffies span, not a wait-forever value.
const MAX_JIFFY_OFFSET: isize = (isize::MAX >> 1) - 1;

/// The time unit of Linux kernel. One jiffy equals (1/HZ) second.
pub type Jiffies = crate::ffi::c_ulong;

/// The millisecond time unit.
pub type Msecs = crate::ffi::c_uint;

/// Converts milliseconds to jiffies.
#[inline]
pub fn msecs_to_jiffies(msecs: Msecs) -> Jiffies {
    // SAFETY: The `__msecs_to_jiffies` function is always safe to call no
    // matter what the argument is.
    unsafe { bindings::__msecs_to_jiffies(msecs) }
}

/// Trait for kernel clock identifiers.
///
/// Selection of the clock depends on the use case. In some cases the usage of a
/// particular clock is mandatory, e.g. in network protocols, filesystems. In other
/// cases the user of the clock has to decide which clock is best suited for the
/// purpose. In most scenarios clock [`Monotonic`] is the best choice as it
/// provides a accurate monotonic notion of time (leap second smearing ignored).
///
/// # Safety
///
/// Implementers must ensure that `ktime_get()` returns a value in the inclusive range
/// `0..=KTIME_MAX` (i.e., greater than or equal to 0 and less than or equal to
/// `KTIME_MAX`, where `KTIME_MAX` equals `i64::MAX`).
pub unsafe trait ClockId {
    /// The kernel clock ID associated with this clock.
    ///
    /// This constant corresponds to the C side `clockid_t` value.
    const ID: bindings::clockid_t;

    /// Get the current time from the clock.
    ///
    /// The function must return a value in the range `0..=KTIME_MAX`.
    fn ktime_get() -> bindings::ktime_t;
}

/// A monotonically increasing clock.
///
/// A nonsettable system-wide clock that represents monotonic time since as
/// described by POSIX, "some unspecified point in the past". On Linux, that
/// point corresponds to the number of seconds that the system has been
/// running since it was booted.
///
/// The CLOCK_MONOTONIC clock is not affected by discontinuous jumps in the
/// CLOCK_REAL (e.g., if the system administrator manually changes the
/// clock), but is affected by frequency adjustments. This clock does not
/// count time that the system is suspended.
pub struct Monotonic;

// SAFETY: The kernel's `ktime_get()` is guaranteed to return a value
// in `0..=KTIME_MAX`.
unsafe impl ClockId for Monotonic {
    const ID: bindings::clockid_t = bindings::CLOCK_MONOTONIC as bindings::clockid_t;

    fn ktime_get() -> bindings::ktime_t {
        // SAFETY: It is always safe to call `ktime_get()` outside of NMI context.
        unsafe { bindings::ktime_get() }
    }
}

/// A settable system-wide clock that measures real (i.e., wall-clock) time.
///
/// Setting this clock requires appropriate privileges. This clock is
/// affected by discontinuous jumps in the system time (e.g., if the system
/// administrator manually changes the clock), and by frequency adjustments
/// performed by NTP and similar applications via adjtime(3), adjtimex(2),
/// clock_adjtime(2), and ntp_adjtime(3). This clock normally counts the
/// number of seconds since 1970-01-01 00:00:00 Coordinated Universal Time
/// (UTC) except that it ignores leap seconds; near a leap second it may be
/// adjusted by leap second smearing to stay roughly in sync with UTC. Leap
/// second smearing applies frequency adjustments to the clock to speed up
/// or slow down the clock to account for the leap second without
/// discontinuities in the clock. If leap second smearing is not applied,
/// the clock will experience discontinuity around leap second adjustment.
pub struct RealTime;

// SAFETY: The kernel's `ktime_get_real()` is guaranteed to return a value
// in `0..=KTIME_MAX`.
unsafe impl ClockId for RealTime {
    const ID: bindings::clockid_t = bindings::CLOCK_REALTIME as bindings::clockid_t;

    fn ktime_get() -> bindings::ktime_t {
        // SAFETY: It is always safe to call `ktime_get_real()` outside of NMI context.
        unsafe { bindings::ktime_get_real() }
    }
}

/// A monotonic that ticks while system is suspended.
///
/// A nonsettable system-wide clock that is identical to CLOCK_MONOTONIC,
/// except that it also includes any time that the system is suspended. This
/// allows applications to get a suspend-aware monotonic clock without
/// having to deal with the complications of CLOCK_REALTIME, which may have
/// discontinuities if the time is changed using settimeofday(2) or similar.
pub struct BootTime;

// SAFETY: The kernel's `ktime_get_boottime()` is guaranteed to return a value
// in `0..=KTIME_MAX`.
unsafe impl ClockId for BootTime {
    const ID: bindings::clockid_t = bindings::CLOCK_BOOTTIME as bindings::clockid_t;

    fn ktime_get() -> bindings::ktime_t {
        // SAFETY: It is always safe to call `ktime_get_boottime()` outside of NMI context.
        unsafe { bindings::ktime_get_boottime() }
    }
}

/// International Atomic Time.
///
/// A system-wide clock derived from wall-clock time but counting leap seconds.
///
/// This clock is coupled to CLOCK_REALTIME and will be set when CLOCK_REALTIME is
/// set, or when the offset to CLOCK_REALTIME is changed via adjtimex(2). This
/// usually happens during boot and **should** not happen during normal operations.
/// However, if NTP or another application adjusts CLOCK_REALTIME by leap second
/// smearing, this clock will not be precise during leap second smearing.
///
/// The acronym TAI refers to International Atomic Time.
pub struct Tai;

// SAFETY: The kernel's `ktime_get_clocktai()` is guaranteed to return a value
// in `0..=KTIME_MAX`.
unsafe impl ClockId for Tai {
    const ID: bindings::clockid_t = bindings::CLOCK_TAI as bindings::clockid_t;

    fn ktime_get() -> bindings::ktime_t {
        // SAFETY: It is always safe to call `ktime_get_tai()` outside of NMI context.
        unsafe { bindings::ktime_get_clocktai() }
    }
}

/// A specific point in time.
///
/// # Invariants
///
/// The `inner` value is in the range from 0 to `KTIME_MAX`.
#[repr(transparent)]
#[derive(PartialEq, PartialOrd, Eq, Ord)]
pub struct Instant<C: ClockId> {
    inner: bindings::ktime_t,
    _c: PhantomData<C>,
}

impl<C: ClockId> Clone for Instant<C> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<C: ClockId> Copy for Instant<C> {}

impl<C: ClockId> Instant<C> {
    /// Get the current time from the clock source.
    #[inline]
    pub fn now() -> Self {
        // INVARIANT: The `ClockId::ktime_get()` function returns a value in the range
        // from 0 to `KTIME_MAX`.
        Self {
            inner: C::ktime_get(),
            _c: PhantomData,
        }
    }

    /// Return the amount of time elapsed since the [`Instant`].
    #[inline]
    pub fn elapsed(&self) -> Delta {
        Self::now() - *self
    }

    #[inline]
    pub(crate) fn as_nanos(&self) -> i64 {
        self.inner
    }

    /// Create an [`Instant`] from a `ktime_t` without checking if it is non-negative.
    ///
    /// # Panics
    ///
    /// On debug builds, this function will panic if `ktime` is not in the range from 0 to
    /// `KTIME_MAX`.
    ///
    /// # Safety
    ///
    /// The caller promises that `ktime` is in the range from 0 to `KTIME_MAX`.
    #[inline]
    pub(crate) unsafe fn from_ktime(ktime: bindings::ktime_t) -> Self {
        debug_assert!(ktime >= 0);

        // INVARIANT: Our safety contract ensures that `ktime` is in the range from 0 to
        // `KTIME_MAX`.
        Self {
            inner: ktime,
            _c: PhantomData,
        }
    }
}

impl<C: ClockId> ops::Sub for Instant<C> {
    type Output = Delta;

    // By the type invariant, it never overflows.
    #[inline]
    fn sub(self, other: Instant<C>) -> Delta {
        Delta {
            value: self.inner - other.inner,
        }
    }
}

impl<T: ClockId> ops::Add<Delta> for Instant<T> {
    type Output = Self;

    #[inline]
    fn add(self, rhs: Delta) -> Self::Output {
        // INVARIANT: With arithmetic over/underflow checks enabled, this will panic if we overflow
        // (e.g. go above `KTIME_MAX`)
        let res = self.inner + rhs.value;

        // INVARIANT: With overflow checks enabled, we verify here that the value is >= 0
        #[cfg(CONFIG_RUST_OVERFLOW_CHECKS)]
        assert!(res >= 0);

        Self {
            inner: res,
            _c: PhantomData,
        }
    }
}

impl<T: ClockId> ops::Sub<Delta> for Instant<T> {
    type Output = Self;

    #[inline]
    fn sub(self, rhs: Delta) -> Self::Output {
        // INVARIANT: With arithmetic over/underflow checks enabled, this will panic if we overflow
        // (e.g. go above `KTIME_MAX`)
        let res = self.inner - rhs.value;

        // INVARIANT: With overflow checks enabled, we verify here that the value is >= 0
        #[cfg(CONFIG_RUST_OVERFLOW_CHECKS)]
        assert!(res >= 0);

        Self {
            inner: res,
            _c: PhantomData,
        }
    }
}

mod private {
    pub trait Sealed {}

    impl Sealed for super::Nsec {}
    impl Sealed for super::Jiffy {}
}

/// A trait for time units.
pub trait TimeUnit: private::Sealed {
    /// The underlying representation of the time unit.
    type Repr: Copy + Clone + PartialEq + PartialOrd + Eq + Ord + core::fmt::Debug;

    /// The zero value of [`Self::Repr`].
    const ZERO: Self::Repr;

    /// The suffix that denotes this unit when a [`Delta`] is formatted.
    const SUFFIX: &'static str;

    /// Widens `repr` to an [`i64`].
    fn repr_to_i64(repr: Self::Repr) -> i64;
}

/// A time unit of nanoseconds.
///
/// A [`Delta<Nsec>`] stores its value as [`i64`] nanoseconds and can represent
/// any [`i64`] value, including negative, zero, and positive numbers.
#[derive(Copy, Clone, PartialEq, PartialOrd, Eq, Ord, Debug)]
pub enum Nsec {}

impl TimeUnit for Nsec {
    type Repr = i64;
    const ZERO: Self::Repr = 0;
    const SUFFIX: &'static str = "ns";

    #[inline]
    fn repr_to_i64(repr: Self::Repr) -> i64 {
        repr
    }
}

/// A time unit of jiffies.
///
/// A [`Delta<Jiffy>`] stores its value as [`isize`] jiffies and can represent
/// any [`isize`] value, including negative, zero, and positive numbers.
#[derive(Copy, Clone, PartialEq, PartialOrd, Eq, Ord, Debug)]
pub enum Jiffy {}

impl TimeUnit for Jiffy {
    type Repr = isize;
    const ZERO: Self::Repr = 0;
    const SUFFIX: &'static str = "jiffies";

    #[inline]
    fn repr_to_i64(repr: Self::Repr) -> i64 {
        // CAST: `isize` is 32 or 64 bits wide on the architectures the kernel
        // supports, so widening to `i64` preserves the value.
        repr as i64
    }
}

/// A span of time.
///
/// The span is stored in the unit given by the type parameter `U` (see
/// [`TimeUnit`]); its value has type `U::Repr`. `U` defaults to [`Nsec`], so a
/// plain [`Delta`] is a span in nanoseconds. The value can be negative, zero, or
/// positive.
#[derive(Copy, Clone, PartialEq, PartialOrd, Eq, Ord)]
pub struct Delta<U: TimeUnit = Nsec> {
    value: U::Repr,
}

impl<U: TimeUnit> Delta<U> {
    /// A span of time equal to zero.
    pub const ZERO: Self = Self { value: U::ZERO };
}

impl Delta<Jiffy> {
    /// Create a new [`Delta`] from a number of jiffies.
    #[inline]
    pub const fn from_jiffies(jiffies: isize) -> Self {
        Self { value: jiffies }
    }

    /// Return the number of jiffies in the [`Delta`].
    #[inline]
    pub const fn as_jiffies(self) -> isize {
        self.value
    }

    /// Return the number of jiffies in the [`Delta`] as an unsigned value.
    ///
    /// If the number of jiffies is negative, zero is returned.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::time::Delta;
    ///
    /// // A negative span returns zero.
    /// assert_eq!(Delta::from_jiffies(-1).as_jiffies_unsigned(), 0);
    ///
    /// // A positive span returns its value.
    /// assert_eq!(Delta::from_jiffies(1).as_jiffies_unsigned(), 1);
    /// ```
    #[inline]
    pub fn as_jiffies_unsigned(self) -> usize {
        // CAST: `isize::max()` makes the value non-negative, so the cast keeps it.
        isize::max(self.value, 0) as usize
    }
}

impl ops::Add for Delta {
    type Output = Self;

    #[inline]
    fn add(self, rhs: Self) -> Self {
        Self {
            value: self.value + rhs.value,
        }
    }
}

impl ops::AddAssign for Delta {
    #[inline]
    fn add_assign(&mut self, rhs: Self) {
        self.value += rhs.value;
    }
}

impl ops::Sub for Delta {
    type Output = Self;

    #[inline]
    fn sub(self, rhs: Self) -> Self::Output {
        Self {
            value: self.value - rhs.value,
        }
    }
}

impl ops::SubAssign for Delta {
    #[inline]
    fn sub_assign(&mut self, rhs: Self) {
        self.value -= rhs.value;
    }
}

impl ops::Mul<i64> for Delta {
    type Output = Self;

    #[inline]
    fn mul(self, rhs: i64) -> Self::Output {
        Self {
            value: self.value * rhs,
        }
    }
}

impl ops::MulAssign<i64> for Delta {
    #[inline]
    fn mul_assign(&mut self, rhs: i64) {
        self.value *= rhs;
    }
}

impl ops::Div for Delta {
    type Output = i64;

    #[inline]
    fn div(self, rhs: Self) -> Self::Output {
        #[cfg(CONFIG_64BIT)]
        {
            self.value / rhs.value
        }

        #[cfg(not(CONFIG_64BIT))]
        {
            // SAFETY: This function is always safe to call regardless of the input values
            unsafe { bindings::div64_s64(self.value, rhs.value) }
        }
    }
}

impl Delta {
    /// Create a new [`Delta`] from a number of nanoseconds.
    #[inline]
    pub const fn from_nanos(nanos: i64) -> Self {
        Self { value: nanos }
    }

    /// Create a new [`Delta`] from a number of microseconds.
    ///
    /// The `micros` can range from -9_223_372_036_854_775 to 9_223_372_036_854_775.
    /// If `micros` is outside this range, `i64::MIN` is used for negative values,
    /// and `i64::MAX` is used for positive values due to saturation.
    #[inline]
    pub const fn from_micros(micros: i64) -> Self {
        Self {
            value: micros.saturating_mul(NSEC_PER_USEC),
        }
    }

    /// Create a new [`Delta`] from a number of milliseconds.
    ///
    /// The `millis` can range from -9_223_372_036_854 to 9_223_372_036_854.
    /// If `millis` is outside this range, `i64::MIN` is used for negative values,
    /// and `i64::MAX` is used for positive values due to saturation.
    #[inline]
    pub const fn from_millis(millis: i64) -> Self {
        Self {
            value: millis.saturating_mul(NSEC_PER_MSEC),
        }
    }

    /// Create a new [`Delta`] from a number of seconds.
    ///
    /// The `secs` can range from -9_223_372_036 to 9_223_372_036.
    /// If `secs` is outside this range, `i64::MIN` is used for negative values,
    /// and `i64::MAX` is used for positive values due to saturation.
    #[inline]
    pub const fn from_secs(secs: i64) -> Self {
        Self {
            value: secs.saturating_mul(NSEC_PER_SEC),
        }
    }

    /// Return `true` if the [`Delta`] spans no time.
    #[inline]
    pub fn is_zero(self) -> bool {
        self.as_nanos() == 0
    }

    /// Return `true` if the [`Delta`] spans a negative amount of time.
    #[inline]
    pub fn is_negative(self) -> bool {
        self.as_nanos() < 0
    }

    /// Return the number of nanoseconds in the [`Delta`].
    #[inline]
    pub const fn as_nanos(self) -> i64 {
        self.value
    }

    /// Return the smallest number of microseconds greater than or equal
    /// to the value in the [`Delta`].
    #[inline]
    pub fn as_micros_ceil(self) -> i64 {
        // Only positive values need to be rounded up: truncating division already
        // rounds towards zero, i.e. up, for negative values.
        //
        // The usual `(nanos + d - 1) / d` is not used because the addition overflows
        // once `nanos` exceeds `i64::MAX - (d - 1)`; saturating the addition instead
        // would drop the rounding bias and return a result one unit too small.
        let n = self.as_nanos();

        let (n, add) = if n > 0 { (n - 1, 1) } else { (n, 0) };

        #[cfg(CONFIG_64BIT)]
        {
            n / NSEC_PER_USEC + add
        }

        #[cfg(not(CONFIG_64BIT))]
        // SAFETY: It is always safe to call `ktime_to_us()` with any value.
        unsafe {
            bindings::ktime_to_us(n) + add
        }
    }

    /// Return the number of milliseconds in the [`Delta`].
    #[inline]
    pub fn as_millis(self) -> i64 {
        #[cfg(CONFIG_64BIT)]
        {
            self.as_nanos() / NSEC_PER_MSEC
        }

        #[cfg(not(CONFIG_64BIT))]
        // SAFETY: It is always safe to call `ktime_to_ms()` with any value.
        unsafe {
            bindings::ktime_to_ms(self.as_nanos())
        }
    }

    /// Return the smallest number of milliseconds greater than or equal
    /// to the value in the [`Delta`].
    #[inline]
    pub fn as_millis_ceil(self) -> i64 {
        // Only positive values need to be rounded up: truncating division already
        // rounds towards zero, i.e. up, for negative values.
        //
        // The usual `(nanos + d - 1) / d` is not used because the addition overflows
        // once `nanos` exceeds `i64::MAX - (d - 1)`; saturating the addition instead
        // would drop the rounding bias and return a result one unit too small.
        let n = self.as_nanos();

        let (n, add) = if n > 0 { (n - 1, 1) } else { (n, 0) };

        #[cfg(CONFIG_64BIT)]
        {
            n / NSEC_PER_MSEC + add
        }

        #[cfg(not(CONFIG_64BIT))]
        // SAFETY: It is always safe to call `ktime_to_ms()` with any value.
        unsafe {
            bindings::ktime_to_ms(n) + add
        }
    }

    /// Convert this span to a [`Delta<Jiffy>`] suitable for use as a timeout.
    ///
    /// Unless the result saturates, the value is rounded up to the next whole
    /// jiffy, so the resulting timeout is never shorter than `self`.
    ///
    /// A negative span saturates at zero jiffies, i.e. an immediate timeout.
    ///
    /// A span that does not fit saturates at the kernel's [`MAX_JIFFY_OFFSET`],
    /// the upper bound for a jiffies span. That is a finite timeout, so a
    /// saturated result can be shorter than the requested span. It is derived
    /// from `long`, so only 32 bit can reach it, at about 12 days with `HZ=1000`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::time::Delta;
    ///
    /// // A negative span is an immediate timeout.
    /// assert_eq!(Delta::from_millis(-1).to_jiffies_timeout().as_jiffies(), 0);
    ///
    /// // A span shorter than a jiffy still waits, i.e. the timeout is never
    /// // shorter than the span.
    /// assert!(Delta::from_nanos(1).to_jiffies_timeout().as_jiffies() >= 1);
    /// ```
    ///
    /// [`MAX_JIFFY_OFFSET`]: srctree/include/linux/jiffies.h
    #[inline]
    pub fn to_jiffies_timeout(self) -> Delta<Jiffy> {
        const HZ: u64 = bindings::HZ as u64;

        // The quotient `(nsecs * HZ + NSEC_PER_SEC - 1) / NSEC_PER_SEC` has to fit in
        // `u64`; `nsecs * HZ` does not. With `HZ <= NSEC_PER_SEC` the numerator is at
        // most `(nsecs + 1) * NSEC_PER_SEC - 1`, so the quotient is at most `nsecs`.
        crate::static_assert!(HZ <= NSEC_PER_SEC as u64);

        // CAST: `i64::max()` makes the value non-negative, so the cast keeps it.
        let nsecs = i64::max(self.as_nanos(), 0) as u64;

        // SAFETY: `mul_u64_add_u64_div_u64()` must not be called with a zero divisor,
        // and its result must fit in `u64`. `NSEC_PER_SEC` is a non-zero constant, and
        // the assertion above bounds the quotient by `nsecs`.
        let jiffies = unsafe {
            bindings::mul_u64_add_u64_div_u64(
                nsecs,
                HZ,
                (NSEC_PER_SEC - 1) as u64,
                NSEC_PER_SEC as u64,
            )
        };

        // CAST: `jiffies` is clamped to `MAX_JIFFY_OFFSET`, which is `<= isize::MAX`.
        let jiffies = u64::min(jiffies, MAX_JIFFY_OFFSET as u64) as isize;

        Delta::<Jiffy>::from_jiffies(jiffies)
    }

    /// Return `self % dividend` where `dividend` is in nanoseconds.
    ///
    /// The kernel doesn't have any emulation for `s64 % s64` on 32 bit platforms, so this is
    /// limited to 32 bit dividends.
    #[inline]
    pub fn rem_nanos(self, dividend: i32) -> Self {
        #[cfg(CONFIG_64BIT)]
        {
            Self {
                value: self.as_nanos() % i64::from(dividend),
            }
        }

        #[cfg(not(CONFIG_64BIT))]
        {
            let mut rem = 0;

            // SAFETY: `rem` is in the stack, so we can always provide a valid pointer to it.
            unsafe { bindings::div_s64_rem(self.as_nanos(), dividend, &mut rem) };

            Self {
                value: i64::from(rem),
            }
        }
    }
}

impl<U: TimeUnit> kernel::fmt::Debug for Delta<U> {
    /// Formats the span as a number of time units, followed by the unit.
    ///
    /// The value and the unit are separated by a space, and each group of three
    /// digits is separated by an underscore, the way Rust integer literals are
    /// written.
    ///
    /// ```
    /// # use kernel::{prelude::fmt, str::CString, time::Delta};
    /// let delta = Delta::from_micros(5);
    /// let formatted = CString::try_from_fmt(fmt!("{delta:?}"))?;
    /// assert_eq!(formatted.to_bytes(), b"5_000 ns");
    ///
    /// let negative = Delta::from_millis(-2);
    /// let formatted = CString::try_from_fmt(fmt!("{negative:?}"))?;
    /// assert_eq!(formatted.to_bytes(), b"-2_000_000 ns");
    ///
    /// let jiffies = Delta::from_jiffies(1234);
    /// let formatted = CString::try_from_fmt(fmt!("{jiffies:?}"))?;
    /// assert_eq!(formatted.to_bytes(), b"1_234 jiffies");
    /// # Ok::<(), kernel::error::Error>(())
    /// ```
    fn fmt(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let value = U::repr_to_i64(self.value);

        if value < 0 {
            f.write_str("-")?;
        }

        // `unsigned_abs` is used because the magnitude of `i64::MIN` is not
        // representable as an `i64`.
        let mut magnitude = value.unsigned_abs();

        // Split the magnitude into groups of three digits, least significant
        // group first. A `u64` holds at most 20 digits, so 7 groups suffice.
        //
        // Dividing by the constant keeps the compiler from emitting a check for
        // a division by zero with a potential panic.
        let mut groups = [0u64; 7];
        let mut used = 0;

        for group in groups.iter_mut() {
            *group = magnitude % 1000;
            magnitude /= 1000;
            used += 1;

            if magnitude == 0 {
                break;
            }
        }

        let mut groups = groups.into_iter().take(used).rev();

        // The leading group holds the most significant digits of the value, so
        // it is the only one that is not zero padded.
        if let Some(leading) = groups.next() {
            write!(f, "{}", leading)?;
        }

        for group in groups {
            write!(f, "_{:03}", group)?;
        }

        write!(f, " {}", U::SUFFIX)
    }
}

#[cfg(CONFIG_RUST_TIME_KUNIT_TEST)]
#[macros::kunit_tests(rust_kernel_time)]
mod tests {
    use super::*;
    use crate::{prelude::fmt, str::CString};

    /// Formats `delta` with its [`kernel::fmt::Debug`] implementation.
    fn debug_fmt<U: TimeUnit>(delta: Delta<U>) -> Result<CString, crate::error::Error> {
        CString::try_from_fmt(fmt!("{delta:?}"))
    }

    #[test]
    fn delta_debug_separates_digit_groups() -> Result<(), crate::error::Error> {
        // A value below one thousand is printed without a separator.
        assert_eq!(debug_fmt(Delta::from_nanos(999))?.to_bytes(), b"999 ns");

        // Groups that do not lead the value are zero padded.
        assert_eq!(debug_fmt(Delta::from_nanos(1000))?.to_bytes(), b"1_000 ns");
        assert_eq!(
            debug_fmt(Delta::from_nanos(1_000_007))?.to_bytes(),
            b"1_000_007 ns"
        );

        // Zero has a single group.
        assert_eq!(debug_fmt(Delta::<Nsec>::ZERO)?.to_bytes(), b"0 ns");

        // The extremes of the representation are printed in full. The magnitude
        // of `i64::MIN` does not fit an `i64`, which makes it the interesting
        // one.
        assert_eq!(
            debug_fmt(Delta::from_nanos(i64::MAX))?.to_bytes(),
            b"9_223_372_036_854_775_807 ns"
        );
        assert_eq!(
            debug_fmt(Delta::from_nanos(i64::MIN))?.to_bytes(),
            b"-9_223_372_036_854_775_808 ns"
        );

        Ok(())
    }

    #[test]
    fn delta_debug_uses_the_unit_suffix() -> Result<(), crate::error::Error> {
        assert_eq!(debug_fmt(Delta::from_nanos(1))?.to_bytes(), b"1 ns");
        assert_eq!(debug_fmt(Delta::from_jiffies(1))?.to_bytes(), b"1 jiffies");
        assert_eq!(debug_fmt(Delta::<Jiffy>::ZERO)?.to_bytes(), b"0 jiffies");

        Ok(())
    }

    #[test]
    fn to_jiffies_timeout_converts() {
        const HZ: isize = bindings::HZ as isize;

        // One second is exactly `HZ` jiffies, and the round-up must not add one.
        assert_eq!(Delta::from_secs(1).to_jiffies_timeout().as_jiffies(), HZ);

        // One nanosecond more has to round up to the next whole jiffy.
        assert_eq!(
            Delta::from_nanos(NSEC_PER_SEC + 1)
                .to_jiffies_timeout()
                .as_jiffies(),
            HZ + 1
        );
    }

    #[test]
    fn to_jiffies_timeout_saturates() {
        // The result never exceeds `MAX_JIFFY_OFFSET`. On 32 bit with `HZ=1000` this
        // span is 2147483647 jiffies, so the clamp is what keeps it in range; on 64
        // bit it fits and the check holds for every possible return value.
        let clamped = Delta::from_millis(i64::from(i32::MAX)).to_jiffies_timeout();
        assert!(clamped.as_jiffies() <= MAX_JIFFY_OFFSET);

        // `MAX_JIFFY_OFFSET` is derived from `long`, so only 32 bit can reach it. On
        // 64 bit `i64::MAX` nanoseconds is about 292 years, which is 9223372036855
        // jiffies with `HZ=1000`, far below the limit.
        #[cfg(not(CONFIG_64BIT))]
        {
            // An overlong span is clamped to `MAX_JIFFY_OFFSET`.
            let overlong = Delta::from_nanos(i64::MAX).to_jiffies_timeout();
            assert_eq!(overlong.as_jiffies(), MAX_JIFFY_OFFSET);
        }

        // A negative span is an immediate timeout, however long it is.
        let negative = Delta::from_nanos(i64::MIN).to_jiffies_timeout();
        assert_eq!(negative.as_jiffies(), 0);
    }
}
