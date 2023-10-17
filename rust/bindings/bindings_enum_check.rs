// SPDX-License-Identifier: GPL-2.0

//! Bindings' enum exhaustiveness check.
//!
//! Eventually, this should be replaced by a safe version of `--rustified-enum`, see
//! https://github.com/rust-lang/rust-bindgen/issues/2646.

#![no_std]
#![allow(
    clippy::all,
    dead_code,
    missing_docs,
    non_camel_case_types,
    non_upper_case_globals,
    non_snake_case,
    improper_ctypes,
    unreachable_pub,
    unsafe_op_in_unsafe_fn
)]

include!(concat!(
    env!("OBJTREE"),
    "/rust/bindings/bindings_generated_enum_check.rs"
));

fn check_phy_state(
    (phy_state::PHY_DOWN
    | phy_state::PHY_READY
    | phy_state::PHY_HALTED
    | phy_state::PHY_ERROR
    | phy_state::PHY_UP
    | phy_state::PHY_RUNNING
    | phy_state::PHY_NOLINK
    | phy_state::PHY_CABLETEST): phy_state,
) {
}
