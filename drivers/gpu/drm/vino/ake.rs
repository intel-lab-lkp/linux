// SPDX-License-Identifier: GPL-2.0

//! HDCP 2.2 AKE wire layer (sec 5.1 OUT framing, sec 5.2 IN parsing) -- the byte-exact
//! message builders the AKE state machine drives, mirroring the verified userspace
//! oracle (`vino-driver::hdcp_msgs`). DLM hardcodes per-message `sub_size` /
//! `sub_len_dw` values the dock validates, so they are reproduced verbatim rather
//! than derived.
//!
//! OUT body layout (sec 5.1), after the 16-byte sec 3 transport header:
//! ```text
//!   body[0..2]   u16 sub_size      (DLM-fixed per message)
//!   body[2..4]   u16 = 0x0010
//!   body[4..8]   u32 hdcp_seq      increments 1..7 across the AKE OUT messages
//!   body[8..22]  14 zero bytes
//!   body[22..26] u32 = 0x00000030  marker
//!   body[26]     u8  = 0x00        flag
//!   body[27]     u8  = msg_id
//!   body[28..]   HDCP payload (zero-padded to the fixed body length)
//! ```
#![allow(dead_code)] // AKE message builders; response handlers run only after CP engagement

use super::*;

/// HDCP 2.2 message IDs (sec 5.3). `pub(crate)` so the AKE state machine
/// ([`super::VinoDriver::run_ake`]) can match on the response IDs too.
pub(crate) mod id {
    use kernel::bindings;

    // Standard HDCP 2.2 message IDs: reuse the canonical values from
    // `<drm/display/drm_hdcp.h>` rather than redefining them, so vino stays in
    // lockstep with the kernel's HDCP definitions. Only the transport framing
    // around these (the DisplayLink type/sub/ctr header) is vino-specific.
    pub(crate) const AKE_INIT: u8 = bindings::HDCP_2_2_AKE_INIT as u8;
    pub(crate) const AKE_SEND_CERT: u8 = bindings::HDCP_2_2_AKE_SEND_CERT as u8;
    pub(crate) const AKE_NO_STORED_KM: u8 = bindings::HDCP_2_2_AKE_NO_STORED_KM as u8;
    pub(crate) const AKE_SEND_H_PRIME: u8 = bindings::HDCP_2_2_AKE_SEND_HPRIME as u8;
    pub(crate) const AKE_SEND_PAIRING_INFO: u8 = bindings::HDCP_2_2_AKE_SEND_PAIRING_INFO as u8;
    pub(crate) const LC_INIT: u8 = bindings::HDCP_2_2_LC_INIT as u8;
    pub(crate) const LC_SEND_L_PRIME: u8 = bindings::HDCP_2_2_LC_SEND_LPRIME as u8;
    pub(crate) const SKE_SEND_EKS: u8 = bindings::HDCP_2_2_SKE_SEND_EKS as u8;
    pub(crate) const REPEATERAUTH_SEND_RECEIVERID_LIST: u8 =
        bindings::HDCP_2_2_REP_SEND_RECVID_LIST as u8;
    pub(crate) const REPEATERAUTH_SEND_ACK: u8 = bindings::HDCP_2_2_REP_SEND_ACK as u8;
    pub(crate) const REPEATERAUTH_STREAM_MANAGE: u8 = bindings::HDCP_2_2_REP_STREAM_MANAGE as u8;
    pub(crate) const REPEATERAUTH_STREAM_READY: u8 = bindings::HDCP_2_2_REP_STREAM_READY as u8;

    // DisplayLink-specific message IDs with no `<drm/display/drm_hdcp.h>` equivalent
    // (the AKE_Send_rrx split and the transmitter/receiver-info + auth-status messages
    // the DL3 dock uses), kept as literals.
    pub(crate) const AKE_SEND_RRX: u8 = 0x06;
    pub(crate) const RECEIVER_AUTH_STATUS: u8 = 0x12;
    pub(crate) const AKE_TRANSMITTER_INFO: u8 = 0x13;
    pub(crate) const AKE_RECEIVER_INFO: u8 = 0x14;
}

/// transport `sub_id` for HDCP OUT messages (type=4 sub=0x04, sec 5.1).
const SUB_HDCP: u16 = 0x04;

/// Allocate a `body_len`-byte zeroed body with the sec 5.1 header filled in
/// (`sub_size`, the `0x0010` marker, `hdcp_seq`, the `0x30` marker and `msg_id`).
/// The caller writes the payload into `body[28..]`.
fn body(body_len: usize, sub_size: u16, hdcp_seq: u32, msg_id: u8) -> Result<KVec<u8>> {
    let mut b = KVec::from_elem(0u8, body_len, GFP_KERNEL)?;
    b[0..2].copy_from_slice(&sub_size.to_le_bytes());
    b[2..4].copy_from_slice(&0x0010u16.to_le_bytes());
    b[4..8].copy_from_slice(&hdcp_seq.to_le_bytes());
    b[22..26].copy_from_slice(&0x0000_0030u32.to_le_bytes());
    b[27] = msg_id;
    Ok(b)
}

/// Wrap a finished HDCP body in the sec 3 transport header (type=4 sub=0x04) with
/// the DLM-fixed `sub_len_dw` and the transport `seq`.
fn wrap(sub_len_dw: u16, seq: u32, body: &[u8]) -> Result<KVec<u8>> {
    let mut frame = KVec::with_capacity(16 + body.len(), GFP_KERNEL)?;
    proto::push_frame_with(&mut frame, 0x04, SUB_HDCP, sub_len_dw, seq, body)?;
    Ok(frame)
}

/// `AKE_Init` (msg_id 0x02): `rtx[8] || TxCaps[3]`, padded to a 48-byte body
/// (`sub_size=0x22`, `sub_len_dw=0x0c` -- guide sec 5.4 table).
pub(super) fn ake_init(
    hdcp_seq: u32,
    seq: u32,
    rtx: &[u8; 8],
    tx_caps: &[u8; 3],
) -> Result<KVec<u8>> {
    let mut b = body(48, 0x0022, hdcp_seq, id::AKE_INIT)?;
    b[28..36].copy_from_slice(rtx);
    b[36..39].copy_from_slice(tx_caps);
    wrap(0x000c, seq, &b)
}

/// `AKE_Transmitter_Info` (msg_id 0x13): byte-exact DLM framing
/// (`sub_size=0x1f`, `sub_len_dw=0x0f`), payload `00 06 02 00 02`.
pub(super) fn ake_transmitter_info(hdcp_seq: u32, seq: u32) -> Result<KVec<u8>> {
    let mut b = body(48, 0x001f, hdcp_seq, id::AKE_TRANSMITTER_INFO)?;
    b[28..33].copy_from_slice(&[0x00, 0x06, 0x02, 0x00, 0x02]);
    wrap(0x000f, seq, &b)
}

/// `AKE_No_Stored_km` (msg_id 0x04): the 128-byte RSA-OAEP-SHA256 `Ekpub(km)`
/// in a 160-byte body (`sub_size=0x9a`, `sub_len_dw=0x04` -- guide sec 5.4 table).
pub(super) fn ake_no_stored_km(
    hdcp_seq: u32,
    seq: u32,
    ekpub_km: &[u8; 128],
) -> Result<KVec<u8>> {
    let mut b = body(160, 0x009a, hdcp_seq, id::AKE_NO_STORED_KM)?;
    b[28..156].copy_from_slice(ekpub_km);
    wrap(0x0004, seq, &b)
}

/// `LC_Init` (msg_id 0x09): `rn[8]` in a 48-byte body
/// (`sub_size=0x22`, `sub_len_dw=0x0c`).
pub(super) fn lc_init(hdcp_seq: u32, seq: u32, rn: &[u8; 8]) -> Result<KVec<u8>> {
    let mut b = body(48, 0x0022, hdcp_seq, id::LC_INIT)?;
    b[28..36].copy_from_slice(rn);
    wrap(0x000c, seq, &b)
}

/// `SKE_Send_Eks` (msg_id 0x0b): `Edkey(ks)[16] || riv[8]` in a 64-byte body
/// (`sub_size=0x32`, `sub_len_dw=0x0c`).
pub(super) fn ske_send_eks(
    hdcp_seq: u32,
    seq: u32,
    edkey_ks: &[u8; 16],
    riv: &[u8; 8],
) -> Result<KVec<u8>> {
    let mut b = body(64, 0x0032, hdcp_seq, id::SKE_SEND_EKS)?;
    b[28..44].copy_from_slice(edkey_ks);
    b[44..52].copy_from_slice(riv);
    wrap(0x000c, seq, &b)
}

/// `RepeaterAuth_Send_ACK` (msg_id 0x0f): the full `V[16]` in a 48-byte body
/// (`sub_size=0x2a`, `sub_len_dw=0x04`).
pub(super) fn repeater_auth_send_ack(
    hdcp_seq: u32,
    seq: u32,
    v: &[u8; 16],
) -> Result<KVec<u8>> {
    let mut b = body(48, 0x002a, hdcp_seq, id::REPEATERAUTH_SEND_ACK)?;
    b[28..44].copy_from_slice(v);
    wrap(0x0004, seq, &b)
}

/// `RepeaterAuth_Stream_Manage` SM2 (msg_id 0x10): byte-exact DLM replica sent
/// after Send_ACK -- `k=2` (LE), `StreamID_Type[0]=4` (LE), `body[43]=0x05`
/// (`sub_size=0x2d`, `sub_len_dw=0x01`). See guide sec 5.4 and sec 8.2.
pub(super) fn repeater_auth_stream_manage(hdcp_seq: u32, seq: u32) -> Result<KVec<u8>> {
    let mut b = body(48, 0x002d, hdcp_seq, id::REPEATERAUTH_STREAM_MANAGE)?;
    b[32..36].copy_from_slice(&[0x02, 0, 0, 0]); // k = 2 (LE)
    b[36..40].copy_from_slice(&[0x04, 0, 0, 0]); // StreamID_Type[0] = 4 (LE)
    b[43] = 0x05;
    wrap(0x0001, seq, &b)
}

/// Parse an IN HDCP message body (sec 5.2): `body[8]` marker, `body[9]` msg_id,
/// `body[10..]` payload (for `AKE_Send_Cert`, `body[10]` is a version flag).
/// Returns `(msg_id, payload)`.
pub(super) fn parse_in(body: &[u8]) -> Option<(u8, &[u8])> {
    if body.len() < 10 {
        return None;
    }
    Some((body[9], &body[10..]))
}
