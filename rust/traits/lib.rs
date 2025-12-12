// SPDX-License-Identifier: GPL-2.0

//! Support crate containing traits and other definitions that need to be available without the
//! kernel crate. The usual case for this is code that needs to be available in the bindings crate.
#![no_std]
pub mod transmute;
