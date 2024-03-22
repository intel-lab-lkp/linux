// SPDX-License-Identifier: GPL-2.0

//! Architectural atomic and barrier primitives.

#[cfg(CONFIG_X86)]
pub(crate) use x86::*;

#[cfg(CONFIG_ARM64)]
pub(crate) use arm64::*;

#[cfg(CONFIG_X86)]
pub(crate) mod x86;

#[cfg(CONFIG_ARM64)]
pub(crate) mod arm64;
