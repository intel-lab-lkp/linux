// SPDX-License-Identifier: GPL-2.0

// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

//! Interconnect abstractions
//!
//! (based on clk.rs)
//!
//! C headers:
//! [`include/linux/interconnect.h`](srctree/include/linux/interconnect.h)
//! [`include/linux/interconnect-provider.h`](srctree/include/linux/interconnect-provider.h)
//!
//! Reference: <https://docs.kernel.org/driver-api/interconnect.html>

/// The interconnect framework bandidth unit.
///
/// Represents a bus bandwidth request in kBps, wrapping a [`u32`] value.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct IccBwUnit(pub u32);

impl IccBwUnit {
    /// Create a new instance from bytes (B)
    pub const fn from_bytes_per_sec(bps: u32) -> Self {
        Self(bps / 1000)
    }

    /// Create a new instance from kilobytes (kB) per second
    pub const fn from_kilobytes_per_sec(kbps: u32) -> Self {
        Self(kbps)
    }

    /// Create a new instance from megabytes (MB) per second
    pub const fn from_megabytes_per_sec(mbps: u32) -> Self {
        Self(mbps * 1000)
    }

    /// Create a new instance from gigabytes (GB) per second
    pub const fn from_gigabytes_per_sec(gbps: u32) -> Self {
        Self(gbps * 1000 * 1000)
    }

    /// Create a new instance from bits (b) per second
    pub const fn from_bits_per_sec(_bps: u32) -> Self {
        Self(1)
    }

    /// Create a new instance from kilobits (kb) per second
    pub const fn from_kilobits_per_sec(kbps: u32) -> Self {
        Self(kbps.div_ceil(8))
    }

    /// Create a new instance from megabits (Mb) per second
    pub const fn from_megabits_per_sec(mbps: u32) -> Self {
        Self(mbps * 1000 / 8)
    }

    /// Create a new instance from gigabits (Gb) per second
    pub const fn from_gigabits_per_sec(mbps: u32) -> Self {
        Self(mbps * 1000 * 1000 / 8)
    }

    /// Get the bandwidth in bytes (B) per second
    pub const fn as_bytes_per_sec(self) -> u32 {
        self.0 * 1000
    }

    /// Get the bandwidth in kilobytes (kB) per second
    pub const fn as_kilobytes_per_sec(self) -> u32 {
        self.0
    }

    /// Get the bandwidth in megabytes (MB) per second
    pub const fn as_megabytes_per_sec(self) -> u32 {
        self.0 / 1000
    }

    /// Get the bandwidth in gigabytes (GB) per second
    pub const fn as_gigabytes_per_sec(self) -> u32 {
        self.0 / 1000 / 1000
    }

    /// Get the bandwidth in bits (b) per second
    pub const fn as_bits_per_sec(self) -> u32 {
        self.0 * 8 / 1000
    }

    /// Get the bandwidth in kilobits (kb) per second
    pub const fn as_kilobits_per_sec(self) -> u32 {
        self.0 * 8
    }

    /// Get the bandwidth in megabits (Mb) per second
    pub const fn as_megabits_per_sec(self) -> u32 {
        self.0 * 8 * 1000
    }

    /// Get the bandwidth in gigabits (Gb) per second
    pub const fn as_gigabits_per_sec(self) -> u32 {
        self.0 * 8 * 1000 * 1000
    }
}

impl From<IccBwUnit> for u32 {
    fn from(bw: IccBwUnit) -> Self {
        bw.0
    }
}

#[cfg(CONFIG_INTERCONNECT)]
mod icc_path {
    use super::IccBwUnit;
    use crate::{
        device::Device,
        error::{Result, from_err_ptr, to_result},
        prelude::*,
    };

    use core::ptr;

    /// A reference-counted interconnect path.
    ///
    /// Rust abstraction for the C [`struct icc_path`]
    ///
    /// # Invariants
    ///
    /// An [`IccPath`] instance holds either a pointer to a valid [`struct icc_path`] created by
    /// the C portion of the kernel, or a NULL pointer.
    ///
    /// Instances of this type are reference-counted. Calling [`IccPath::of_get`] ensures that the
    /// allocation remains valid for the lifetime of the [`IccPath`].
    ///
    /// # Examples
    ///
    /// The following example demonstrates hwo to obtain and configure an interconnect path for
    /// a device.
    ///
    /// ```
    /// use kernel::icc_path::{IccPath, IccBwUnit};
    /// use kernel::device::Device;
    /// use kernel::error::Result;
    ///
    /// fn poke_at_bus(dev: &Device) -> Result {
    ///     let bus_path = IccPath::of_get(dev, Some(c_str!("bus")))?;
    ///
    ///     bus_path.set_bw(
    ///         IccBwUnit::from_megabits_per_sec(400),
    ///         IccBwUnit::from_megabits_per_sec(800)
    ///     )?;
    ///
    ///     // bus_path goes out of scope and self-disables if there are no other users
    ///
    ///     Ok(())
    /// }
    /// ```
    #[repr(transparent)]
    pub struct IccPath(*mut bindings::icc_path);

    impl IccPath {
        /// Get [`IccPath`] corresponding to a [`Device`]
        ///
        /// Equivalent to the kernel's of_icc_get() API.
        pub fn of_get(dev: &Device, name: Option<&CStr>) -> Result<Self> {
            let id = name.map_or(ptr::null(), |n| n.as_ptr());

            // SAFETY: It's always safe to call [`of_icc_get`]
            //
            // INVARIANT: The reference count is decremented when [`IccPath`] goes out of scope
            Ok(Self(from_err_ptr(unsafe {
                bindings::of_icc_get(dev.as_raw(), id)
            })?))
        }

        /// Obtain the raw [`struct icc_path`] pointer.
        #[inline]
        pub fn as_raw(&self) -> *mut bindings::icc_path {
            self.0
        }

        /// Enable the path.
        ///
        /// Equivalent to the kernel's icc_enable() API.
        #[inline]
        pub fn enable(&self) -> Result {
            // SAFETY: By the type invariants, self.as_raw() is a valid argument for `icc_enable`].
            to_result(unsafe { bindings::icc_enable(self.as_raw()) })
        }

        /// Disable the path.
        ///
        /// Equivalent to the kernel's icc_disable() API.
        #[inline]
        pub fn disable(&self) -> Result {
            // SAFETY: By the type invariants, self.as_raw() is a valid argument for `icc_disable`].
            to_result(unsafe { bindings::icc_disable(self.as_raw()) })
        }

        /// Set the bandwidth on a path
        ///
        /// Equivalent to the kernel's icc_set_bw() API.
        #[inline]
        pub fn set_bw(&self, avg_bw: IccBwUnit, peak_bw: IccBwUnit) -> Result {
            // SAFETY: By the type invariants, self.as_raw() is a valid argument for [`icc_set_bw`].
            to_result(unsafe {
                bindings::icc_set_bw(
                    self.as_raw(),
                    avg_bw.as_kilobytes_per_sec(),
                    peak_bw.as_kilobytes_per_sec(),
                )
            })
        }
    }

    impl Drop for IccPath {
        fn drop(&mut self) {
            // SAFETY: By the type invariants, self.as_raw() is a valid argument for [`icc_put`].
            unsafe { bindings::icc_put(self.as_raw()) }
        }
    }
}

// SAFETY: An `IccPath` is always reference-counted and can be released from any thread.
unsafe impl Send for IccPath {}

#[cfg(CONFIG_INTERCONNECT)]
pub use icc_path::*;
