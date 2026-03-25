// SPDX-License-Identifier: GPL-2.0

pub(crate) mod commands;
mod r570_144;
pub(crate) mod rm;

// Alias to avoid repeating the version number with every use.
use r570_144 as bindings;

use core::ops::Range;

use kernel::{
    dma::Coherent,
    prelude::*,
    ptr::{
        Alignable,
        Alignment,
        KnownSize, //
    },
    sizes::{
        SZ_128K,
        SZ_1M, //
    },
    transmute::{
        AsBytes,
        FromBytes, //
    },
};

use crate::{
    fb::FbLayout,
    firmware::gsp::GspFirmware,
    gpu::Chipset,
    gsp::{
        cmdq::Cmdq, //
        GSP_PAGE_SIZE,
    },
    num::{
        self,
        FromSafeCast, //
    },
};

// TODO: Replace with `IoView` projections once available.
pub(super) mod gsp_mem {
    use core::sync::atomic::{
        fence,
        Ordering, //
    };

    use kernel::{
        dma::Coherent,
        dma_read,
        dma_write, //
    };

    use crate::gsp::cmdq::{
        GspMem,
        MSGQ_NUM_PAGES, //
    };

    pub(in crate::gsp) fn gsp_write_ptr(qs: &Coherent<GspMem>) -> u32 {
        dma_read!(qs, .gspq.tx.0.writePtr) % MSGQ_NUM_PAGES
    }

    pub(in crate::gsp) fn gsp_read_ptr(qs: &Coherent<GspMem>) -> u32 {
        dma_read!(qs, .gspq.rx.0.readPtr) % MSGQ_NUM_PAGES
    }

    pub(in crate::gsp) fn cpu_read_ptr(qs: &Coherent<GspMem>) -> u32 {
        dma_read!(qs, .cpuq.rx.0.readPtr) % MSGQ_NUM_PAGES
    }

    pub(in crate::gsp) fn advance_cpu_read_ptr(qs: &Coherent<GspMem>, count: u32) {
        let rptr = cpu_read_ptr(qs).wrapping_add(count) % MSGQ_NUM_PAGES;

        // Ensure read pointer is properly ordered.
        fence(Ordering::SeqCst);

        dma_write!(qs, .cpuq.rx.0.readPtr, rptr);
    }

    pub(in crate::gsp) fn cpu_write_ptr(qs: &Coherent<GspMem>) -> u32 {
        dma_read!(qs, .cpuq.tx.0.writePtr) % MSGQ_NUM_PAGES
    }

    pub(in crate::gsp) fn advance_cpu_write_ptr(qs: &Coherent<GspMem>, count: u32) {
        let wptr = cpu_write_ptr(qs).wrapping_add(count) % MSGQ_NUM_PAGES;

        dma_write!(qs, .cpuq.tx.0.writePtr, wptr);

        // Ensure all command data is visible before triggering the GSP read.
        fence(Ordering::SeqCst);
    }
}

/// Maximum size of a single GSP message queue element in bytes.
pub(crate) const GSP_MSG_QUEUE_ELEMENT_SIZE_MAX: usize =
    num::u32_as_usize(bindings::GSP_MSG_QUEUE_ELEMENT_SIZE_MAX);

/// Status code returned by GSP-RM RPCs.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum NvStatus {
    Ok,
    AlreadySignalled,
    BrokenFb,
    BufferTooSmall,
    BusyRetry,
    CallbackNotScheduled,
    CardNotPresent,
    CycleDetected,
    DmaInUse,
    DmaMemNotLocked,
    DmaMemNotUnlocked,
    DualLinkInuse,
    EccError,
    FabricManagerNotPresent,
    FatalError,
    FeatureNotEnabled,
    FifoBadAccess,
    FlcnError,
    FreqNotSupported,
    Generic,
    GpuDmaNotInitialized,
    GpuInDebugMode,
    GpuInFullchipReset,
    GpuIsLost,
    GpuMemoryOnliningFailure,
    GpuNotFullPower,
    GpuUuidNotFound,
    HotSwitch,
    I2cError,
    I2cSpeedTooHigh,
    IllegalAction,
    InUse,
    InflateCompressedDataFailed,
    InsertDuplicateName,
    InsufficientPermissions,
    InsufficientPower,
    InsufficientResources,
    InsufficientZbcEntry,
    InvalidAccessType,
    InvalidAddress,
    InvalidArgument,
    InvalidBase,
    InvalidChannel,
    InvalidClass,
    InvalidClient,
    InvalidCommand,
    InvalidData,
    InvalidDevice,
    InvalidDmaSpecifier,
    InvalidEvent,
    InvalidFlags,
    InvalidFunction,
    InvalidHeap,
    InvalidIndex,
    InvalidIrqLevel,
    InvalidLicense,
    InvalidLimit,
    InvalidLockState,
    InvalidMethod,
    InvalidObject,
    InvalidObjectBuffer,
    InvalidObjectHandle,
    InvalidObjectNew,
    InvalidObjectOld,
    InvalidObjectParent,
    InvalidOffset,
    InvalidOperation,
    InvalidOwner,
    InvalidParamStruct,
    InvalidParameter,
    InvalidPath,
    InvalidPointer,
    InvalidRead,
    InvalidRegistryKey,
    InvalidRequest,
    InvalidState,
    InvalidStringLength,
    InvalidWrite,
    InvalidXlate,
    IrqEdgeTriggered,
    IrqNotFiring,
    KeyRotationInProgress,
    LibRmVersionMismatch,
    MaxSessionLimitReached,
    MemoryError,
    MemoryTrainingFailed,
    MismatchedSlave,
    MismatchedTarget,
    MissingTableEntry,
    ModuleLoadFailed,
    MoreDataAvailable,
    MoreProcessingRequired,
    MultipleMemoryTypes,
    NoFreeFifos,
    NoIntrPending,
    NoMemory,
    NoSuchDomain,
    NoValidPath,
    NotCompatible,
    NotReady,
    NotSupported,
    NvlinkClockError,
    NvlinkConfigurationError,
    NvlinkFabricFailure,
    NvlinkFabricNotReady,
    NvlinkInitError,
    NvlinkMinionError,
    NvlinkTrainingError,
    ObjectNotFound,
    ObjectTypeMismatch,
    OperatingSystem,
    OtherDeviceFound,
    OutOfRange,
    OverlappingUvmCommit,
    PageTableNotAvail,
    PidNotFound,
    PmuNotReady,
    PrivSecViolation,
    ProtectionFault,
    QueueTaskSlotNotAvailable,
    RcError,
    ReductionManagerNotAvailable,
    RejectedVbios,
    ResetRequired,
    ResourceLost,
    ResourceRetirementError,
    RiscvError,
    SecureBootFailed,
    SignalPending,
    StateInUse,
    TestOnlyCodeNotEnabled,
    Timeout,
    TimeoutRetry,
    TooManyPrimaries,
    UvmAddressInUse,
    Unknown(u32),
}

impl From<NvStatus> for Result {
    fn from(status: NvStatus) -> Self {
        match status {
            NvStatus::Ok => Ok(()),

            NvStatus::BufferTooSmall | NvStatus::MoreDataAvailable => Err(EMSGSIZE),

            NvStatus::BusyRetry
            | NvStatus::DmaInUse
            | NvStatus::DualLinkInuse
            | NvStatus::GpuInFullchipReset
            | NvStatus::InUse
            | NvStatus::KeyRotationInProgress
            | NvStatus::NotReady
            | NvStatus::NvlinkFabricNotReady
            | NvStatus::PmuNotReady
            | NvStatus::StateInUse
            | NvStatus::UvmAddressInUse => Err(EBUSY),

            NvStatus::CardNotPresent
            | NvStatus::FabricManagerNotPresent
            | NvStatus::InvalidDevice
            | NvStatus::ReductionManagerNotAvailable => Err(ENODEV),

            NvStatus::FeatureNotEnabled
            | NvStatus::FreqNotSupported
            | NvStatus::NotSupported
            | NvStatus::TestOnlyCodeNotEnabled => Err(ENOTSUPP),

            NvStatus::GpuUuidNotFound
            | NvStatus::MissingTableEntry
            | NvStatus::NoSuchDomain
            | NvStatus::NoValidPath
            | NvStatus::ObjectNotFound => Err(ENOENT),

            NvStatus::I2cSpeedTooHigh
            | NvStatus::InvalidAccessType
            | NvStatus::InvalidArgument
            | NvStatus::InvalidBase
            | NvStatus::InvalidChannel
            | NvStatus::InvalidClass
            | NvStatus::InvalidClient
            | NvStatus::InvalidCommand
            | NvStatus::InvalidData
            | NvStatus::InvalidDmaSpecifier
            | NvStatus::InvalidEvent
            | NvStatus::InvalidFlags
            | NvStatus::InvalidFunction
            | NvStatus::InvalidHeap
            | NvStatus::InvalidIndex
            | NvStatus::InvalidIrqLevel
            | NvStatus::InvalidLimit
            | NvStatus::InvalidLockState
            | NvStatus::InvalidMethod
            | NvStatus::InvalidObject
            | NvStatus::InvalidObjectBuffer
            | NvStatus::InvalidObjectHandle
            | NvStatus::InvalidObjectNew
            | NvStatus::InvalidObjectOld
            | NvStatus::InvalidObjectParent
            | NvStatus::InvalidOffset
            | NvStatus::InvalidOperation
            | NvStatus::InvalidOwner
            | NvStatus::InvalidParamStruct
            | NvStatus::InvalidParameter
            | NvStatus::InvalidPath
            | NvStatus::InvalidRegistryKey
            | NvStatus::InvalidRequest
            | NvStatus::InvalidState
            | NvStatus::InvalidStringLength
            | NvStatus::InvalidXlate
            | NvStatus::LibRmVersionMismatch
            | NvStatus::MismatchedSlave
            | NvStatus::MismatchedTarget
            | NvStatus::MultipleMemoryTypes
            | NvStatus::NotCompatible
            | NvStatus::ObjectTypeMismatch
            | NvStatus::OverlappingUvmCommit
            | NvStatus::RejectedVbios => Err(EINVAL),

            NvStatus::IllegalAction => Err(EPERM),

            NvStatus::InsertDuplicateName => Err(EEXIST),

            NvStatus::InsufficientPermissions
            | NvStatus::InvalidLicense
            | NvStatus::PrivSecViolation => Err(EACCES),

            NvStatus::InsufficientResources | NvStatus::NoMemory | NvStatus::PageTableNotAvail => {
                Err(ENOMEM)
            }

            NvStatus::InsufficientZbcEntry
            | NvStatus::MaxSessionLimitReached
            | NvStatus::NoFreeFifos
            | NvStatus::QueueTaskSlotNotAvailable
            | NvStatus::TooManyPrimaries => Err(ENOSPC),

            NvStatus::InvalidAddress | NvStatus::InvalidPointer | NvStatus::ProtectionFault => {
                Err(EFAULT)
            }

            NvStatus::MoreProcessingRequired | NvStatus::TimeoutRetry => Err(EAGAIN),

            NvStatus::OutOfRange => Err(ERANGE),

            NvStatus::PidNotFound => Err(ESRCH),

            NvStatus::SignalPending => Err(EINTR),

            NvStatus::Timeout => Err(ETIMEDOUT),

            _ => Err(EIO),
        }
    }
}

impl From<u32> for NvStatus {
    fn from(value: u32) -> Self {
        match value {
            bindings::NV_OK => Self::Ok,
            bindings::NV_ERR_ALREADY_SIGNALLED => Self::AlreadySignalled,
            bindings::NV_ERR_BROKEN_FB => Self::BrokenFb,
            bindings::NV_ERR_BUFFER_TOO_SMALL => Self::BufferTooSmall,
            bindings::NV_ERR_BUSY_RETRY => Self::BusyRetry,
            bindings::NV_ERR_CALLBACK_NOT_SCHEDULED => Self::CallbackNotScheduled,
            bindings::NV_ERR_CARD_NOT_PRESENT => Self::CardNotPresent,
            bindings::NV_ERR_CYCLE_DETECTED => Self::CycleDetected,
            bindings::NV_ERR_DMA_IN_USE => Self::DmaInUse,
            bindings::NV_ERR_DMA_MEM_NOT_LOCKED => Self::DmaMemNotLocked,
            bindings::NV_ERR_DMA_MEM_NOT_UNLOCKED => Self::DmaMemNotUnlocked,
            bindings::NV_ERR_DUAL_LINK_INUSE => Self::DualLinkInuse,
            bindings::NV_ERR_ECC_ERROR => Self::EccError,
            bindings::NV_ERR_FABRIC_MANAGER_NOT_PRESENT => Self::FabricManagerNotPresent,
            bindings::NV_ERR_FATAL_ERROR => Self::FatalError,
            bindings::NV_ERR_FEATURE_NOT_ENABLED => Self::FeatureNotEnabled,
            bindings::NV_ERR_FIFO_BAD_ACCESS => Self::FifoBadAccess,
            bindings::NV_ERR_FLCN_ERROR => Self::FlcnError,
            bindings::NV_ERR_FREQ_NOT_SUPPORTED => Self::FreqNotSupported,
            bindings::NV_ERR_GENERIC => Self::Generic,
            bindings::NV_ERR_GPU_DMA_NOT_INITIALIZED => Self::GpuDmaNotInitialized,
            bindings::NV_ERR_GPU_IN_DEBUG_MODE => Self::GpuInDebugMode,
            bindings::NV_ERR_GPU_IN_FULLCHIP_RESET => Self::GpuInFullchipReset,
            bindings::NV_ERR_GPU_IS_LOST => Self::GpuIsLost,
            bindings::NV_ERR_GPU_MEMORY_ONLINING_FAILURE => Self::GpuMemoryOnliningFailure,
            bindings::NV_ERR_GPU_NOT_FULL_POWER => Self::GpuNotFullPower,
            bindings::NV_ERR_GPU_UUID_NOT_FOUND => Self::GpuUuidNotFound,
            bindings::NV_ERR_HOT_SWITCH => Self::HotSwitch,
            bindings::NV_ERR_I2C_ERROR => Self::I2cError,
            bindings::NV_ERR_I2C_SPEED_TOO_HIGH => Self::I2cSpeedTooHigh,
            bindings::NV_ERR_ILLEGAL_ACTION => Self::IllegalAction,
            bindings::NV_ERR_IN_USE => Self::InUse,
            bindings::NV_ERR_INFLATE_COMPRESSED_DATA_FAILED => Self::InflateCompressedDataFailed,
            bindings::NV_ERR_INSERT_DUPLICATE_NAME => Self::InsertDuplicateName,
            bindings::NV_ERR_INSUFFICIENT_PERMISSIONS => Self::InsufficientPermissions,
            bindings::NV_ERR_INSUFFICIENT_POWER => Self::InsufficientPower,
            bindings::NV_ERR_INSUFFICIENT_RESOURCES => Self::InsufficientResources,
            bindings::NV_ERR_INSUFFICIENT_ZBC_ENTRY => Self::InsufficientZbcEntry,
            bindings::NV_ERR_INVALID_ACCESS_TYPE => Self::InvalidAccessType,
            bindings::NV_ERR_INVALID_ADDRESS => Self::InvalidAddress,
            bindings::NV_ERR_INVALID_ARGUMENT => Self::InvalidArgument,
            bindings::NV_ERR_INVALID_BASE => Self::InvalidBase,
            bindings::NV_ERR_INVALID_CHANNEL => Self::InvalidChannel,
            bindings::NV_ERR_INVALID_CLASS => Self::InvalidClass,
            bindings::NV_ERR_INVALID_CLIENT => Self::InvalidClient,
            bindings::NV_ERR_INVALID_COMMAND => Self::InvalidCommand,
            bindings::NV_ERR_INVALID_DATA => Self::InvalidData,
            bindings::NV_ERR_INVALID_DEVICE => Self::InvalidDevice,
            bindings::NV_ERR_INVALID_DMA_SPECIFIER => Self::InvalidDmaSpecifier,
            bindings::NV_ERR_INVALID_EVENT => Self::InvalidEvent,
            bindings::NV_ERR_INVALID_FLAGS => Self::InvalidFlags,
            bindings::NV_ERR_INVALID_FUNCTION => Self::InvalidFunction,
            bindings::NV_ERR_INVALID_HEAP => Self::InvalidHeap,
            bindings::NV_ERR_INVALID_INDEX => Self::InvalidIndex,
            bindings::NV_ERR_INVALID_IRQ_LEVEL => Self::InvalidIrqLevel,
            bindings::NV_ERR_INVALID_LICENSE => Self::InvalidLicense,
            bindings::NV_ERR_INVALID_LIMIT => Self::InvalidLimit,
            bindings::NV_ERR_INVALID_LOCK_STATE => Self::InvalidLockState,
            bindings::NV_ERR_INVALID_METHOD => Self::InvalidMethod,
            bindings::NV_ERR_INVALID_OBJECT => Self::InvalidObject,
            bindings::NV_ERR_INVALID_OBJECT_BUFFER => Self::InvalidObjectBuffer,
            bindings::NV_ERR_INVALID_OBJECT_HANDLE => Self::InvalidObjectHandle,
            bindings::NV_ERR_INVALID_OBJECT_NEW => Self::InvalidObjectNew,
            bindings::NV_ERR_INVALID_OBJECT_OLD => Self::InvalidObjectOld,
            bindings::NV_ERR_INVALID_OBJECT_PARENT => Self::InvalidObjectParent,
            bindings::NV_ERR_INVALID_OFFSET => Self::InvalidOffset,
            bindings::NV_ERR_INVALID_OPERATION => Self::InvalidOperation,
            bindings::NV_ERR_INVALID_OWNER => Self::InvalidOwner,
            bindings::NV_ERR_INVALID_PARAM_STRUCT => Self::InvalidParamStruct,
            bindings::NV_ERR_INVALID_PARAMETER => Self::InvalidParameter,
            bindings::NV_ERR_INVALID_PATH => Self::InvalidPath,
            bindings::NV_ERR_INVALID_POINTER => Self::InvalidPointer,
            bindings::NV_ERR_INVALID_READ => Self::InvalidRead,
            bindings::NV_ERR_INVALID_REGISTRY_KEY => Self::InvalidRegistryKey,
            bindings::NV_ERR_INVALID_REQUEST => Self::InvalidRequest,
            bindings::NV_ERR_INVALID_STATE => Self::InvalidState,
            bindings::NV_ERR_INVALID_STRING_LENGTH => Self::InvalidStringLength,
            bindings::NV_ERR_INVALID_WRITE => Self::InvalidWrite,
            bindings::NV_ERR_INVALID_XLATE => Self::InvalidXlate,
            bindings::NV_ERR_IRQ_EDGE_TRIGGERED => Self::IrqEdgeTriggered,
            bindings::NV_ERR_IRQ_NOT_FIRING => Self::IrqNotFiring,
            bindings::NV_ERR_KEY_ROTATION_IN_PROGRESS => Self::KeyRotationInProgress,
            bindings::NV_ERR_LIB_RM_VERSION_MISMATCH => Self::LibRmVersionMismatch,
            bindings::NV_ERR_MAX_SESSION_LIMIT_REACHED => Self::MaxSessionLimitReached,
            bindings::NV_ERR_MEMORY_ERROR => Self::MemoryError,
            bindings::NV_ERR_MEMORY_TRAINING_FAILED => Self::MemoryTrainingFailed,
            bindings::NV_ERR_MISMATCHED_SLAVE => Self::MismatchedSlave,
            bindings::NV_ERR_MISMATCHED_TARGET => Self::MismatchedTarget,
            bindings::NV_ERR_MISSING_TABLE_ENTRY => Self::MissingTableEntry,
            bindings::NV_ERR_MODULE_LOAD_FAILED => Self::ModuleLoadFailed,
            bindings::NV_ERR_MORE_DATA_AVAILABLE => Self::MoreDataAvailable,
            bindings::NV_ERR_MORE_PROCESSING_REQUIRED => Self::MoreProcessingRequired,
            bindings::NV_ERR_MULTIPLE_MEMORY_TYPES => Self::MultipleMemoryTypes,
            bindings::NV_ERR_NO_FREE_FIFOS => Self::NoFreeFifos,
            bindings::NV_ERR_NO_INTR_PENDING => Self::NoIntrPending,
            bindings::NV_ERR_NO_MEMORY => Self::NoMemory,
            bindings::NV_ERR_NO_SUCH_DOMAIN => Self::NoSuchDomain,
            bindings::NV_ERR_NO_VALID_PATH => Self::NoValidPath,
            bindings::NV_ERR_NOT_COMPATIBLE => Self::NotCompatible,
            bindings::NV_ERR_NOT_READY => Self::NotReady,
            bindings::NV_ERR_NOT_SUPPORTED => Self::NotSupported,
            bindings::NV_ERR_NVLINK_CLOCK_ERROR => Self::NvlinkClockError,
            bindings::NV_ERR_NVLINK_CONFIGURATION_ERROR => Self::NvlinkConfigurationError,
            bindings::NV_ERR_NVLINK_FABRIC_FAILURE => Self::NvlinkFabricFailure,
            bindings::NV_ERR_NVLINK_FABRIC_NOT_READY => Self::NvlinkFabricNotReady,
            bindings::NV_ERR_NVLINK_INIT_ERROR => Self::NvlinkInitError,
            bindings::NV_ERR_NVLINK_MINION_ERROR => Self::NvlinkMinionError,
            bindings::NV_ERR_NVLINK_TRAINING_ERROR => Self::NvlinkTrainingError,
            bindings::NV_ERR_OBJECT_NOT_FOUND => Self::ObjectNotFound,
            bindings::NV_ERR_OBJECT_TYPE_MISMATCH => Self::ObjectTypeMismatch,
            bindings::NV_ERR_OPERATING_SYSTEM => Self::OperatingSystem,
            bindings::NV_ERR_OTHER_DEVICE_FOUND => Self::OtherDeviceFound,
            bindings::NV_ERR_OUT_OF_RANGE => Self::OutOfRange,
            bindings::NV_ERR_OVERLAPPING_UVM_COMMIT => Self::OverlappingUvmCommit,
            bindings::NV_ERR_PAGE_TABLE_NOT_AVAIL => Self::PageTableNotAvail,
            bindings::NV_ERR_PID_NOT_FOUND => Self::PidNotFound,
            bindings::NV_ERR_PMU_NOT_READY => Self::PmuNotReady,
            bindings::NV_ERR_PRIV_SEC_VIOLATION => Self::PrivSecViolation,
            bindings::NV_ERR_PROTECTION_FAULT => Self::ProtectionFault,
            bindings::NV_ERR_QUEUE_TASK_SLOT_NOT_AVAILABLE => Self::QueueTaskSlotNotAvailable,
            bindings::NV_ERR_RC_ERROR => Self::RcError,
            bindings::NV_ERR_REDUCTION_MANAGER_NOT_AVAILABLE => Self::ReductionManagerNotAvailable,
            bindings::NV_ERR_REJECTED_VBIOS => Self::RejectedVbios,
            bindings::NV_ERR_RESET_REQUIRED => Self::ResetRequired,
            bindings::NV_ERR_RESOURCE_LOST => Self::ResourceLost,
            bindings::NV_ERR_RESOURCE_RETIREMENT_ERROR => Self::ResourceRetirementError,
            bindings::NV_ERR_RISCV_ERROR => Self::RiscvError,
            bindings::NV_ERR_SECURE_BOOT_FAILED => Self::SecureBootFailed,
            bindings::NV_ERR_SIGNAL_PENDING => Self::SignalPending,
            bindings::NV_ERR_STATE_IN_USE => Self::StateInUse,
            bindings::NV_ERR_TEST_ONLY_CODE_NOT_ENABLED => Self::TestOnlyCodeNotEnabled,
            bindings::NV_ERR_TIMEOUT => Self::Timeout,
            bindings::NV_ERR_TIMEOUT_RETRY => Self::TimeoutRetry,
            bindings::NV_ERR_TOO_MANY_PRIMARIES => Self::TooManyPrimaries,
            bindings::NV_ERR_UVM_ADDRESS_IN_USE => Self::UvmAddressInUse,
            other => Self::Unknown(other),
        }
    }
}

/// Empty type to group methods related to heap parameters for running the GSP firmware.
enum GspFwHeapParams {}

/// Minimum required alignment for the GSP heap.
const GSP_HEAP_ALIGNMENT: Alignment = Alignment::new::<{ 1 << 20 }>();

impl GspFwHeapParams {
    /// Returns the amount of GSP-RM heap memory used during GSP-RM boot and initialization (up to
    /// and including the first client subdevice allocation).
    fn base_rm_size(_chipset: Chipset) -> u64 {
        // TODO: this needs to be updated to return the correct value for Hopper+ once support for
        // them is added:
        // u64::from(bindings::GSP_FW_HEAP_PARAM_BASE_RM_SIZE_GH100)
        u64::from(bindings::GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X)
    }

    /// Returns the amount of heap memory required to support a single channel allocation.
    fn client_alloc_size() -> u64 {
        u64::from(bindings::GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE)
            .align_up(GSP_HEAP_ALIGNMENT)
            .unwrap_or(u64::MAX)
    }

    /// Returns the amount of memory to reserve for management purposes for a framebuffer of size
    /// `fb_size`.
    fn management_overhead(fb_size: u64) -> u64 {
        let fb_size_gb = fb_size.div_ceil(u64::from_safe_cast(kernel::sizes::SZ_1G));

        u64::from(bindings::GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB)
            .saturating_mul(fb_size_gb)
            .align_up(GSP_HEAP_ALIGNMENT)
            .unwrap_or(u64::MAX)
    }
}

/// Heap memory requirements and constraints for a given version of the GSP LIBOS.
pub(crate) struct LibosParams {
    /// The base amount of heap required by the GSP operating system, in bytes.
    carveout_size: u64,
    /// The minimum and maximum sizes allowed for the GSP FW heap, in bytes.
    allowed_heap_size: Range<u64>,
}

impl LibosParams {
    /// Version 2 of the GSP LIBOS (Turing and GA100)
    const LIBOS2: LibosParams = LibosParams {
        carveout_size: num::u32_as_u64(bindings::GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS2),
        allowed_heap_size: num::u32_as_u64(bindings::GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS2_MIN_MB)
            * num::usize_as_u64(SZ_1M)
            ..num::u32_as_u64(bindings::GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS2_MAX_MB)
                * num::usize_as_u64(SZ_1M),
    };

    /// Version 3 of the GSP LIBOS (GA102+)
    const LIBOS3: LibosParams = LibosParams {
        carveout_size: num::u32_as_u64(bindings::GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS3_BAREMETAL),
        allowed_heap_size: num::u32_as_u64(
            bindings::GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_BAREMETAL_MIN_MB,
        ) * num::usize_as_u64(SZ_1M)
            ..num::u32_as_u64(bindings::GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_BAREMETAL_MAX_MB)
                * num::usize_as_u64(SZ_1M),
    };

    /// Returns the libos parameters corresponding to `chipset`.
    pub(crate) fn from_chipset(chipset: Chipset) -> &'static LibosParams {
        if chipset < Chipset::GA102 {
            &Self::LIBOS2
        } else {
            &Self::LIBOS3
        }
    }

    /// Returns the amount of memory (in bytes) to allocate for the WPR heap for a framebuffer size
    /// of `fb_size` (in bytes) for `chipset`.
    pub(crate) fn wpr_heap_size(&self, chipset: Chipset, fb_size: u64) -> u64 {
        // The WPR heap will contain the following:
        // LIBOS carveout,
        self.carveout_size
            // RM boot working memory,
            .saturating_add(GspFwHeapParams::base_rm_size(chipset))
            // One RM client,
            .saturating_add(GspFwHeapParams::client_alloc_size())
            // Overhead for memory management.
            .saturating_add(GspFwHeapParams::management_overhead(fb_size))
            // Clamp to the supported heap sizes.
            .clamp(self.allowed_heap_size.start, self.allowed_heap_size.end - 1)
    }
}

/// Structure passed to the GSP bootloader, containing the framebuffer layout as well as the DMA
/// addresses of the GSP bootloader and firmware.
#[repr(transparent)]
pub(crate) struct GspFwWprMeta {
    inner: bindings::GspFwWprMeta,
}

// SAFETY: Padding is explicit and does not contain uninitialized data.
unsafe impl AsBytes for GspFwWprMeta {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for GspFwWprMeta {}

type GspFwWprMetaBootResumeInfo = bindings::GspFwWprMeta__bindgen_ty_1;
type GspFwWprMetaBootInfo = bindings::GspFwWprMeta__bindgen_ty_1__bindgen_ty_1;

impl GspFwWprMeta {
    /// Returns an initializer for a `GspFwWprMeta` suitable for booting `gsp_firmware` using the
    /// `fb_layout` layout.
    pub(crate) fn new<'a>(
        gsp_firmware: &'a GspFirmware,
        fb_layout: &'a FbLayout,
    ) -> impl Init<Self> + 'a {
        #[allow(non_snake_case)]
        let init_inner = init!(bindings::GspFwWprMeta {
            // CAST: we want to store the bits of `GSP_FW_WPR_META_MAGIC` unmodified.
            magic: bindings::GSP_FW_WPR_META_MAGIC as u64,
            revision: u64::from(bindings::GSP_FW_WPR_META_REVISION),
            sysmemAddrOfRadix3Elf: gsp_firmware.radix3_dma_handle(),
            sizeOfRadix3Elf: u64::from_safe_cast(gsp_firmware.size),
            sysmemAddrOfBootloader: gsp_firmware.bootloader.ucode.dma_handle(),
            sizeOfBootloader: u64::from_safe_cast(gsp_firmware.bootloader.ucode.size()),
            bootloaderCodeOffset: u64::from(gsp_firmware.bootloader.code_offset),
            bootloaderDataOffset: u64::from(gsp_firmware.bootloader.data_offset),
            bootloaderManifestOffset: u64::from(gsp_firmware.bootloader.manifest_offset),
            __bindgen_anon_1: GspFwWprMetaBootResumeInfo {
                __bindgen_anon_1: GspFwWprMetaBootInfo {
                    sysmemAddrOfSignature: gsp_firmware.signatures.dma_handle(),
                    sizeOfSignature: u64::from_safe_cast(gsp_firmware.signatures.size()),
                },
            },
            gspFwRsvdStart: fb_layout.heap.start,
            nonWprHeapOffset: fb_layout.heap.start,
            nonWprHeapSize: fb_layout.heap.end - fb_layout.heap.start,
            gspFwWprStart: fb_layout.wpr2.start,
            gspFwHeapOffset: fb_layout.wpr2_heap.start,
            gspFwHeapSize: fb_layout.wpr2_heap.end - fb_layout.wpr2_heap.start,
            gspFwOffset: fb_layout.elf.start,
            bootBinOffset: fb_layout.boot.start,
            frtsOffset: fb_layout.frts.start,
            frtsSize: fb_layout.frts.end - fb_layout.frts.start,
            gspFwWprEnd: fb_layout
                .vga_workspace
                .start
                .align_down(Alignment::new::<SZ_128K>()),
            gspFwHeapVfPartitionCount: fb_layout.vf_partition_count,
            fbSize: fb_layout.fb.end - fb_layout.fb.start,
            vgaWorkspaceOffset: fb_layout.vga_workspace.start,
            vgaWorkspaceSize: fb_layout.vga_workspace.end - fb_layout.vga_workspace.start,
            ..Zeroable::init_zeroed()
        });

        init!(GspFwWprMeta {
            inner <- init_inner,
        })
    }
}

#[derive(Copy, Clone, Debug, PartialEq)]
#[repr(u32)]
pub(crate) enum MsgFunction {
    // Common function codes
    AllocChannelDma = bindings::NV_VGPU_MSG_FUNCTION_ALLOC_CHANNEL_DMA,
    AllocCtxDma = bindings::NV_VGPU_MSG_FUNCTION_ALLOC_CTX_DMA,
    AllocDevice = bindings::NV_VGPU_MSG_FUNCTION_ALLOC_DEVICE,
    AllocMemory = bindings::NV_VGPU_MSG_FUNCTION_ALLOC_MEMORY,
    AllocObject = bindings::NV_VGPU_MSG_FUNCTION_ALLOC_OBJECT,
    AllocRoot = bindings::NV_VGPU_MSG_FUNCTION_ALLOC_ROOT,
    BindCtxDma = bindings::NV_VGPU_MSG_FUNCTION_BIND_CTX_DMA,
    ContinuationRecord = bindings::NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD,
    Free = bindings::NV_VGPU_MSG_FUNCTION_FREE,
    GetGspStaticInfo = bindings::NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO,
    GetStaticInfo = bindings::NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO,
    GspInitPostObjGpu = bindings::NV_VGPU_MSG_FUNCTION_GSP_INIT_POST_OBJGPU,
    GspRmControl = bindings::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL,
    GspSetSystemInfo = bindings::NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO,
    Log = bindings::NV_VGPU_MSG_FUNCTION_LOG,
    MapMemory = bindings::NV_VGPU_MSG_FUNCTION_MAP_MEMORY,
    Nop = bindings::NV_VGPU_MSG_FUNCTION_NOP,
    SetGuestSystemInfo = bindings::NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO,
    SetRegistry = bindings::NV_VGPU_MSG_FUNCTION_SET_REGISTRY,

    // Event codes
    GspInitDone = bindings::NV_VGPU_MSG_EVENT_GSP_INIT_DONE,
    GspLockdownNotice = bindings::NV_VGPU_MSG_EVENT_GSP_LOCKDOWN_NOTICE,
    GspPostNoCat = bindings::NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD,
    GspRunCpuSequencer = bindings::NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER,
    MmuFaultQueued = bindings::NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED,
    OsErrorLog = bindings::NV_VGPU_MSG_EVENT_OS_ERROR_LOG,
    PostEvent = bindings::NV_VGPU_MSG_EVENT_POST_EVENT,
    RcTriggered = bindings::NV_VGPU_MSG_EVENT_RC_TRIGGERED,
    UcodeLibOsPrint = bindings::NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT,
}

impl TryFrom<u32> for MsgFunction {
    type Error = kernel::error::Error;

    fn try_from(value: u32) -> Result<MsgFunction> {
        match value {
            // Common function codes
            bindings::NV_VGPU_MSG_FUNCTION_ALLOC_CHANNEL_DMA => Ok(MsgFunction::AllocChannelDma),
            bindings::NV_VGPU_MSG_FUNCTION_ALLOC_CTX_DMA => Ok(MsgFunction::AllocCtxDma),
            bindings::NV_VGPU_MSG_FUNCTION_ALLOC_DEVICE => Ok(MsgFunction::AllocDevice),
            bindings::NV_VGPU_MSG_FUNCTION_ALLOC_MEMORY => Ok(MsgFunction::AllocMemory),
            bindings::NV_VGPU_MSG_FUNCTION_ALLOC_OBJECT => Ok(MsgFunction::AllocObject),
            bindings::NV_VGPU_MSG_FUNCTION_ALLOC_ROOT => Ok(MsgFunction::AllocRoot),
            bindings::NV_VGPU_MSG_FUNCTION_BIND_CTX_DMA => Ok(MsgFunction::BindCtxDma),
            bindings::NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD => {
                Ok(MsgFunction::ContinuationRecord)
            }
            bindings::NV_VGPU_MSG_FUNCTION_FREE => Ok(MsgFunction::Free),
            bindings::NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO => Ok(MsgFunction::GetGspStaticInfo),
            bindings::NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO => Ok(MsgFunction::GetStaticInfo),
            bindings::NV_VGPU_MSG_FUNCTION_GSP_INIT_POST_OBJGPU => {
                Ok(MsgFunction::GspInitPostObjGpu)
            }
            bindings::NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL => Ok(MsgFunction::GspRmControl),
            bindings::NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO => Ok(MsgFunction::GspSetSystemInfo),
            bindings::NV_VGPU_MSG_FUNCTION_LOG => Ok(MsgFunction::Log),
            bindings::NV_VGPU_MSG_FUNCTION_MAP_MEMORY => Ok(MsgFunction::MapMemory),
            bindings::NV_VGPU_MSG_FUNCTION_NOP => Ok(MsgFunction::Nop),
            bindings::NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO => {
                Ok(MsgFunction::SetGuestSystemInfo)
            }
            bindings::NV_VGPU_MSG_FUNCTION_SET_REGISTRY => Ok(MsgFunction::SetRegistry),

            // Event codes
            bindings::NV_VGPU_MSG_EVENT_GSP_INIT_DONE => Ok(MsgFunction::GspInitDone),
            bindings::NV_VGPU_MSG_EVENT_GSP_LOCKDOWN_NOTICE => Ok(MsgFunction::GspLockdownNotice),
            bindings::NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD => Ok(MsgFunction::GspPostNoCat),
            bindings::NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER => {
                Ok(MsgFunction::GspRunCpuSequencer)
            }
            bindings::NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED => Ok(MsgFunction::MmuFaultQueued),
            bindings::NV_VGPU_MSG_EVENT_OS_ERROR_LOG => Ok(MsgFunction::OsErrorLog),
            bindings::NV_VGPU_MSG_EVENT_POST_EVENT => Ok(MsgFunction::PostEvent),
            bindings::NV_VGPU_MSG_EVENT_RC_TRIGGERED => Ok(MsgFunction::RcTriggered),
            bindings::NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT => Ok(MsgFunction::UcodeLibOsPrint),
            _ => Err(EINVAL),
        }
    }
}

impl From<MsgFunction> for u32 {
    fn from(value: MsgFunction) -> Self {
        // CAST: `MsgFunction` is `repr(u32)` and can thus be cast losslessly.
        value as u32
    }
}

/// Sequencer buffer opcode for GSP sequencer commands.
#[derive(Copy, Clone, Debug, PartialEq)]
#[repr(u32)]
pub(crate) enum SeqBufOpcode {
    // Core operation opcodes
    CoreReset = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_RESET,
    CoreResume = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_RESUME,
    CoreStart = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_START,
    CoreWaitForHalt = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT,

    // Delay opcode
    DelayUs = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_DELAY_US,

    // Register operation opcodes
    RegModify = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_MODIFY,
    RegPoll = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_POLL,
    RegStore = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_STORE,
    RegWrite = bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_WRITE,
}

impl TryFrom<u32> for SeqBufOpcode {
    type Error = kernel::error::Error;

    fn try_from(value: u32) -> Result<SeqBufOpcode> {
        match value {
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_RESET => {
                Ok(SeqBufOpcode::CoreReset)
            }
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_RESUME => {
                Ok(SeqBufOpcode::CoreResume)
            }
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_START => {
                Ok(SeqBufOpcode::CoreStart)
            }
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT => {
                Ok(SeqBufOpcode::CoreWaitForHalt)
            }
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_DELAY_US => Ok(SeqBufOpcode::DelayUs),
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_MODIFY => {
                Ok(SeqBufOpcode::RegModify)
            }
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_POLL => Ok(SeqBufOpcode::RegPoll),
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_STORE => Ok(SeqBufOpcode::RegStore),
            bindings::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_WRITE => Ok(SeqBufOpcode::RegWrite),
            _ => Err(EINVAL),
        }
    }
}

impl From<SeqBufOpcode> for u32 {
    fn from(value: SeqBufOpcode) -> Self {
        // CAST: `SeqBufOpcode` is `repr(u32)` and can thus be cast losslessly.
        value as u32
    }
}

/// Wrapper for GSP sequencer register write payload.
#[repr(transparent)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct RegWritePayload(bindings::GSP_SEQ_BUF_PAYLOAD_REG_WRITE);

impl RegWritePayload {
    /// Returns the register address.
    pub(crate) fn addr(&self) -> u32 {
        self.0.addr
    }

    /// Returns the value to write.
    pub(crate) fn val(&self) -> u32 {
        self.0.val
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for RegWritePayload {}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for RegWritePayload {}

/// Wrapper for GSP sequencer register modify payload.
#[repr(transparent)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct RegModifyPayload(bindings::GSP_SEQ_BUF_PAYLOAD_REG_MODIFY);

impl RegModifyPayload {
    /// Returns the register address.
    pub(crate) fn addr(&self) -> u32 {
        self.0.addr
    }

    /// Returns the mask to apply.
    pub(crate) fn mask(&self) -> u32 {
        self.0.mask
    }

    /// Returns the value to write.
    pub(crate) fn val(&self) -> u32 {
        self.0.val
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for RegModifyPayload {}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for RegModifyPayload {}

/// Wrapper for GSP sequencer register poll payload.
#[repr(transparent)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct RegPollPayload(bindings::GSP_SEQ_BUF_PAYLOAD_REG_POLL);

impl RegPollPayload {
    /// Returns the register address.
    pub(crate) fn addr(&self) -> u32 {
        self.0.addr
    }

    /// Returns the mask to apply.
    pub(crate) fn mask(&self) -> u32 {
        self.0.mask
    }

    /// Returns the expected value.
    pub(crate) fn val(&self) -> u32 {
        self.0.val
    }

    /// Returns the timeout in microseconds.
    pub(crate) fn timeout(&self) -> u32 {
        self.0.timeout
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for RegPollPayload {}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for RegPollPayload {}

/// Wrapper for GSP sequencer delay payload.
#[repr(transparent)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct DelayUsPayload(bindings::GSP_SEQ_BUF_PAYLOAD_DELAY_US);

impl DelayUsPayload {
    /// Returns the delay value in microseconds.
    pub(crate) fn val(&self) -> u32 {
        self.0.val
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for DelayUsPayload {}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for DelayUsPayload {}

/// Wrapper for GSP sequencer register store payload.
#[repr(transparent)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct RegStorePayload(bindings::GSP_SEQ_BUF_PAYLOAD_REG_STORE);

impl RegStorePayload {
    /// Returns the register address.
    pub(crate) fn addr(&self) -> u32 {
        self.0.addr
    }

    /// Returns the storage index.
    #[allow(unused)]
    pub(crate) fn index(&self) -> u32 {
        self.0.index
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for RegStorePayload {}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for RegStorePayload {}

/// Wrapper for GSP sequencer buffer command.
#[repr(transparent)]
pub(crate) struct SequencerBufferCmd(bindings::GSP_SEQUENCER_BUFFER_CMD);

impl SequencerBufferCmd {
    /// Returns the opcode as a `SeqBufOpcode` enum, or error if invalid.
    pub(crate) fn opcode(&self) -> Result<SeqBufOpcode> {
        self.0.opCode.try_into()
    }

    /// Returns the register write payload by value.
    ///
    /// Returns an error if the opcode is not `SeqBufOpcode::RegWrite`.
    pub(crate) fn reg_write_payload(&self) -> Result<RegWritePayload> {
        if self.opcode()? != SeqBufOpcode::RegWrite {
            return Err(EINVAL);
        }
        // SAFETY: Opcode is verified to be `RegWrite`, so union contains valid `RegWritePayload`.
        Ok(RegWritePayload(unsafe { self.0.payload.regWrite }))
    }

    /// Returns the register modify payload by value.
    ///
    /// Returns an error if the opcode is not `SeqBufOpcode::RegModify`.
    pub(crate) fn reg_modify_payload(&self) -> Result<RegModifyPayload> {
        if self.opcode()? != SeqBufOpcode::RegModify {
            return Err(EINVAL);
        }
        // SAFETY: Opcode is verified to be `RegModify`, so union contains valid `RegModifyPayload`.
        Ok(RegModifyPayload(unsafe { self.0.payload.regModify }))
    }

    /// Returns the register poll payload by value.
    ///
    /// Returns an error if the opcode is not `SeqBufOpcode::RegPoll`.
    pub(crate) fn reg_poll_payload(&self) -> Result<RegPollPayload> {
        if self.opcode()? != SeqBufOpcode::RegPoll {
            return Err(EINVAL);
        }
        // SAFETY: Opcode is verified to be `RegPoll`, so union contains valid `RegPollPayload`.
        Ok(RegPollPayload(unsafe { self.0.payload.regPoll }))
    }

    /// Returns the delay payload by value.
    ///
    /// Returns an error if the opcode is not `SeqBufOpcode::DelayUs`.
    pub(crate) fn delay_us_payload(&self) -> Result<DelayUsPayload> {
        if self.opcode()? != SeqBufOpcode::DelayUs {
            return Err(EINVAL);
        }
        // SAFETY: Opcode is verified to be `DelayUs`, so union contains valid `DelayUsPayload`.
        Ok(DelayUsPayload(unsafe { self.0.payload.delayUs }))
    }

    /// Returns the register store payload by value.
    ///
    /// Returns an error if the opcode is not `SeqBufOpcode::RegStore`.
    pub(crate) fn reg_store_payload(&self) -> Result<RegStorePayload> {
        if self.opcode()? != SeqBufOpcode::RegStore {
            return Err(EINVAL);
        }
        // SAFETY: Opcode is verified to be `RegStore`, so union contains valid `RegStorePayload`.
        Ok(RegStorePayload(unsafe { self.0.payload.regStore }))
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for SequencerBufferCmd {}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for SequencerBufferCmd {}

/// Wrapper for GSP run CPU sequencer RPC.
#[repr(transparent)]
pub(crate) struct RunCpuSequencer(bindings::rpc_run_cpu_sequencer_v17_00);

impl RunCpuSequencer {
    /// Returns the command index.
    pub(crate) fn cmd_index(&self) -> u32 {
        self.0.cmdIndex
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for RunCpuSequencer {}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for RunCpuSequencer {}

/// Struct containing the arguments required to pass a memory buffer to the GSP
/// for use during initialisation.
///
/// The GSP only understands 4K pages (GSP_PAGE_SIZE), so even if the kernel is
/// configured for a larger page size (e.g. 64K pages), we need to give
/// the GSP an array of 4K pages. Since we only create physically contiguous
/// buffers the math to calculate the addresses is simple.
///
/// The buffers must be a multiple of GSP_PAGE_SIZE.  GSP-RM also currently
/// ignores the @kind field for LOGINIT, LOGINTR, and LOGRM, but expects the
/// buffers to be physically contiguous anyway.
///
/// The memory allocated for the arguments must remain until the GSP sends the
/// init_done RPC.
#[repr(transparent)]
pub(crate) struct LibosMemoryRegionInitArgument {
    inner: bindings::LibosMemoryRegionInitArgument,
}

// SAFETY: Padding is explicit and does not contain uninitialized data.
unsafe impl AsBytes for LibosMemoryRegionInitArgument {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for LibosMemoryRegionInitArgument {}

impl LibosMemoryRegionInitArgument {
    pub(crate) fn new<'a, A: AsBytes + FromBytes + KnownSize + ?Sized>(
        name: &'static str,
        obj: &'a Coherent<A>,
    ) -> impl Init<Self> + 'a {
        /// Generates the `ID8` identifier required for some GSP objects.
        fn id8(name: &str) -> u64 {
            let mut bytes = [0u8; core::mem::size_of::<u64>()];

            for (c, b) in name.bytes().rev().zip(&mut bytes) {
                *b = c;
            }

            u64::from_ne_bytes(bytes)
        }

        #[allow(non_snake_case)]
        let init_inner = init!(bindings::LibosMemoryRegionInitArgument {
            id8: id8(name),
            pa: obj.dma_handle(),
            size: num::usize_as_u64(obj.size()),
            kind: num::u32_into_u8::<
                { bindings::LibosMemoryRegionKind_LIBOS_MEMORY_REGION_CONTIGUOUS },
            >(),
            loc: num::u32_into_u8::<
                { bindings::LibosMemoryRegionLoc_LIBOS_MEMORY_REGION_LOC_SYSMEM },
            >(),
            ..Zeroable::init_zeroed()
        });

        init!(LibosMemoryRegionInitArgument {
            inner <- init_inner,
        })
    }
}

/// TX header for setting up a message queue with the GSP.
#[repr(transparent)]
pub(crate) struct MsgqTxHeader(bindings::msgqTxHeader);

impl MsgqTxHeader {
    /// Create a new TX queue header.
    ///
    /// # Arguments
    ///
    /// * `msgq_size` - Total size of the message queue structure, in bytes.
    /// * `rx_hdr_offset` - Offset, in bytes, of the start of the RX header in the message queue
    ///   structure.
    /// * `msg_count` - Number of messages that can be sent, i.e. the number of memory pages
    ///   allocated for the message queue in the message queue structure.
    pub(crate) fn new(msgq_size: u32, rx_hdr_offset: u32, msg_count: u32) -> Self {
        Self(bindings::msgqTxHeader {
            version: 0,
            size: msgq_size,
            msgSize: num::usize_into_u32::<GSP_PAGE_SIZE>(),
            msgCount: msg_count,
            writePtr: 0,
            flags: 1,
            rxHdrOff: rx_hdr_offset,
            entryOff: num::usize_into_u32::<GSP_PAGE_SIZE>(),
        })
    }
}

// SAFETY: Padding is explicit and does not contain uninitialized data.
unsafe impl AsBytes for MsgqTxHeader {}

/// RX header for setting up a message queue with the GSP.
#[repr(transparent)]
pub(crate) struct MsgqRxHeader(bindings::msgqRxHeader);

/// Header for the message RX queue.
impl MsgqRxHeader {
    /// Creates a new RX queue header.
    pub(crate) fn new() -> Self {
        Self(Default::default())
    }
}

// SAFETY: Padding is explicit and does not contain uninitialized data.
unsafe impl AsBytes for MsgqRxHeader {}

bitfield! {
    struct MsgHeaderVersion(u32) {
        31:24 major as u8;
        23:16 minor as u8;
    }
}

impl MsgHeaderVersion {
    const MAJOR_TOT: u8 = 3;
    const MINOR_TOT: u8 = 0;

    fn new() -> Self {
        Self::default()
            .set_major(Self::MAJOR_TOT)
            .set_minor(Self::MINOR_TOT)
    }
}

impl bindings::rpc_message_header_v {
    fn init(cmd_size: usize, function: MsgFunction) -> impl Init<Self, Error> {
        type RpcMessageHeader = bindings::rpc_message_header_v;

        try_init!(RpcMessageHeader {
            header_version: MsgHeaderVersion::new().into(),
            signature: bindings::NV_VGPU_MSG_SIGNATURE_VALID,
            function: function.into(),
            length: size_of::<Self>()
                .checked_add(cmd_size)
                .ok_or(EOVERFLOW)
                .and_then(|v| v.try_into().map_err(|_| EINVAL))?,
            rpc_result: 0xffffffff,
            rpc_result_private: 0xffffffff,
            ..Zeroable::init_zeroed()
        })
    }
}

/// GSP Message Element.
///
/// This is essentially a message header expected to be followed by the message data.
#[repr(transparent)]
pub(crate) struct GspMsgElement {
    inner: bindings::GSP_MSG_QUEUE_ELEMENT,
}

impl GspMsgElement {
    /// Creates a new message element.
    ///
    /// # Arguments
    ///
    /// * `sequence` - Sequence number of the message.
    /// * `cmd_size` - Size of the command (not including the message element), in bytes.
    /// * `function` - Function of the message.
    #[allow(non_snake_case)]
    pub(crate) fn init(
        sequence: u32,
        cmd_size: usize,
        function: MsgFunction,
    ) -> impl Init<Self, Error> {
        type RpcMessageHeader = bindings::rpc_message_header_v;
        type InnerGspMsgElement = bindings::GSP_MSG_QUEUE_ELEMENT;
        let init_inner = try_init!(InnerGspMsgElement {
            seqNum: sequence,
            elemCount: size_of::<Self>()
                .checked_add(cmd_size)
                .ok_or(EOVERFLOW)?
                .div_ceil(GSP_PAGE_SIZE)
                .try_into()
                .map_err(|_| EOVERFLOW)?,
            rpc <- RpcMessageHeader::init(cmd_size, function),
            ..Zeroable::init_zeroed()
        });

        try_init!(GspMsgElement {
            inner <- init_inner,
        })
    }

    /// Sets the checksum of this message.
    ///
    /// Since the header is also part of the checksum, this is usually called after the whole
    /// message has been written to the shared memory area.
    pub(crate) fn set_checksum(&mut self, checksum: u32) {
        self.inner.checkSum = checksum;
    }

    /// Returns the length of the message's payload.
    pub(crate) fn payload_length(&self) -> usize {
        // `rpc.length` includes the length of the RPC message header.
        num::u32_as_usize(self.inner.rpc.length)
            .saturating_sub(size_of::<bindings::rpc_message_header_v>())
    }

    /// Returns the total length of the message, message and RPC headers included.
    pub(crate) fn length(&self) -> usize {
        size_of::<Self>() + self.payload_length()
    }

    // Returns the sequence number of the message.
    pub(crate) fn sequence(&self) -> u32 {
        self.inner.rpc.sequence
    }

    // Returns the function of the message, if it is valid, or the invalid function number as an
    // error.
    pub(crate) fn function(&self) -> Result<MsgFunction, u32> {
        self.inner
            .rpc
            .function
            .try_into()
            .map_err(|_| self.inner.rpc.function)
    }

    // Returns the number of elements (i.e. memory pages) used by this message.
    pub(crate) fn element_count(&self) -> u32 {
        self.inner.elemCount
    }
}

// SAFETY: Padding is explicit and does not contain uninitialized data.
unsafe impl AsBytes for GspMsgElement {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for GspMsgElement {}

/// Arguments for GSP startup.
#[repr(transparent)]
#[derive(Zeroable)]
pub(crate) struct GspArgumentsCached {
    inner: bindings::GSP_ARGUMENTS_CACHED,
}

impl GspArgumentsCached {
    /// Creates the arguments for starting the GSP up using `cmdq` as its command queue.
    pub(crate) fn new(cmdq: &Cmdq) -> impl Init<Self> + '_ {
        #[allow(non_snake_case)]
        let init_inner = init!(bindings::GSP_ARGUMENTS_CACHED {
            messageQueueInitArguments <- MessageQueueInitArguments::new(cmdq),
            bDmemStack: 1,
            ..Zeroable::init_zeroed()
        });

        init!(GspArgumentsCached {
            inner <- init_inner,
        })
    }
}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for GspArgumentsCached {}

/// On Turing and GA100, the entries in the `LibosMemoryRegionInitArgument`
/// must all be a multiple of GSP_PAGE_SIZE in size, so add padding to force it
/// to that size.
#[repr(C)]
#[derive(Zeroable)]
pub(crate) struct GspArgumentsPadded {
    pub(crate) inner: GspArgumentsCached,
    _padding: [u8; GSP_PAGE_SIZE - core::mem::size_of::<bindings::GSP_ARGUMENTS_CACHED>()],
}

impl GspArgumentsPadded {
    pub(crate) fn new(cmdq: &Cmdq) -> impl Init<Self> + '_ {
        init!(GspArgumentsPadded {
            inner <- GspArgumentsCached::new(cmdq),
            ..Zeroable::init_zeroed()
        })
    }
}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for GspArgumentsPadded {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for GspArgumentsPadded {}

/// Init arguments for the message queue.
type MessageQueueInitArguments = bindings::MESSAGE_QUEUE_INIT_ARGUMENTS;

impl MessageQueueInitArguments {
    /// Creates a new init arguments structure for `cmdq`.
    #[allow(non_snake_case)]
    fn new(cmdq: &Cmdq) -> impl Init<Self> + '_ {
        init!(MessageQueueInitArguments {
            sharedMemPhysAddr: cmdq.dma_handle,
            pageTableEntryCount: num::usize_into_u32::<{ Cmdq::NUM_PTES }>(),
            cmdQueueOffset: num::usize_as_u64(Cmdq::CMDQ_OFFSET),
            statQueueOffset: num::usize_as_u64(Cmdq::STATQ_OFFSET),
            ..Zeroable::init_zeroed()
        })
    }
}
