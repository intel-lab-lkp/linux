// SPDX-License-Identifier: GPL-2.0

//! Rust support related to IOMMU.

#[cfg(CONFIG_IOMMU_API)]
pub mod domain;
pub mod pgtable;

/// Protection flags used with IOMMU mappings.
pub mod prot {
    /// Read access.
    pub const READ: u32 = bindings::IOMMU_READ;
    /// Write access.
    pub const WRITE: u32 = bindings::IOMMU_WRITE;
    /// Request cache coherency.
    pub const CACHE: u32 = bindings::IOMMU_CACHE;
    /// Request no-execute permission.
    pub const NOEXEC: u32 = bindings::IOMMU_NOEXEC;
    /// MMIO peripheral mapping.
    pub const MMIO: u32 = bindings::IOMMU_MMIO;
    /// Privileged mapping.
    pub const PRIVILEGED: u32 = bindings::IOMMU_PRIV;
}
