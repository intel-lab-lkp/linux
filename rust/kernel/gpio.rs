// SPDX-License-Identifier: GPL-2.0

//! GPIO abstractions.

use crate::{
    error::{
        Error,
        Result, //
    },
    fmt,
    prelude::*, //
};

/// Describes GPIO direction.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum LineDirection {
    /// Represent the output direction.
    Out = bindings::GPIO_LINE_DIRECTION_OUT,

    /// Represent the input direction.
    In = bindings::GPIO_LINE_DIRECTION_IN,
}

impl core::ops::Not for LineDirection {
    type Output = Self;
    fn not(self) -> Self::Output {
        match self {
            Self::Out => Self::In,
            Self::In => Self::Out,
        }
    }
}

impl TryFrom<c_int> for LineDirection {
    type Error = Error;
    fn try_from(value: c_int) -> Result<Self> {
        match value {
            v if v == Self::Out as c_int => Ok(Self::Out),
            v if v == Self::In as c_int => Ok(Self::In),
            _ => Err(EINVAL),
        }
    }
}

impl fmt::Display for LineDirection {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::In => f.pad("In"),
            Self::Out => f.pad("Out"),
        }
    }
}

/// Describes the logical GPIO level, i.e. taking the ACTIVE_LOW status into account.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum LogicalLineLevel {
    /// Represent the logical inactive level.
    Inactive,

    /// Represent the logical active level.
    Active,
}

impl core::ops::Not for LogicalLineLevel {
    type Output = Self;
    fn not(self) -> Self::Output {
        match self {
            Self::Inactive => Self::Active,
            Self::Active => Self::Inactive,
        }
    }
}

impl LogicalLineLevel {
    fn as_c_int(&self) -> c_int {
        match self {
            Self::Inactive => 0,
            Self::Active => 1,
        }
    }
}

impl TryFrom<c_int> for LogicalLineLevel {
    type Error = Error;
    fn try_from(value: c_int) -> Result<Self> {
        match value {
            0 => Ok(Self::Inactive),
            1 => Ok(Self::Active),
            _ => Err(EINVAL),
        }
    }
}

impl fmt::Display for LogicalLineLevel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Inactive => f.pad("Inactive"),
            Self::Active => f.pad("Active"),
        }
    }
}

/// Describes the raw GPIO level, i.e. the value of its physical line without regard for its
/// ACTIVE_LOW status.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PhysicalLineLevel {
    /// Represent the physical LOW level.
    Low,

    /// Represent the physical HIGH level.
    High,
}

impl core::ops::Not for PhysicalLineLevel {
    type Output = Self;
    fn not(self) -> Self::Output {
        match self {
            Self::Low => Self::High,
            Self::High => Self::Low,
        }
    }
}

impl PhysicalLineLevel {
    fn as_c_int(&self) -> c_int {
        match self {
            Self::Low => 0,
            Self::High => 1,
        }
    }
}

impl TryFrom<c_int> for PhysicalLineLevel {
    type Error = Error;
    fn try_from(value: c_int) -> Result<Self> {
        match value {
            0 => Ok(Self::Low),
            1 => Ok(Self::High),
            _ => Err(EINVAL),
        }
    }
}

impl fmt::Display for PhysicalLineLevel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Low => f.pad("Low"),
            Self::High => f.pad("High"),
        }
    }
}
