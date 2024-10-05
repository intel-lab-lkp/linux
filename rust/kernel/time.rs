// SPDX-License-Identifier: GPL-2.0

//! Time related primitives.
//!
//! This module contains the kernel APIs related to time and timers that
//! have been ported or wrapped for usage by Rust code in the kernel.
//!
//! C header: [`include/linux/jiffies.h`](srctree/include/linux/jiffies.h).
//! C header: [`include/linux/ktime.h`](srctree/include/linux/ktime.h).

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

/// A Rust wrapper around a `ktime_t`.
#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Ktime {
    inner: bindings::ktime_t,
}

impl Ktime {
    /// Create a `Ktime` from a raw `ktime_t`.
    #[inline]
    pub fn from_raw(inner: bindings::ktime_t) -> Self {
        Self { inner }
    }

    /// Get the current time using `CLOCK_MONOTONIC`.
    #[inline]
    pub fn ktime_get() -> Self {
        // SAFETY: It is always safe to call `ktime_get` outside of NMI context.
        Self::from_raw(unsafe { bindings::ktime_get() })
    }

    /// Divide the number of nanoseconds by a compile-time constant.
    #[inline]
    fn divns_constant<const DIV: i64>(self) -> i64 {
        self.to_ns() / DIV
    }

    /// Returns the number of nanoseconds.
    #[inline]
    pub fn to_ns(self) -> i64 {
        self.inner
    }

    /// Returns the number of milliseconds.
    #[inline]
    pub fn to_ms(self) -> i64 {
        self.divns_constant::<NSEC_PER_MSEC>()
    }
}

/// Returns the number of milliseconds between two ktimes.
#[inline]
pub fn ktime_ms_delta(later: Ktime, earlier: Ktime) -> i64 {
    (later - earlier).to_ms()
}

impl core::ops::Sub for Ktime {
    type Output = Ktime;

    #[inline]
    fn sub(self, other: Ktime) -> Ktime {
        Self {
            inner: self.inner - other.inner,
        }
    }
}

impl PartialEq for Ktime {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        // SAFETY: FFI call.
        let ret = unsafe { bindings::ktime_compare(self.inner, other.inner) };
        ret == 0
    }
}

impl PartialOrd for Ktime {
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        // SAFETY: FFI call.
        let ret = unsafe { bindings::ktime_compare(self.inner, other.inner) };
        match ret {
            0 => Some(core::cmp::Ordering::Equal),
            x if x < 0 => Some(core::cmp::Ordering::Less),
            _ => Some(core::cmp::Ordering::Greater),
        }
    }
}

/// A span of time.
#[derive(Copy, Clone)]
pub struct Delta {
    nanos: i64,
}

impl Delta {
    /// Create a new `Delta` from a number of nanoseconds.
    #[inline]
    pub fn from_nanos(nanos: u16) -> Self {
        Self {
            nanos: nanos.into(),
        }
    }

    /// Create a new `Delta` from a number of microseconds.
    #[inline]
    pub fn from_micros(micros: u16) -> Self {
        Self {
            nanos: micros as i64 * NSEC_PER_USEC,
        }
    }

    /// Create a new `Delta` from a number of milliseconds.
    #[inline]
    pub fn from_millis(millis: u16) -> Self {
        Self {
            nanos: millis as i64 * NSEC_PER_MSEC,
        }
    }

    /// Create a new `Delta` from a number of seconds.
    #[inline]
    pub fn from_secs(secs: u16) -> Self {
        Self {
            nanos: secs as i64 * NSEC_PER_SEC,
        }
    }

    /// Return `true` if the `Detla` spans no time.
    #[inline]
    pub fn is_zero(self) -> bool {
        self.nanos == 0
    }

    /// Return the number of nanoseconds in the `Delta`.
    #[inline]
    pub fn as_nanos(self) -> i64 {
        self.nanos
    }

    /// Return the number of microseconds in the `Delta`.
    #[inline]
    pub fn as_micros(self) -> i64 {
        self.nanos / NSEC_PER_USEC
    }
}
