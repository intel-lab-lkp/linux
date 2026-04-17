// SPDX-License-Identifier: GPL-2.0

pub(crate) mod commands;
mod r570_144;
pub(crate) mod rm;

// Alias to avoid repeating the version number with every use.
use r570_144 as bindings;

use core::{
    fmt,
    ops::Range, //
};

use kernel::{
    device,
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

/// Status code returned by GSP-RM operations.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum GspMsgRmStatus {
    /// The operation succeeded.
    Ok,
    /// The operation completed with a non-fatal warning.
    Warning(GspMsgRmWarning),
    /// The operation failed with a GSP-RM-specific error.
    Error(GspMsgRmError),
}

/// Warning code returned by GSP-RM RPCs.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum GspMsgRmWarning {
    HotSwitch = bindings::NV_WARN_HOT_SWITCH,
    IncorrectPerfmonData = bindings::NV_WARN_INCORRECT_PERFMON_DATA,
    MismatchedSlave = bindings::NV_WARN_MISMATCHED_SLAVE,
    MismatchedTarget = bindings::NV_WARN_MISMATCHED_TARGET,
    MoreProcessingRequired = bindings::NV_WARN_MORE_PROCESSING_REQUIRED,
    NothingToDo = bindings::NV_WARN_NOTHING_TO_DO,
    NullObject = bindings::NV_WARN_NULL_OBJECT,
    OutOfRange = bindings::NV_WARN_OUT_OF_RANGE,
}

// TODO[FPRI]: This is a temporary solution to be replaced with the corresponding derive macros
// once they land.
impl TryFrom<u32> for GspMsgRmWarning {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            bindings::NV_WARN_HOT_SWITCH => Ok(Self::HotSwitch),
            bindings::NV_WARN_INCORRECT_PERFMON_DATA => Ok(Self::IncorrectPerfmonData),
            bindings::NV_WARN_MISMATCHED_SLAVE => Ok(Self::MismatchedSlave),
            bindings::NV_WARN_MISMATCHED_TARGET => Ok(Self::MismatchedTarget),
            bindings::NV_WARN_MORE_PROCESSING_REQUIRED => Ok(Self::MoreProcessingRequired),
            bindings::NV_WARN_NOTHING_TO_DO => Ok(Self::NothingToDo),
            bindings::NV_WARN_NULL_OBJECT => Ok(Self::NullObject),
            bindings::NV_WARN_OUT_OF_RANGE => Ok(Self::OutOfRange),
            _ => Err(EINVAL),
        }
    }
}

/// Error code returned by GSP-RM RPCs.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum GspMsgRmError {
    AlreadySignalled = bindings::NV_ERR_ALREADY_SIGNALLED,
    BrokenFb = bindings::NV_ERR_BROKEN_FB,
    BufferTooSmall = bindings::NV_ERR_BUFFER_TOO_SMALL,
    BusyRetry = bindings::NV_ERR_BUSY_RETRY,
    CallbackNotScheduled = bindings::NV_ERR_CALLBACK_NOT_SCHEDULED,
    CardNotPresent = bindings::NV_ERR_CARD_NOT_PRESENT,
    CycleDetected = bindings::NV_ERR_CYCLE_DETECTED,
    DmaInUse = bindings::NV_ERR_DMA_IN_USE,
    DmaMemNotLocked = bindings::NV_ERR_DMA_MEM_NOT_LOCKED,
    DmaMemNotUnlocked = bindings::NV_ERR_DMA_MEM_NOT_UNLOCKED,
    DualLinkInuse = bindings::NV_ERR_DUAL_LINK_INUSE,
    EccError = bindings::NV_ERR_ECC_ERROR,
    FabricManagerNotPresent = bindings::NV_ERR_FABRIC_MANAGER_NOT_PRESENT,
    FatalError = bindings::NV_ERR_FATAL_ERROR,
    FeatureNotEnabled = bindings::NV_ERR_FEATURE_NOT_ENABLED,
    FifoBadAccess = bindings::NV_ERR_FIFO_BAD_ACCESS,
    FlcnError = bindings::NV_ERR_FLCN_ERROR,
    FreqNotSupported = bindings::NV_ERR_FREQ_NOT_SUPPORTED,
    Generic = bindings::NV_ERR_GENERIC,
    GpuDmaNotInitialized = bindings::NV_ERR_GPU_DMA_NOT_INITIALIZED,
    GpuInDebugMode = bindings::NV_ERR_GPU_IN_DEBUG_MODE,
    GpuInFullchipReset = bindings::NV_ERR_GPU_IN_FULLCHIP_RESET,
    GpuIsLost = bindings::NV_ERR_GPU_IS_LOST,
    GpuMemoryOnliningFailure = bindings::NV_ERR_GPU_MEMORY_ONLINING_FAILURE,
    GpuNotFullPower = bindings::NV_ERR_GPU_NOT_FULL_POWER,
    GpuUuidNotFound = bindings::NV_ERR_GPU_UUID_NOT_FOUND,
    HotSwitch = bindings::NV_ERR_HOT_SWITCH,
    I2cError = bindings::NV_ERR_I2C_ERROR,
    I2cSpeedTooHigh = bindings::NV_ERR_I2C_SPEED_TOO_HIGH,
    IllegalAction = bindings::NV_ERR_ILLEGAL_ACTION,
    InUse = bindings::NV_ERR_IN_USE,
    InflateCompressedDataFailed = bindings::NV_ERR_INFLATE_COMPRESSED_DATA_FAILED,
    InsertDuplicateName = bindings::NV_ERR_INSERT_DUPLICATE_NAME,
    InsufficientPermissions = bindings::NV_ERR_INSUFFICIENT_PERMISSIONS,
    InsufficientPower = bindings::NV_ERR_INSUFFICIENT_POWER,
    InsufficientResources = bindings::NV_ERR_INSUFFICIENT_RESOURCES,
    InsufficientZbcEntry = bindings::NV_ERR_INSUFFICIENT_ZBC_ENTRY,
    InvalidAccessType = bindings::NV_ERR_INVALID_ACCESS_TYPE,
    InvalidAddress = bindings::NV_ERR_INVALID_ADDRESS,
    InvalidArgument = bindings::NV_ERR_INVALID_ARGUMENT,
    InvalidBase = bindings::NV_ERR_INVALID_BASE,
    InvalidChannel = bindings::NV_ERR_INVALID_CHANNEL,
    InvalidClass = bindings::NV_ERR_INVALID_CLASS,
    InvalidClient = bindings::NV_ERR_INVALID_CLIENT,
    InvalidCommand = bindings::NV_ERR_INVALID_COMMAND,
    InvalidData = bindings::NV_ERR_INVALID_DATA,
    InvalidDevice = bindings::NV_ERR_INVALID_DEVICE,
    InvalidDmaSpecifier = bindings::NV_ERR_INVALID_DMA_SPECIFIER,
    InvalidEvent = bindings::NV_ERR_INVALID_EVENT,
    InvalidFlags = bindings::NV_ERR_INVALID_FLAGS,
    InvalidFunction = bindings::NV_ERR_INVALID_FUNCTION,
    InvalidHeap = bindings::NV_ERR_INVALID_HEAP,
    InvalidIndex = bindings::NV_ERR_INVALID_INDEX,
    InvalidIrqLevel = bindings::NV_ERR_INVALID_IRQ_LEVEL,
    InvalidLicense = bindings::NV_ERR_INVALID_LICENSE,
    InvalidLimit = bindings::NV_ERR_INVALID_LIMIT,
    InvalidLockState = bindings::NV_ERR_INVALID_LOCK_STATE,
    InvalidMethod = bindings::NV_ERR_INVALID_METHOD,
    InvalidObject = bindings::NV_ERR_INVALID_OBJECT,
    InvalidObjectBuffer = bindings::NV_ERR_INVALID_OBJECT_BUFFER,
    InvalidObjectHandle = bindings::NV_ERR_INVALID_OBJECT_HANDLE,
    InvalidObjectNew = bindings::NV_ERR_INVALID_OBJECT_NEW,
    InvalidObjectOld = bindings::NV_ERR_INVALID_OBJECT_OLD,
    InvalidObjectParent = bindings::NV_ERR_INVALID_OBJECT_PARENT,
    InvalidOffset = bindings::NV_ERR_INVALID_OFFSET,
    InvalidOperation = bindings::NV_ERR_INVALID_OPERATION,
    InvalidOwner = bindings::NV_ERR_INVALID_OWNER,
    InvalidParamStruct = bindings::NV_ERR_INVALID_PARAM_STRUCT,
    InvalidParameter = bindings::NV_ERR_INVALID_PARAMETER,
    InvalidPath = bindings::NV_ERR_INVALID_PATH,
    InvalidPointer = bindings::NV_ERR_INVALID_POINTER,
    InvalidRead = bindings::NV_ERR_INVALID_READ,
    InvalidRegistryKey = bindings::NV_ERR_INVALID_REGISTRY_KEY,
    InvalidRequest = bindings::NV_ERR_INVALID_REQUEST,
    InvalidState = bindings::NV_ERR_INVALID_STATE,
    InvalidStringLength = bindings::NV_ERR_INVALID_STRING_LENGTH,
    InvalidWrite = bindings::NV_ERR_INVALID_WRITE,
    InvalidXlate = bindings::NV_ERR_INVALID_XLATE,
    IrqEdgeTriggered = bindings::NV_ERR_IRQ_EDGE_TRIGGERED,
    IrqNotFiring = bindings::NV_ERR_IRQ_NOT_FIRING,
    KeyRotationInProgress = bindings::NV_ERR_KEY_ROTATION_IN_PROGRESS,
    LibRmVersionMismatch = bindings::NV_ERR_LIB_RM_VERSION_MISMATCH,
    MaxSessionLimitReached = bindings::NV_ERR_MAX_SESSION_LIMIT_REACHED,
    MemoryError = bindings::NV_ERR_MEMORY_ERROR,
    MemoryTrainingFailed = bindings::NV_ERR_MEMORY_TRAINING_FAILED,
    MismatchedSlave = bindings::NV_ERR_MISMATCHED_SLAVE,
    MismatchedTarget = bindings::NV_ERR_MISMATCHED_TARGET,
    MissingTableEntry = bindings::NV_ERR_MISSING_TABLE_ENTRY,
    ModuleLoadFailed = bindings::NV_ERR_MODULE_LOAD_FAILED,
    MoreDataAvailable = bindings::NV_ERR_MORE_DATA_AVAILABLE,
    MoreProcessingRequired = bindings::NV_ERR_MORE_PROCESSING_REQUIRED,
    MultipleMemoryTypes = bindings::NV_ERR_MULTIPLE_MEMORY_TYPES,
    NoFreeFifos = bindings::NV_ERR_NO_FREE_FIFOS,
    NoIntrPending = bindings::NV_ERR_NO_INTR_PENDING,
    NoMemory = bindings::NV_ERR_NO_MEMORY,
    NoSuchDomain = bindings::NV_ERR_NO_SUCH_DOMAIN,
    NoValidPath = bindings::NV_ERR_NO_VALID_PATH,
    NotCompatible = bindings::NV_ERR_NOT_COMPATIBLE,
    NotReady = bindings::NV_ERR_NOT_READY,
    NotSupported = bindings::NV_ERR_NOT_SUPPORTED,
    NvlinkClockError = bindings::NV_ERR_NVLINK_CLOCK_ERROR,
    NvlinkConfigurationError = bindings::NV_ERR_NVLINK_CONFIGURATION_ERROR,
    NvlinkFabricFailure = bindings::NV_ERR_NVLINK_FABRIC_FAILURE,
    NvlinkFabricNotReady = bindings::NV_ERR_NVLINK_FABRIC_NOT_READY,
    NvlinkInitError = bindings::NV_ERR_NVLINK_INIT_ERROR,
    NvlinkMinionError = bindings::NV_ERR_NVLINK_MINION_ERROR,
    NvlinkTrainingError = bindings::NV_ERR_NVLINK_TRAINING_ERROR,
    ObjectNotFound = bindings::NV_ERR_OBJECT_NOT_FOUND,
    ObjectTypeMismatch = bindings::NV_ERR_OBJECT_TYPE_MISMATCH,
    OperatingSystem = bindings::NV_ERR_OPERATING_SYSTEM,
    OtherDeviceFound = bindings::NV_ERR_OTHER_DEVICE_FOUND,
    OutOfRange = bindings::NV_ERR_OUT_OF_RANGE,
    OverlappingUvmCommit = bindings::NV_ERR_OVERLAPPING_UVM_COMMIT,
    PageTableNotAvail = bindings::NV_ERR_PAGE_TABLE_NOT_AVAIL,
    PidNotFound = bindings::NV_ERR_PID_NOT_FOUND,
    PmuNotReady = bindings::NV_ERR_PMU_NOT_READY,
    PrivSecViolation = bindings::NV_ERR_PRIV_SEC_VIOLATION,
    ProtectionFault = bindings::NV_ERR_PROTECTION_FAULT,
    QueueTaskSlotNotAvailable = bindings::NV_ERR_QUEUE_TASK_SLOT_NOT_AVAILABLE,
    RcError = bindings::NV_ERR_RC_ERROR,
    ReductionManagerNotAvailable = bindings::NV_ERR_REDUCTION_MANAGER_NOT_AVAILABLE,
    RejectedVbios = bindings::NV_ERR_REJECTED_VBIOS,
    ResetRequired = bindings::NV_ERR_RESET_REQUIRED,
    ResourceLost = bindings::NV_ERR_RESOURCE_LOST,
    ResourceRetirementError = bindings::NV_ERR_RESOURCE_RETIREMENT_ERROR,
    RiscvError = bindings::NV_ERR_RISCV_ERROR,
    SecureBootFailed = bindings::NV_ERR_SECURE_BOOT_FAILED,
    SignalPending = bindings::NV_ERR_SIGNAL_PENDING,
    StateInUse = bindings::NV_ERR_STATE_IN_USE,
    TestOnlyCodeNotEnabled = bindings::NV_ERR_TEST_ONLY_CODE_NOT_ENABLED,
    Timeout = bindings::NV_ERR_TIMEOUT,
    TimeoutRetry = bindings::NV_ERR_TIMEOUT_RETRY,
    TooManyPrimaries = bindings::NV_ERR_TOO_MANY_PRIMARIES,
    UvmAddressInUse = bindings::NV_ERR_UVM_ADDRESS_IN_USE,
}

impl From<GspMsgRmError> for Error {
    fn from(status: GspMsgRmError) -> Self {
        match status {
            GspMsgRmError::BufferTooSmall | GspMsgRmError::MoreDataAvailable => ETOOSMALL,

            GspMsgRmError::DmaInUse
            | GspMsgRmError::DmaMemNotUnlocked
            | GspMsgRmError::DualLinkInuse
            | GspMsgRmError::GpuInDebugMode
            | GspMsgRmError::GpuInFullchipReset
            | GspMsgRmError::KeyRotationInProgress
            | GspMsgRmError::NotReady
            | GspMsgRmError::NvlinkFabricNotReady
            | GspMsgRmError::PmuNotReady
            | GspMsgRmError::StateInUse
            | GspMsgRmError::UvmAddressInUse => EBUSY,

            GspMsgRmError::CardNotPresent
            | GspMsgRmError::FabricManagerNotPresent
            | GspMsgRmError::GpuDmaNotInitialized
            | GspMsgRmError::GpuIsLost
            | GspMsgRmError::GpuUuidNotFound
            | GspMsgRmError::OtherDeviceFound
            | GspMsgRmError::ReductionManagerNotAvailable
            | GspMsgRmError::ResetRequired => ENODEV,

            GspMsgRmError::FeatureNotEnabled
            | GspMsgRmError::FreqNotSupported
            | GspMsgRmError::NotSupported
            | GspMsgRmError::TestOnlyCodeNotEnabled => ENOTSUPP,

            GspMsgRmError::CallbackNotScheduled
            | GspMsgRmError::MissingTableEntry
            | GspMsgRmError::NoIntrPending
            | GspMsgRmError::NoSuchDomain
            | GspMsgRmError::NoValidPath
            | GspMsgRmError::ObjectNotFound
            | GspMsgRmError::ResourceLost => ENOENT,

            GspMsgRmError::DmaMemNotLocked
            | GspMsgRmError::I2cSpeedTooHigh
            | GspMsgRmError::InflateCompressedDataFailed
            | GspMsgRmError::InvalidArgument
            | GspMsgRmError::InvalidBase
            | GspMsgRmError::InvalidChannel
            | GspMsgRmError::InvalidClass
            | GspMsgRmError::InvalidClient
            | GspMsgRmError::InvalidCommand
            | GspMsgRmError::InvalidData
            | GspMsgRmError::InvalidDevice
            | GspMsgRmError::InvalidDmaSpecifier
            | GspMsgRmError::InvalidEvent
            | GspMsgRmError::InvalidFlags
            | GspMsgRmError::InvalidFunction
            | GspMsgRmError::InvalidHeap
            | GspMsgRmError::InvalidIndex
            | GspMsgRmError::InvalidIrqLevel
            | GspMsgRmError::InvalidLimit
            | GspMsgRmError::InvalidLockState
            | GspMsgRmError::InvalidMethod
            | GspMsgRmError::InvalidObject
            | GspMsgRmError::InvalidObjectBuffer
            | GspMsgRmError::InvalidObjectHandle
            | GspMsgRmError::InvalidObjectNew
            | GspMsgRmError::InvalidObjectOld
            | GspMsgRmError::InvalidObjectParent
            | GspMsgRmError::InvalidOffset
            | GspMsgRmError::InvalidOperation
            | GspMsgRmError::InvalidOwner
            | GspMsgRmError::InvalidParamStruct
            | GspMsgRmError::InvalidParameter
            | GspMsgRmError::InvalidPath
            | GspMsgRmError::InvalidRegistryKey
            | GspMsgRmError::InvalidRequest
            | GspMsgRmError::InvalidState
            | GspMsgRmError::InvalidStringLength
            | GspMsgRmError::InvalidXlate
            | GspMsgRmError::LibRmVersionMismatch
            | GspMsgRmError::MismatchedSlave
            | GspMsgRmError::MismatchedTarget
            | GspMsgRmError::MultipleMemoryTypes
            | GspMsgRmError::NotCompatible
            | GspMsgRmError::ObjectTypeMismatch
            | GspMsgRmError::OverlappingUvmCommit
            | GspMsgRmError::RejectedVbios => EINVAL,

            GspMsgRmError::IllegalAction | GspMsgRmError::InsufficientPermissions => EPERM,

            GspMsgRmError::AlreadySignalled
            | GspMsgRmError::InUse
            | GspMsgRmError::InsertDuplicateName => EEXIST,

            GspMsgRmError::FifoBadAccess
            | GspMsgRmError::InvalidAccessType
            | GspMsgRmError::InvalidLicense
            | GspMsgRmError::PrivSecViolation
            | GspMsgRmError::SecureBootFailed => EACCES,

            GspMsgRmError::GpuMemoryOnliningFailure
            | GspMsgRmError::InsufficientResources
            | GspMsgRmError::NoMemory
            | GspMsgRmError::PageTableNotAvail => ENOMEM,

            GspMsgRmError::InsufficientZbcEntry
            | GspMsgRmError::MaxSessionLimitReached
            | GspMsgRmError::NoFreeFifos
            | GspMsgRmError::QueueTaskSlotNotAvailable
            | GspMsgRmError::TooManyPrimaries => ENOSPC,

            GspMsgRmError::InvalidAddress
            | GspMsgRmError::InvalidPointer
            | GspMsgRmError::InvalidRead
            | GspMsgRmError::InvalidWrite
            | GspMsgRmError::ProtectionFault => EFAULT,

            GspMsgRmError::BusyRetry
            | GspMsgRmError::GpuNotFullPower
            | GspMsgRmError::HotSwitch
            | GspMsgRmError::InsufficientPower
            | GspMsgRmError::MoreProcessingRequired => EAGAIN,

            GspMsgRmError::OutOfRange => EOVERFLOW,

            GspMsgRmError::PidNotFound => ESRCH,

            GspMsgRmError::SignalPending => EINTR,

            GspMsgRmError::Timeout | GspMsgRmError::TimeoutRetry => ETIMEDOUT,

            GspMsgRmError::ModuleLoadFailed => ENXIO,

            GspMsgRmError::BrokenFb
            | GspMsgRmError::CycleDetected
            | GspMsgRmError::EccError
            | GspMsgRmError::FatalError
            | GspMsgRmError::FlcnError
            | GspMsgRmError::Generic
            | GspMsgRmError::I2cError
            | GspMsgRmError::IrqEdgeTriggered
            | GspMsgRmError::IrqNotFiring
            | GspMsgRmError::MemoryError
            | GspMsgRmError::MemoryTrainingFailed
            | GspMsgRmError::NvlinkClockError
            | GspMsgRmError::NvlinkConfigurationError
            | GspMsgRmError::NvlinkFabricFailure
            | GspMsgRmError::NvlinkInitError
            | GspMsgRmError::NvlinkMinionError
            | GspMsgRmError::NvlinkTrainingError
            | GspMsgRmError::OperatingSystem
            | GspMsgRmError::RcError
            | GspMsgRmError::ResourceRetirementError
            | GspMsgRmError::RiscvError => EIO,
        }
    }
}

// TODO[FPRI]: This is a temporary solution to be replaced with the corresponding derive macros
// once they land.
impl TryFrom<u32> for GspMsgRmError {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            bindings::NV_ERR_ALREADY_SIGNALLED => Ok(Self::AlreadySignalled),
            bindings::NV_ERR_BROKEN_FB => Ok(Self::BrokenFb),
            bindings::NV_ERR_BUFFER_TOO_SMALL => Ok(Self::BufferTooSmall),
            bindings::NV_ERR_BUSY_RETRY => Ok(Self::BusyRetry),
            bindings::NV_ERR_CALLBACK_NOT_SCHEDULED => Ok(Self::CallbackNotScheduled),
            bindings::NV_ERR_CARD_NOT_PRESENT => Ok(Self::CardNotPresent),
            bindings::NV_ERR_CYCLE_DETECTED => Ok(Self::CycleDetected),
            bindings::NV_ERR_DMA_IN_USE => Ok(Self::DmaInUse),
            bindings::NV_ERR_DMA_MEM_NOT_LOCKED => Ok(Self::DmaMemNotLocked),
            bindings::NV_ERR_DMA_MEM_NOT_UNLOCKED => Ok(Self::DmaMemNotUnlocked),
            bindings::NV_ERR_DUAL_LINK_INUSE => Ok(Self::DualLinkInuse),
            bindings::NV_ERR_ECC_ERROR => Ok(Self::EccError),
            bindings::NV_ERR_FABRIC_MANAGER_NOT_PRESENT => Ok(Self::FabricManagerNotPresent),
            bindings::NV_ERR_FATAL_ERROR => Ok(Self::FatalError),
            bindings::NV_ERR_FEATURE_NOT_ENABLED => Ok(Self::FeatureNotEnabled),
            bindings::NV_ERR_FIFO_BAD_ACCESS => Ok(Self::FifoBadAccess),
            bindings::NV_ERR_FLCN_ERROR => Ok(Self::FlcnError),
            bindings::NV_ERR_FREQ_NOT_SUPPORTED => Ok(Self::FreqNotSupported),
            bindings::NV_ERR_GENERIC => Ok(Self::Generic),
            bindings::NV_ERR_GPU_DMA_NOT_INITIALIZED => Ok(Self::GpuDmaNotInitialized),
            bindings::NV_ERR_GPU_IN_DEBUG_MODE => Ok(Self::GpuInDebugMode),
            bindings::NV_ERR_GPU_IN_FULLCHIP_RESET => Ok(Self::GpuInFullchipReset),
            bindings::NV_ERR_GPU_IS_LOST => Ok(Self::GpuIsLost),
            bindings::NV_ERR_GPU_MEMORY_ONLINING_FAILURE => Ok(Self::GpuMemoryOnliningFailure),
            bindings::NV_ERR_GPU_NOT_FULL_POWER => Ok(Self::GpuNotFullPower),
            bindings::NV_ERR_GPU_UUID_NOT_FOUND => Ok(Self::GpuUuidNotFound),
            bindings::NV_ERR_HOT_SWITCH => Ok(Self::HotSwitch),
            bindings::NV_ERR_I2C_ERROR => Ok(Self::I2cError),
            bindings::NV_ERR_I2C_SPEED_TOO_HIGH => Ok(Self::I2cSpeedTooHigh),
            bindings::NV_ERR_ILLEGAL_ACTION => Ok(Self::IllegalAction),
            bindings::NV_ERR_IN_USE => Ok(Self::InUse),
            bindings::NV_ERR_INFLATE_COMPRESSED_DATA_FAILED => {
                Ok(Self::InflateCompressedDataFailed)
            }
            bindings::NV_ERR_INSERT_DUPLICATE_NAME => Ok(Self::InsertDuplicateName),
            bindings::NV_ERR_INSUFFICIENT_PERMISSIONS => Ok(Self::InsufficientPermissions),
            bindings::NV_ERR_INSUFFICIENT_POWER => Ok(Self::InsufficientPower),
            bindings::NV_ERR_INSUFFICIENT_RESOURCES => Ok(Self::InsufficientResources),
            bindings::NV_ERR_INSUFFICIENT_ZBC_ENTRY => Ok(Self::InsufficientZbcEntry),
            bindings::NV_ERR_INVALID_ACCESS_TYPE => Ok(Self::InvalidAccessType),
            bindings::NV_ERR_INVALID_ADDRESS => Ok(Self::InvalidAddress),
            bindings::NV_ERR_INVALID_ARGUMENT => Ok(Self::InvalidArgument),
            bindings::NV_ERR_INVALID_BASE => Ok(Self::InvalidBase),
            bindings::NV_ERR_INVALID_CHANNEL => Ok(Self::InvalidChannel),
            bindings::NV_ERR_INVALID_CLASS => Ok(Self::InvalidClass),
            bindings::NV_ERR_INVALID_CLIENT => Ok(Self::InvalidClient),
            bindings::NV_ERR_INVALID_COMMAND => Ok(Self::InvalidCommand),
            bindings::NV_ERR_INVALID_DATA => Ok(Self::InvalidData),
            bindings::NV_ERR_INVALID_DEVICE => Ok(Self::InvalidDevice),
            bindings::NV_ERR_INVALID_DMA_SPECIFIER => Ok(Self::InvalidDmaSpecifier),
            bindings::NV_ERR_INVALID_EVENT => Ok(Self::InvalidEvent),
            bindings::NV_ERR_INVALID_FLAGS => Ok(Self::InvalidFlags),
            bindings::NV_ERR_INVALID_FUNCTION => Ok(Self::InvalidFunction),
            bindings::NV_ERR_INVALID_HEAP => Ok(Self::InvalidHeap),
            bindings::NV_ERR_INVALID_INDEX => Ok(Self::InvalidIndex),
            bindings::NV_ERR_INVALID_IRQ_LEVEL => Ok(Self::InvalidIrqLevel),
            bindings::NV_ERR_INVALID_LICENSE => Ok(Self::InvalidLicense),
            bindings::NV_ERR_INVALID_LIMIT => Ok(Self::InvalidLimit),
            bindings::NV_ERR_INVALID_LOCK_STATE => Ok(Self::InvalidLockState),
            bindings::NV_ERR_INVALID_METHOD => Ok(Self::InvalidMethod),
            bindings::NV_ERR_INVALID_OBJECT => Ok(Self::InvalidObject),
            bindings::NV_ERR_INVALID_OBJECT_BUFFER => Ok(Self::InvalidObjectBuffer),
            bindings::NV_ERR_INVALID_OBJECT_HANDLE => Ok(Self::InvalidObjectHandle),
            bindings::NV_ERR_INVALID_OBJECT_NEW => Ok(Self::InvalidObjectNew),
            bindings::NV_ERR_INVALID_OBJECT_OLD => Ok(Self::InvalidObjectOld),
            bindings::NV_ERR_INVALID_OBJECT_PARENT => Ok(Self::InvalidObjectParent),
            bindings::NV_ERR_INVALID_OFFSET => Ok(Self::InvalidOffset),
            bindings::NV_ERR_INVALID_OPERATION => Ok(Self::InvalidOperation),
            bindings::NV_ERR_INVALID_OWNER => Ok(Self::InvalidOwner),
            bindings::NV_ERR_INVALID_PARAM_STRUCT => Ok(Self::InvalidParamStruct),
            bindings::NV_ERR_INVALID_PARAMETER => Ok(Self::InvalidParameter),
            bindings::NV_ERR_INVALID_PATH => Ok(Self::InvalidPath),
            bindings::NV_ERR_INVALID_POINTER => Ok(Self::InvalidPointer),
            bindings::NV_ERR_INVALID_READ => Ok(Self::InvalidRead),
            bindings::NV_ERR_INVALID_REGISTRY_KEY => Ok(Self::InvalidRegistryKey),
            bindings::NV_ERR_INVALID_REQUEST => Ok(Self::InvalidRequest),
            bindings::NV_ERR_INVALID_STATE => Ok(Self::InvalidState),
            bindings::NV_ERR_INVALID_STRING_LENGTH => Ok(Self::InvalidStringLength),
            bindings::NV_ERR_INVALID_WRITE => Ok(Self::InvalidWrite),
            bindings::NV_ERR_INVALID_XLATE => Ok(Self::InvalidXlate),
            bindings::NV_ERR_IRQ_EDGE_TRIGGERED => Ok(Self::IrqEdgeTriggered),
            bindings::NV_ERR_IRQ_NOT_FIRING => Ok(Self::IrqNotFiring),
            bindings::NV_ERR_KEY_ROTATION_IN_PROGRESS => Ok(Self::KeyRotationInProgress),
            bindings::NV_ERR_LIB_RM_VERSION_MISMATCH => Ok(Self::LibRmVersionMismatch),
            bindings::NV_ERR_MAX_SESSION_LIMIT_REACHED => Ok(Self::MaxSessionLimitReached),
            bindings::NV_ERR_MEMORY_ERROR => Ok(Self::MemoryError),
            bindings::NV_ERR_MEMORY_TRAINING_FAILED => Ok(Self::MemoryTrainingFailed),
            bindings::NV_ERR_MISMATCHED_SLAVE => Ok(Self::MismatchedSlave),
            bindings::NV_ERR_MISMATCHED_TARGET => Ok(Self::MismatchedTarget),
            bindings::NV_ERR_MISSING_TABLE_ENTRY => Ok(Self::MissingTableEntry),
            bindings::NV_ERR_MODULE_LOAD_FAILED => Ok(Self::ModuleLoadFailed),
            bindings::NV_ERR_MORE_DATA_AVAILABLE => Ok(Self::MoreDataAvailable),
            bindings::NV_ERR_MORE_PROCESSING_REQUIRED => Ok(Self::MoreProcessingRequired),
            bindings::NV_ERR_MULTIPLE_MEMORY_TYPES => Ok(Self::MultipleMemoryTypes),
            bindings::NV_ERR_NO_FREE_FIFOS => Ok(Self::NoFreeFifos),
            bindings::NV_ERR_NO_INTR_PENDING => Ok(Self::NoIntrPending),
            bindings::NV_ERR_NO_MEMORY => Ok(Self::NoMemory),
            bindings::NV_ERR_NO_SUCH_DOMAIN => Ok(Self::NoSuchDomain),
            bindings::NV_ERR_NO_VALID_PATH => Ok(Self::NoValidPath),
            bindings::NV_ERR_NOT_COMPATIBLE => Ok(Self::NotCompatible),
            bindings::NV_ERR_NOT_READY => Ok(Self::NotReady),
            bindings::NV_ERR_NOT_SUPPORTED => Ok(Self::NotSupported),
            bindings::NV_ERR_NVLINK_CLOCK_ERROR => Ok(Self::NvlinkClockError),
            bindings::NV_ERR_NVLINK_CONFIGURATION_ERROR => Ok(Self::NvlinkConfigurationError),
            bindings::NV_ERR_NVLINK_FABRIC_FAILURE => Ok(Self::NvlinkFabricFailure),
            bindings::NV_ERR_NVLINK_FABRIC_NOT_READY => Ok(Self::NvlinkFabricNotReady),
            bindings::NV_ERR_NVLINK_INIT_ERROR => Ok(Self::NvlinkInitError),
            bindings::NV_ERR_NVLINK_MINION_ERROR => Ok(Self::NvlinkMinionError),
            bindings::NV_ERR_NVLINK_TRAINING_ERROR => Ok(Self::NvlinkTrainingError),
            bindings::NV_ERR_OBJECT_NOT_FOUND => Ok(Self::ObjectNotFound),
            bindings::NV_ERR_OBJECT_TYPE_MISMATCH => Ok(Self::ObjectTypeMismatch),
            bindings::NV_ERR_OPERATING_SYSTEM => Ok(Self::OperatingSystem),
            bindings::NV_ERR_OTHER_DEVICE_FOUND => Ok(Self::OtherDeviceFound),
            bindings::NV_ERR_OUT_OF_RANGE => Ok(Self::OutOfRange),
            bindings::NV_ERR_OVERLAPPING_UVM_COMMIT => Ok(Self::OverlappingUvmCommit),
            bindings::NV_ERR_PAGE_TABLE_NOT_AVAIL => Ok(Self::PageTableNotAvail),
            bindings::NV_ERR_PID_NOT_FOUND => Ok(Self::PidNotFound),
            bindings::NV_ERR_PMU_NOT_READY => Ok(Self::PmuNotReady),
            bindings::NV_ERR_PRIV_SEC_VIOLATION => Ok(Self::PrivSecViolation),
            bindings::NV_ERR_PROTECTION_FAULT => Ok(Self::ProtectionFault),
            bindings::NV_ERR_QUEUE_TASK_SLOT_NOT_AVAILABLE => Ok(Self::QueueTaskSlotNotAvailable),
            bindings::NV_ERR_RC_ERROR => Ok(Self::RcError),
            bindings::NV_ERR_REDUCTION_MANAGER_NOT_AVAILABLE => {
                Ok(Self::ReductionManagerNotAvailable)
            }
            bindings::NV_ERR_REJECTED_VBIOS => Ok(Self::RejectedVbios),
            bindings::NV_ERR_RESET_REQUIRED => Ok(Self::ResetRequired),
            bindings::NV_ERR_RESOURCE_LOST => Ok(Self::ResourceLost),
            bindings::NV_ERR_RESOURCE_RETIREMENT_ERROR => Ok(Self::ResourceRetirementError),
            bindings::NV_ERR_RISCV_ERROR => Ok(Self::RiscvError),
            bindings::NV_ERR_SECURE_BOOT_FAILED => Ok(Self::SecureBootFailed),
            bindings::NV_ERR_SIGNAL_PENDING => Ok(Self::SignalPending),
            bindings::NV_ERR_STATE_IN_USE => Ok(Self::StateInUse),
            bindings::NV_ERR_TEST_ONLY_CODE_NOT_ENABLED => Ok(Self::TestOnlyCodeNotEnabled),
            bindings::NV_ERR_TIMEOUT => Ok(Self::Timeout),
            bindings::NV_ERR_TIMEOUT_RETRY => Ok(Self::TimeoutRetry),
            bindings::NV_ERR_TOO_MANY_PRIMARIES => Ok(Self::TooManyPrimaries),
            bindings::NV_ERR_UVM_ADDRESS_IN_USE => Ok(Self::UvmAddressInUse),
            _ => Err(EINVAL),
        }
    }
}

impl TryFrom<u32> for GspMsgRmStatus {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self> {
        if value == bindings::NV_OK {
            return Ok(Self::Ok);
        }

        if let Ok(warning) = GspMsgRmWarning::try_from(value) {
            return Ok(Self::Warning(warning));
        }

        Ok(Self::Error(GspMsgRmError::try_from(value)?))
    }
}

impl GspMsgRmStatus {
    /// Converts [`GspMsgRmStatus`] to a [`Result`], logging if the status is a warning.
    ///
    /// `rpc_name` identifies the RPC for the log message.
    pub(super) fn log_if_warning(
        self,
        dev: &device::Device,
        rpc_name: impl fmt::Debug,
    ) -> Result<(), GspMsgRmError> {
        match self {
            Self::Ok => Ok(()),
            Self::Warning(warning) => {
                dev_warn!(
                    dev,
                    "GSP RPC {:?} returned warning {:?}\n",
                    rpc_name,
                    warning
                );
                Ok(())
            }
            Self::Error(status) => Err(status),
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

    /// Returns the RPC status from the message header.
    pub(super) fn status(&self) -> Result<GspMsgRmStatus> {
        self.inner.rpc.rpc_result.try_into()
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
