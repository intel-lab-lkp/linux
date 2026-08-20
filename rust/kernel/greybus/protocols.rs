// SPDX-License-Identifier: GPL-2.0

//! Greybus wire format definitions.
//!
//! Thin `repr(transparent)` wrappers over the generated bindings for the operation header and the
//! SVC protocol messages, plus the constants that go in their type and result fields. The
//! wrappers exist to keep the byte-order conversions in one place: constructors take native-endian
//! values and store little-endian, accessors convert back.

/// Set in the header type field to mark a message as a response to the operation of the same id.
pub const MESSAGE_TYPE_RESPONSE: u8 = 0x80;

/// The CPort id reserved for the SVC connection on every host device.
pub const GB_SVC_CPORT_ID: u16 = bindings::GB_SVC_CPORT_ID as u16;

/// Defines `u8` constants from same-named `bindings` values.
macro_rules! gb_u8_consts {
    ($($name:ident),* $(,)?) => {
        $(
            #[allow(missing_docs)]
            pub const $name: u8 = bindings::$name as u8;
        )*
    };
}

// SVC Operation Types
gb_u8_consts! {
    GB_SVC_TYPE_PROTOCOL_VERSION,
    GB_SVC_TYPE_SVC_HELLO,
    GB_SVC_TYPE_INTF_DEVICE_ID,
    GB_SVC_TYPE_INTF_RESET,
    GB_SVC_TYPE_CONN_CREATE,
    GB_SVC_TYPE_CONN_DESTROY,
    GB_SVC_TYPE_DME_PEER_GET,
    GB_SVC_TYPE_DME_PEER_SET,
    GB_SVC_TYPE_ROUTE_CREATE,
    GB_SVC_TYPE_ROUTE_DESTROY,
    GB_SVC_TYPE_TIMESYNC_ENABLE,
    GB_SVC_TYPE_TIMESYNC_DISABLE,
    GB_SVC_TYPE_TIMESYNC_AUTHORITATIVE,
    GB_SVC_TYPE_INTF_SET_PWRM,
    GB_SVC_TYPE_INTF_EJECT,
    GB_SVC_TYPE_PING,
    GB_SVC_TYPE_PWRMON_RAIL_COUNT_GET,
    GB_SVC_TYPE_PWRMON_RAIL_NAMES_GET,
    GB_SVC_TYPE_PWRMON_SAMPLE_GET,
    GB_SVC_TYPE_PWRMON_INTF_SAMPLE_GET,
    GB_SVC_TYPE_TIMESYNC_WAKE_PINS_ACQUIRE,
    GB_SVC_TYPE_TIMESYNC_WAKE_PINS_RELEASE,
    GB_SVC_TYPE_TIMESYNC_PING,
    GB_SVC_TYPE_MODULE_INSERTED,
    GB_SVC_TYPE_MODULE_REMOVED,
    GB_SVC_TYPE_INTF_VSYS_ENABLE,
    GB_SVC_TYPE_INTF_VSYS_DISABLE,
    GB_SVC_TYPE_INTF_REFCLK_ENABLE,
    GB_SVC_TYPE_INTF_REFCLK_DISABLE,
    GB_SVC_TYPE_INTF_UNIPRO_ENABLE,
    GB_SVC_TYPE_INTF_UNIPRO_DISABLE,
    GB_SVC_TYPE_INTF_ACTIVATE,
    GB_SVC_TYPE_INTF_RESUME,
    GB_SVC_TYPE_INTF_MAILBOX_EVENT,
    GB_SVC_TYPE_INTF_OOPS,
}

// UNIPRO modes
gb_u8_consts! {
    GB_SVC_UNIPRO_FAST_MODE,
    GB_SVC_UNIPRO_SLOW_MODE,
    GB_SVC_UNIPRO_FAST_AUTO_MODE,
    GB_SVC_UNIPRO_SLOW_AUTO_MODE,
    GB_SVC_UNIPRO_MODE_UNCHANGED,
    GB_SVC_UNIPRO_HIBERNATE_MODE,
    GB_SVC_UNIPRO_OFF_MODE,
}

// PWR States
gb_u8_consts! {
    GB_SVC_SETPWRM_PWR_OK,
    GB_SVC_SETPWRM_PWR_LOCAL,
    GB_SVC_SETPWRM_PWR_REMOTE,
    GB_SVC_SETPWRM_PWR_BUSY,
    GB_SVC_SETPWRM_PWR_ERROR_CAP,
    GB_SVC_SETPWRM_PWR_FATAL_ERROR,
}

// Vsys Result
gb_u8_consts! {
    GB_SVC_INTF_VSYS_OK,
    GB_SVC_INTF_VSYS_FAIL,
}

// Refclk Result
gb_u8_consts! {
    GB_SVC_INTF_REFCLK_OK,
    GB_SVC_INTF_REFCLK_FAIL,
}

// Unipro Result
gb_u8_consts! {
    GB_SVC_INTF_UNIPRO_OK,
    GB_SVC_INTF_UNIPRO_FAIL,
    GB_SVC_INTF_UNIPRO_NOT_OFF,
}

// Op Codes
gb_u8_consts! {
    GB_SVC_OP_SUCCESS,
    GB_SVC_OP_UNKNOWN_ERROR,
    GB_SVC_INTF_NOT_DETECTED,
    GB_SVC_INTF_NO_UPRO_LINK,
    GB_SVC_INTF_UPRO_NOT_DOWN,
    GB_SVC_INTF_UPRO_NOT_HIBERNATED,
    GB_SVC_INTF_NO_V_SYS,
    GB_SVC_INTF_V_CHG,
    GB_SVC_INTF_WAKE_BUSY,
    GB_SVC_INTF_NO_REFCLK,
    GB_SVC_INTF_RELEASING,
    GB_SVC_INTF_NO_ORDER,
    GB_SVC_INTF_MBOX_SET,
    GB_SVC_INTF_BAD_MBOX,
    GB_SVC_INTF_OP_TIMEOUT,
    GB_SVC_PWRMON_OP_NOT_PRESENT,
}

// Greybus Interface Types
gb_u8_consts! {
    GB_SVC_INTF_TYPE_UNKNOWN,
    GB_SVC_INTF_TYPE_DUMMY,
    GB_SVC_INTF_TYPE_UNIPRO,
    GB_SVC_INTF_TYPE_GREYBUS,
}

/// The header every Greybus message starts with.
///
/// # Invariants
///
/// The `size` field covers the header and the payload that follows it.
#[repr(transparent)]
pub struct GbOperationMsgHdr(bindings::gb_operation_msg_hdr);

// SAFETY: `gb_operation_msg_hdr` is a POD type with no padding and no interior mutability.
unsafe impl kernel::transmute::AsBytes for GbOperationMsgHdr {}

impl GbOperationMsgHdr {
    /// Builds a header. `size` is the whole message, this header included.
    #[inline]
    pub const fn new(size: u16, operation_id: u16, type_: u8, result: u8) -> Self {
        Self(bindings::gb_operation_msg_hdr {
            size: size.to_le(),
            operation_id: operation_id.to_le(),
            type_,
            result,
            pad: [0u8; 2],
        })
    }

    /// Returns the type field, response bit included.
    #[inline]
    pub const fn msg_type(&self) -> u8 {
        self.0.type_
    }

    /// Returns whether this is a response rather than a request.
    #[inline]
    pub const fn is_response(&self) -> bool {
        self.0.type_ & MESSAGE_TYPE_RESPONSE != 0
    }

    /// Returns the operation id pairing a response with its request. Zero for unidirectional
    /// messages.
    #[inline]
    pub const fn operation_id(&self) -> u16 {
        u16::from_le(self.0.operation_id)
    }

    /// Returns the type field with the response bit cleared.
    #[inline]
    pub const fn request_type(&self) -> u8 {
        self.msg_type() & !MESSAGE_TYPE_RESPONSE
    }

    /// Returns the whole message size, this header included.
    #[inline]
    pub const fn size(&self) -> u16 {
        u16::from_le(self.0.size)
    }
}

/// Request for [`GB_SVC_TYPE_PROTOCOL_VERSION`].
#[repr(transparent)]
pub struct GbSvcVersionRequest(bindings::gb_svc_version_request);

impl GbSvcVersionRequest {
    /// Creates a request advertising SVC protocol version `major`.`minor`.
    #[inline]
    pub const fn new(major: u8, minor: u8) -> Self {
        Self(bindings::gb_svc_version_request { major, minor })
    }
}

/// Request for [`GB_SVC_TYPE_SVC_HELLO`].
#[repr(transparent)]
pub struct GbSvcHelloRequest(bindings::gb_svc_hello_request);

impl GbSvcHelloRequest {
    /// Creates a hello request identifying the endo as `endo_id` and the AP's own interface as
    /// `interface_id`.
    #[inline]
    pub const fn new(endo_id: u16, interface_id: u8) -> Self {
        Self(bindings::gb_svc_hello_request {
            endo_id: endo_id.to_le(),
            interface_id,
        })
    }
}

/// Request for [`GB_SVC_TYPE_INTF_SET_PWRM`].
#[repr(transparent)]
pub struct GbSvcIntfSetPwrmRequest(bindings::gb_svc_intf_set_pwrm_request);

// SAFETY: `gb_svc_intf_set_pwrm_request` is a struct of `u8` fields, so every bit pattern of its
// size is a valid instance.
unsafe impl kernel::transmute::FromBytes for GbSvcIntfSetPwrmRequest {}

impl GbSvcIntfSetPwrmRequest {
    /// Returns the requested TX gear, one of the `GB_SVC_UNIPRO_*` modes.
    #[inline]
    pub const fn tx_mode(&self) -> u8 {
        self.0.tx_mode
    }

    /// Returns the requested RX gear, one of the `GB_SVC_UNIPRO_*` modes.
    #[inline]
    pub const fn rx_mode(&self) -> u8 {
        self.0.rx_mode
    }
}

/// Response to [`GB_SVC_TYPE_INTF_SET_PWRM`].
#[repr(transparent)]
pub struct GbSvcIntfSetPwrmResponse(bindings::gb_svc_intf_set_pwrm_response);

impl GbSvcIntfSetPwrmResponse {
    /// `result_code` is one of the `GB_SVC_SETPWRM_PWR_*` codes.
    #[inline]
    pub const fn new(result_code: u8) -> Self {
        Self(bindings::gb_svc_intf_set_pwrm_response { result_code })
    }
}

/// Response to [`GB_SVC_TYPE_DME_PEER_GET`].
#[repr(transparent)]
pub struct GbSvcDmePeerGetResponse(bindings::gb_svc_dme_peer_get_response);

impl GbSvcDmePeerGetResponse {
    /// `result_code` is the UniPro `ConfigResultCode`; `attr_value` is the UniPro attribute
    /// value.
    #[inline]
    pub const fn new(result_code: u16, attr_value: u32) -> Self {
        Self(bindings::gb_svc_dme_peer_get_response {
            result_code: result_code.to_le(),
            attr_value: attr_value.to_le(),
        })
    }
}

/// Response to [`GB_SVC_TYPE_DME_PEER_SET`].
#[repr(transparent)]
pub struct GbSvcDmePeerSetResponse(bindings::gb_svc_dme_peer_set_response);

impl GbSvcDmePeerSetResponse {
    /// `result_code` is the UniPro `ConfigResultCode`.
    #[inline]
    pub const fn new(result_code: u16) -> Self {
        Self(bindings::gb_svc_dme_peer_set_response {
            result_code: result_code.to_le(),
        })
    }
}

/// Response to [`GB_SVC_TYPE_PWRMON_RAIL_COUNT_GET`].
#[repr(transparent)]
pub struct GbSvcPwrmonRailCountGetResponse(bindings::gb_svc_pwrmon_rail_count_get_response);

impl GbSvcPwrmonRailCountGetResponse {
    /// Creates a response reporting `rail_count` available rails.
    #[inline]
    pub const fn new(rail_count: u8) -> Self {
        Self(bindings::gb_svc_pwrmon_rail_count_get_response { rail_count })
    }
}

/// Response to [`GB_SVC_TYPE_INTF_VSYS_ENABLE`] and [`GB_SVC_TYPE_INTF_VSYS_DISABLE`].
#[repr(transparent)]
pub struct GbSvcIntfVsysResponse(bindings::gb_svc_intf_vsys_response);

impl GbSvcIntfVsysResponse {
    /// `result_code` is [`GB_SVC_INTF_VSYS_OK`] or [`GB_SVC_INTF_VSYS_FAIL`].
    #[inline]
    pub const fn new(result_code: u8) -> Self {
        Self(bindings::gb_svc_intf_vsys_response { result_code })
    }
}

/// Response to [`GB_SVC_TYPE_INTF_REFCLK_ENABLE`] and [`GB_SVC_TYPE_INTF_REFCLK_DISABLE`].
#[repr(transparent)]
pub struct GbSvcIntfRefclkResponse(bindings::gb_svc_intf_refclk_response);

impl GbSvcIntfRefclkResponse {
    /// `result_code` is [`GB_SVC_INTF_REFCLK_OK`] or [`GB_SVC_INTF_REFCLK_FAIL`].
    #[inline]
    pub const fn new(result_code: u8) -> Self {
        Self(bindings::gb_svc_intf_refclk_response { result_code })
    }
}

/// Response to [`GB_SVC_TYPE_INTF_UNIPRO_ENABLE`] and [`GB_SVC_TYPE_INTF_UNIPRO_DISABLE`].
#[repr(transparent)]
pub struct GbSvcIntfUniproResponse(bindings::gb_svc_intf_unipro_response);

impl GbSvcIntfUniproResponse {
    /// `result_code` is one of the `GB_SVC_INTF_UNIPRO_*` codes.
    #[inline]
    pub const fn new(result_code: u8) -> Self {
        Self(bindings::gb_svc_intf_unipro_response { result_code })
    }
}

/// Response to [`GB_SVC_TYPE_INTF_ACTIVATE`].
#[repr(transparent)]
pub struct GbSvcIntfActivateResponse(bindings::gb_svc_intf_activate_response);

impl GbSvcIntfActivateResponse {
    /// `status` is one of the `GB_SVC_OP_*` codes; `intf_type` is one of the
    /// `GB_SVC_INTF_TYPE_*` values and is only meaningful when `status` is
    /// [`GB_SVC_OP_SUCCESS`].
    #[inline]
    pub const fn new(status: u8, intf_type: u8) -> Self {
        Self(bindings::gb_svc_intf_activate_response { status, intf_type })
    }
}

/// Response to [`GB_SVC_TYPE_INTF_RESUME`].
#[repr(transparent)]
pub struct GbSvcIntfResumeResponse(bindings::gb_svc_intf_resume_response);

impl GbSvcIntfResumeResponse {
    /// `status` is one of the `GB_SVC_OP_*` codes.
    #[inline]
    pub const fn new(status: u8) -> Self {
        Self(bindings::gb_svc_intf_resume_response { status })
    }
}

/// Request for [`GB_SVC_TYPE_MODULE_INSERTED`].
#[repr(transparent)]
pub struct GbSvcModuleInsertedRequest(bindings::gb_svc_module_inserted_request);

impl GbSvcModuleInsertedRequest {
    /// The module spans `intf_count` consecutive interfaces starting at `primary_intf_id`.
    /// `flags` is a mask of `GB_SVC_MODULE_INSERTED_FLAG_*` values.
    #[inline]
    pub const fn new(primary_intf_id: u8, intf_count: u8, flags: u16) -> Self {
        Self(bindings::gb_svc_module_inserted_request {
            primary_intf_id,
            intf_count,
            flags: flags.to_le(),
        })
    }
}

/// Request for [`GB_SVC_TYPE_MODULE_REMOVED`].
#[repr(transparent)]
pub struct GbSvcModuleRemovedRequest(bindings::gb_svc_module_removed_request);

// SAFETY: `gb_svc_module_removed_request` is a struct of `u8` field, so every bit pattern of its
// size is a valid instance.
unsafe impl kernel::transmute::FromBytes for GbSvcModuleRemovedRequest {}

impl GbSvcModuleRemovedRequest {
    /// `primary_intf_id` identifies the module, and matches the one given when it was inserted.
    #[inline]
    pub const fn new(primary_intf_id: u8) -> Self {
        Self(bindings::gb_svc_module_removed_request { primary_intf_id })
    }

    /// Returns the primary_intf_id field.
    #[inline]
    pub const fn primary_intf_id(&self) -> u8 {
        self.0.primary_intf_id
    }
}
