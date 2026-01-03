// SPDX-License-Identifier: GPL-2.0

//! PCI device header type definitions.
//!
//! This module contains PCI header type definitions

use kernel::bindings;

/// PCI device header types.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HeaderType {
    /// Normal PCI device header (Type 0)
    NormalDevice,
    /// PCI-to-PCI bridge header (Type 1)
    PciToPciBridge,
    /// CardBus bridge header (Type 2)
    CardBusBridge,
    /// Unknown or unsupported header type
    Unknown,
}

impl From<u8> for HeaderType {
    fn from(value: u8) -> Self {
        match u32::from(value) & bindings::PCI_HEADER_TYPE_MASK {
            bindings::PCI_HEADER_TYPE_NORMAL => HeaderType::NormalDevice,
            bindings::PCI_HEADER_TYPE_BRIDGE => HeaderType::PciToPciBridge,
            bindings::PCI_HEADER_TYPE_CARDBUS => HeaderType::CardBusBridge,
            _ => HeaderType::Unknown,
        }
    }
}
