// SPDX-License-Identifier: GPL-2.0

//! Time related primitives.
//!
//! This module contains the kernel APIs related to time that
//! have been ported or wrapped for usage by Rust code in the kernel.
//!
//! C header: [`include/linux/jiffies.h`](srctree/include/linux/jiffies.h).
//! C header: [`include/linux/ktime.h`](srctree/include/linux/ktime.h).

pub mod delay;

/// The number of nanoseconds per microsecond.
pub const NSEC_PER_USEC: i64 = bindings::NSEC_PER_USEC as i64;

/// The number of nanoseconds per millisecond.
pub const NSEC_PER_MSEC: i64 = bindings::NSEC_PER_MSEC as i64;

/// The number of nanoseconds per second.
pub const NSEC_PER_SEC: i64 = bindings::NSEC_PER_SEC as i64;

/// The time unit of Linux kernel. One jiffy equals (1/HZ) second.
pub type Jiffies = core::ffi::c_ulong;

/// The millisecond time unit.
pub type Msecs = core::ffi::c_uint;

/// Converts milliseconds to jiffies.
#[inline]
pub fn msecs_to_jiffies(msecs: Msecs) -> Jiffies {
    // SAFETY: The `__msecs_to_jiffies` function is always safe to call no
    // matter what the argument is.
    unsafe { bindings::__msecs_to_jiffies(msecs) }
}

/// A specific point in time.
#[repr(transparent)]
#[derive(Copy, Clone, PartialEq, PartialOrd, Eq, Ord)]
pub struct Instant {
    // Range from 0 to `KTIME_MAX`.
    inner: bindings::ktime_t,
}

impl Instant {
    /// Create a `Instant` from a raw `ktime_t`.
    #[inline]
    fn from_raw(inner: bindings::ktime_t) -> Self {
        Self { inner }
    }

    /// Get the current time using `CLOCK_MONOTONIC`.
    #[inline]
    pub fn now() -> Self {
        // SAFETY: It is always safe to call `ktime_get` outside of NMI context.
        Self::from_raw(unsafe { bindings::ktime_get() })
    }

    #[inline]
    /// Return the amount of time elapsed since the `Instant`.
    pub fn elapsed(&self) -> Delta {
        Self::now() - *self
    }
}

impl core::ops::Sub for Instant {
    type Output = Delta;

    // never overflows
    #[inline]
    fn sub(self, other: Instant) -> Delta {
        Delta {
            nanos: self.inner - other.inner,
        }
    }
}

/// A span of time.
#[derive(Copy, Clone, PartialEq, PartialOrd, Eq, Ord, Debug)]
pub struct Delta {
    nanos: i64,
}

impl Delta {
    /// Create a new `Delta` from a number of milliseconds.
    #[inline]
    pub const fn from_millis(millis: i64) -> Self {
        Self {
            nanos: millis.saturating_mul(NSEC_PER_MSEC),
        }
    }

    /// Create a new `Delta` from a number of seconds.
    #[inline]
    pub const fn from_secs(secs: i64) -> Self {
        Self {
            nanos: secs.saturating_mul(NSEC_PER_SEC),
        }
    }

    /// Return `true` if the `Detla` spans no time.
    #[inline]
    pub fn is_zero(self) -> bool {
        self.as_nanos() == 0
    }

    /// Return the number of nanoseconds in the `Delta`.
    #[inline]
    pub fn as_nanos(self) -> i64 {
        self.nanos
    }

    /// Return the smallest number of microseconds greater than or equal
    /// to the value in the `Delta`.
    #[inline]
    pub fn as_micros_ceil(self) -> i64 {
        self.as_nanos().saturating_add(NSEC_PER_USEC - 1) / NSEC_PER_USEC
    }
}
