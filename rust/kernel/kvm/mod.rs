// SPDX-License-Identifier: GPL-2.0

//! KVM (Kernel-based Virtual Machine) abstractions.
//!
//! This module provides Rust implementations for KVM subsystem components,
//! starting with the eventfd-based irqfd and ioeventfd mechanisms.

pub mod eventfd;
