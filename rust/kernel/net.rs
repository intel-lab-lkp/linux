// SPDX-License-Identifier: GPL-2.0

//! Networking.

#[cfg(CONFIG_RUST_PHYLIB_ABSTRACTIONS)]
pub mod phy;
pub mod netdevice;
pub mod netlink_tap;
pub mod rtnl;
pub mod skbuff;
pub mod stats;
