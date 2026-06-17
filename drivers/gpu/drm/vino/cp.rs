// SPDX-License-Identifier: GPL-2.0

//! Encrypted-control-plane message builders (the inner plaintext of the type=4
//! sub=0x24 AES-CTR frames) plus the AES-CTR `seal` that encrypts and frames them.
//! Layouts are from the reverse-engineered protocol; offsets cite the guide and
//! should be re-checked against a capture before they drive real hardware.
#![allow(dead_code)] // some seal/handler paths run only after the dock engages CP (open blocker)

use super::*;

/// Common CP inner header: `[id u16][sub u16][counter u16][00 00]` (sec 6.1/sec 8.6.4).
fn header(out: &mut KVec<u8>, id: u16, sub: u16, counter: u16) -> Result {
    out.extend_from_slice(&id.to_le_bytes(), GFP_KERNEL)?;
    out.extend_from_slice(&sub.to_le_bytes(), GFP_KERNEL)?;
    out.extend_from_slice(&counter.to_le_bytes(), GFP_KERNEL)?;
    out.extend_from_slice(&[0, 0], GFP_KERNEL)?;
    Ok(())
}

fn pad_to(out: &mut KVec<u8>, len: usize) -> Result {
    while out.len() < len {
        out.push(0, GFP_KERNEL)?;
    }
    Ok(())
}

/// OUT heartbeat (sec 6.1): `id=0x16 sub=0x75`, two AES blocks (`10 27` at block1+6).
pub(super) fn heartbeat(counter: u16) -> Result<KVec<u8>> {
    let mut b = KVec::with_capacity(32, GFP_KERNEL)?;
    header(&mut b, 0x16, 0x75, counter)?;
    pad_to(&mut b, 22)?; // block0 tail + block1[0..6]
    b.extend_from_slice(&[0x10, 0x27], GFP_KERNEL)?; // block1[6..8]
    pad_to(&mut b, 32)?;
    Ok(b)
}

/// OUT get-EDID request (CP-HANDSHAKE.md sec 4f): `id=0x15 sub=0x21`, the message that asks
/// the dock to return the downstream monitor's EDID in an `id=0x194 sub=0x21` reply (parsed
/// by [`parse_edid_from_reply`]). The request carries no payload beyond the inner header, so
/// it is a single 16-byte AES block; [`seal_livemac`] appends the 16-byte Dl3Cmac. The dock
/// echoes the `counter`, so any monotonic value works. The exact request body was never
/// captured (only the reply), so this is the minimal well-formed form -- re-check against a
/// capture if the dock ever NAKs it once CP engages.
pub(super) fn get_edid_req(counter: u16) -> Result<KVec<u8>> {
    let mut b = KVec::with_capacity(16, GFP_KERNEL)?;
    header(&mut b, 0x15, 0x21, counter)?;
    pad_to(&mut b, 16)?;
    Ok(b)
}

/// A video timing in DisplayID-Type-I terms (sec 8.6.4), as carried by the
/// `0x48/0x22` set-mode message. Field meanings and offsets are verified
/// byte-exact against the golden 3840x2160@60 capture (see [`set_mode`]).
#[derive(Clone, Copy)]
pub(super) struct Timing {
    pub hactive: u16,
    pub hblank: u16,
    pub hsync_front: u16,
    pub hsync_width: u16,
    pub vactive: u16,
    pub vblank: u16,
    pub vsync_front: u16,
    pub vsync_width: u16,
    pub refresh_hz: u16,
    /// Pixel clock in 10 kHz units (e.g. 0xd040 = 533.12 MHz for 4K@60).
    pub pixel_clock_10khz: u16,
    /// DisplayID field at off42 -- partly decoded (0x0604 for 4K, 0x0600 for the
    /// 2560x1440 sample in sec 8.6.4); high byte 0x06 constant, low byte mode-varying.
    pub field42: u16,
}

impl Timing {
    /// 3840x2160@60 (CVT-RB) -- the mode the non-HDCP dongle advertises, kept as a
    /// known-good reference whose `set_mode` output is byte-exact vs the golden capture.
    pub(super) const UHD_60: Timing = Timing {
        hactive: 3840, hblank: 160, hsync_front: 48, hsync_width: 32,
        vactive: 2160, vblank: 62, vsync_front: 3, vsync_width: 5,
        refresh_hz: 60, pixel_clock_10khz: 0xd040, field42: 0x0604,
    };
}

/// set-mode (sec 8.6.4): `id=0x48 sub=0x22`, a 96-byte inner message carrying a
/// DisplayID-Type-I u16 timing record. **Verified byte-exact** against the golden
/// `[59]` 3840x2160@60 capture for every byte except the trailing 22-byte session
/// MAC (off74..95), which [`seal`]'s caller / the HDCP session layer appends.
///
/// Layout (inner offsets): off20 BE u32 generation=2; off26 begins the LE u16
/// record `hactive,hblank,hsync_front,hsync_width,vactive,vblank,vsync_front,
/// vsync_width,field42,refresh,flags(0x4000)`; off48/off58/off60/off66 carry
/// constants observed in the 4K capture; off70 the pixel clock (10 kHz units).
pub(super) fn set_mode(counter: u16, t: &Timing) -> Result<KVec<u8>> {
    let mut b = KVec::with_capacity(96, GFP_KERNEL)?;
    header(&mut b, 0x48, 0x22, counter)?;
    pad_to(&mut b, 20)?;
    b.extend_from_slice(&2u32.to_be_bytes(), GFP_KERNEL)?; // off20: BE generation=2
    pad_to(&mut b, 26)?; // off24..25 zero; timing begins at off26
    for v in [
        t.hactive, t.hblank, t.hsync_front, t.hsync_width,
        t.vactive, t.vblank, t.vsync_front, t.vsync_width,
        t.field42, t.refresh_hz, 0x4000, /* off46 flags */ 0x6000, /* off48 */
    ] {
        b.extend_from_slice(&v.to_le_bytes(), GFP_KERNEL)?;
    }
    pad_to(&mut b, 58)?;
    b.extend_from_slice(&0x0080u16.to_le_bytes(), GFP_KERNEL)?; // off58 (observed const)
    b.extend_from_slice(&0x00ffu16.to_le_bytes(), GFP_KERNEL)?; // off60 (observed const)
    pad_to(&mut b, 66)?;
    b.extend_from_slice(&0x0800u16.to_le_bytes(), GFP_KERNEL)?; // off66 (observed const)
    pad_to(&mut b, 70)?;
    b.extend_from_slice(&t.pixel_clock_10khz.to_le_bytes(), GFP_KERNEL)?; // off70
    pad_to(&mut b, 96)?;
    Ok(b)
}

/// Standard VESA MCCS (Monitor Control Command Set 2.2) VCP feature codes, driven over
/// DDC/CI. The macOS DisplayLink agent exposes these as per-display brightness/contrast
/// ("Popover did show -- starting DDC/CI communication", `setBrightness`/`setContrast`); the
/// dock bridges the DDC/CI transaction to the downstream monitor's I2C slave 0x37 -- the same
/// monitor-I2C path the EDID read ([`get_edid_req`]) uses for the 0x50 EDID slave.
pub(super) const VCP_BRIGHTNESS: u8 = 0x10;
pub(super) const VCP_CONTRAST: u8 = 0x12;
/// VCP 0xD6 "Power mode": value 0x01 = on, 0x04 = off (DPMS-off / hard standby). Lets DPMS
/// blank the panel backlight instead of freezing the last frame (see [`crtc_atomic_disable`]).
pub(super) const VCP_POWER_MODE: u8 = 0xd6;
pub(super) const POWER_ON: u16 = 0x01;
pub(super) const POWER_OFF: u16 = 0x04;

/// Build a DDC/CI "Set VCP Feature" request: the 7 bytes a DDC/CI host writes to the
/// monitor's I2C slave 0x37, after the 0x6e (= 0x37<<1) write address (VESA DDC/CI 1.1
/// sec 4.4). Layout: source 0x51, length `0x80 | 4`, opcode 0x03 (Set VCP), VCP code,
/// value-hi, value-lo, then an XOR checksum seeded with the destination address 0x6e. Pure
/// and fully standard, so it is unit-tested byte-exact against the spec
/// ([`super::tests::ddc_ci_set_vcp_checksum`]).
pub(super) fn ddc_ci_set_vcp(vcp: u8, value: u16) -> [u8; 7] {
    let body = [0x51u8, 0x84, 0x03, vcp, (value >> 8) as u8, value as u8];
    let mut chk = 0x6eu8; // checksum seed = destination slave-write address (0x37 << 1)
    for &x in &body {
        chk ^= x;
    }
    [body[0], body[1], body[2], body[3], body[4], body[5], chk]
}

/// CP message that tunnels a DDC/CI Set-VCP write to the downstream monitor -- the brightness,
/// contrast and DPMS-power controls the macOS/Windows agents drive over "DDC/CI communication".
/// The dock's monitor-I2C bridge is the same one the EDID read uses, so this is modelled as the
/// WRITE companion to the `0x15/0x21` EDID read: `id=0x15 sub=0x22`, carrying the I2C slave
/// (0x37) + payload length at off20 and the 7-byte DDC/CI Set-VCP payload at off22.
///
/// The `id`/`sub` and payload offset are **inferred** from the EDID-read pairing -- the write
/// transaction was never captured (it only fires once a monitor is actively driven, i.e. past
/// the CP wall), so re-check against a capture once CP engages. The DDC/CI bytes themselves
/// ([`ddc_ci_set_vcp`]) are standard and verified.
pub(super) fn ddc_set_vcp(counter: u16, vcp: u8, value: u16) -> Result<KVec<u8>> {
    let payload = ddc_ci_set_vcp(vcp, value);
    let mut b = KVec::with_capacity(32, GFP_KERNEL)?;
    header(&mut b, 0x15, 0x22, counter)?;
    pad_to(&mut b, 20)?;
    // off20: monitor DDC/CI I2C slave (0x37) + DDC/CI payload length.
    b.extend_from_slice(&[0x37, payload.len() as u8], GFP_KERNEL)?;
    // off22: the DDC/CI Set-VCP bytes (same off22 convention as the EDID payload).
    b.extend_from_slice(&payload, GFP_KERNEL)?;
    pad_to(&mut b, 32)?;
    Ok(b)
}

/// EDID base-block sanity check: length, the `00 FF..FF 00` magic, and the 1-byte
/// checksum (all 128 base bytes sum to 0 mod 256). A corrupt blob must never drive a
/// mode-set, so [`timing_from_edid`] rejects anything that fails this.
fn edid_valid(edid: &[u8]) -> bool {
    const MAGIC: [u8; 8] = [0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00];
    edid.len() >= 128
        && edid[..8] == MAGIC
        && edid[..128].iter().fold(0u8, |a, &b| a.wrapping_add(b)) == 0
}

/// Parse one 18-byte EDID detailed timing descriptor into a [`Timing`], or `None` if it
/// is too short or not a timing (pixel clock 0 marks a monitor descriptor). `field42`
/// is left at the sec 8.6.4 default (`0x0600`) -- its low byte is mode-varying and not fully
/// decoded, so the live mode-set substitution leaves the captured value in place.
fn parse_dtd(d: &[u8]) -> Option<Timing> {
    if d.len() < 18 {
        return None;
    }
    let pclk = u16::from_le_bytes([d[0], d[1]]);
    if pclk == 0 {
        return None; // monitor descriptor, not a detailed timing
    }
    let hi = |v: u8, lo: u8| -> u16 { ((v as u16) << 8) | lo as u16 };
    let hactive = hi((d[4] >> 4) & 0xf, d[2]);
    let hblank = hi(d[4] & 0xf, d[3]);
    let vactive = hi((d[7] >> 4) & 0xf, d[5]);
    let vblank = hi(d[7] & 0xf, d[6]);
    let hsync_front = (((d[11] >> 6) & 0x3) as u16) << 8 | d[8] as u16;
    let hsync_width = (((d[11] >> 4) & 0x3) as u16) << 8 | d[9] as u16;
    let vsync_front = (((d[11] >> 2) & 0x3) as u16) << 4 | ((d[10] >> 4) & 0xf) as u16;
    let vsync_width = ((d[11] & 0x3) as u16) << 4 | (d[10] & 0xf) as u16;
    let htotal = hactive.wrapping_add(hblank) as u32;
    let vtotal = vactive.wrapping_add(vblank) as u32;
    let refresh_hz = if htotal != 0 && vtotal != 0 {
        ((pclk as u32 * 10_000 + (htotal * vtotal) / 2) / (htotal * vtotal)) as u16
    } else {
        0
    };
    Some(Timing {
        hactive,
        hblank,
        hsync_front,
        hsync_width,
        vactive,
        vblank,
        vsync_front,
        vsync_width,
        refresh_hz,
        pixel_clock_10khz: pclk,
        field42: 0x0600,
    })
}

/// Extract the monitor's **preferred** detailed timing from an EDID for the live mode-set
/// (CP-HANDSHAKE.md sec 4e). The first DTD in the base block is the preferred timing per the
/// EDID spec; scan all four base descriptor slots (off 54/72/90/108) so a leading monitor
/// descriptor (name/range/serial) doesn't hide it, and if the base block carries no DTD at
/// all, fall back to the first DTD in the CTA-861 extension block. The blob is validated
/// first; an invalid or timing-less EDID returns `None` so the caller keeps its known-good
/// fallback timing rather than driving the dock with garbage.
pub(super) fn timing_from_edid(edid: &[u8]) -> Option<Timing> {
    if !edid_valid(edid) {
        return None;
    }
    // Base-block descriptors: the first valid DTD is the preferred timing.
    for off in [54usize, 72, 90, 108] {
        if off + 18 <= edid.len() {
            if let Some(t) = parse_dtd(&edid[off..off + 18]) {
                return Some(t);
            }
        }
    }
    // No DTD in the base block: try the first CTA-861 extension's DTD area. CTA-861 blocks
    // have tag 0x02 at byte 0 and a DTD-area byte offset at byte 2 (>= 4 when DTDs follow);
    // descriptors run in 18-byte records up to the extension's checksum byte (127).
    if edid[126] as usize >= 1 && edid.len() >= 256 {
        let ext = &edid[128..256];
        if ext[0] == 0x02 {
            let start = ext[2] as usize;
            if start >= 4 {
                let mut off = start;
                while off + 18 <= 127 {
                    if let Some(t) = parse_dtd(&ext[off..off + 18]) {
                        return Some(t);
                    }
                    off += 18;
                }
            }
        }
    }
    None
}

/// Overwrite the geometry + clock fields of an in-place set-mode inner message
/// (`id=0x48 sub=0x22`) with `t` (CP-HANDSHAKE.md sec 4e). Offsets mirror [`set_mode`]:
/// the LE u16 timing record at off26 and the pixel clock at off70. `field42` (off42),
/// the off66 token and the encrypted trailer are intentionally **left as captured**;
/// only the EDID-derived values change, so the wire length (hence `wire_seq`) is
/// unchanged. No-op if `plain` is too short.
pub(super) fn apply_edid_timing(plain: &mut [u8], t: &Timing) {
    if plain.len() < 72 {
        return;
    }
    let put = |b: &mut [u8], off: usize, v: u16| {
        b[off] = v as u8;
        b[off + 1] = (v >> 8) as u8;
    };
    put(plain, 26, t.hactive);
    put(plain, 28, t.hblank);
    put(plain, 30, t.hsync_front);
    put(plain, 32, t.hsync_width);
    put(plain, 34, t.vactive);
    put(plain, 36, t.vblank);
    put(plain, 38, t.vsync_front);
    put(plain, 40, t.vsync_width);
    put(plain, 44, t.refresh_hz);
    put(plain, 70, t.pixel_clock_10khz);
}

/// Convert a DRM display mode (the timing the *compositor* selected from the connector's
/// EDID-derived mode list) into a set-mode [`Timing`]. This is what makes the dock
/// multi-mode: `drm_edid_connector_add_modes` already advertises every base+extension mode
/// from the dock's EDID, and when userspace sets any one of them the resulting
/// `drm_display_mode` lands here verbatim -- no re-parsing of EDID offsets. The blanking
/// fields map straight across (CVT/DMT/DisplayID all use the same front-porch/sync model),
/// and the refresh rate comes from DRM's own `drm_mode_vrefresh` helper rather than a
/// hand-rolled divide. `field42` keeps the sec 8.6.4 default (its low byte is mode-varying and
/// not fully decoded); the dock tolerates the high byte `0x06`.
///
/// SAFETY: `mode` must point to a valid `drm_display_mode` for the duration of the call.
pub(super) unsafe fn timing_from_drm_mode(mode: *const bindings::drm_display_mode) -> Timing {
    // SAFETY: caller guarantees `mode` is a live drm_display_mode.
    let m = unsafe { &*mode };
    // SAFETY: `drm_mode_vrefresh` only reads the mode; `mode` is valid per the contract.
    let refresh = unsafe { bindings::drm_mode_vrefresh(mode) } as u16;
    let sub = |a: u16, b: u16| a.saturating_sub(b);
    Timing {
        hactive: m.hdisplay,
        hblank: sub(m.htotal, m.hdisplay),
        hsync_front: sub(m.hsync_start, m.hdisplay),
        hsync_width: sub(m.hsync_end, m.hsync_start),
        vactive: m.vdisplay,
        vblank: sub(m.vtotal, m.vdisplay),
        vsync_front: sub(m.vsync_start, m.vdisplay),
        vsync_width: sub(m.vsync_end, m.vsync_start),
        refresh_hz: refresh,
        // `clock` is in kHz; the set-mode field is in 10 kHz units.
        pixel_clock_10khz: (m.clock / 10).clamp(0, u16::MAX as i32) as u16,
        field42: 0x0600,
    }
}

/// Decode the inner header of a dock->host CP frame: returns `(id, sub, ictr)` from
/// the first decrypted block (CP-HANDSHAKE.md sec 3), or `None` if `wire` is not a
/// decryptable CP frame. Used by the live loop to log what the dock is replying.
pub(super) fn reply_info(
    ks: &[u8; 16],
    out_riv: &[u8; 8],
    wire: &[u8],
) -> Option<(u16, u16, u16)> {
    if wire.len() <= 16 {
        return None;
    }
    let seq = u32::from_le_bytes([wire[12], wire[13], wire[14], wire[15]]);
    let head = &wire[16..wire.len().min(32)];
    let inner = open_in(ks, &in_riv(out_riv), seq, head).ok()?;
    if inner.len() < 6 {
        return None;
    }
    Some((
        u16::from_le_bytes([inner[0], inner[1]]),
        u16::from_le_bytes([inner[2], inner[3]]),
        u16::from_le_bytes([inner[4], inner[5]]),
    ))
}

/// CP `sub` ids seen on the wire (CP-HANDSHAKE.md). Used to score a candidate
/// decrypt: a plaintext whose `sub` is one of these (and whose post-counter pad is
/// zero) is almost certainly the correct key/riv.
fn is_known_sub(sub: u16) -> bool {
    matches!(
        sub,
        0x00 | 0x04 | 0x0c | 0x10 | 0x20 | 0x21 | 0x22 | 0x24 | 0x25 | 0x30 | 0x41
            | 0x42 | 0x43 | 0x45 | 0x75 | 0x84
    )
}

/// Diagnostic decode: try a dock->host frame under every plausible riv variant and
/// return the best-scoring inner `(riv_tag, id, sub, ictr)`. The interactive
/// `wsub=0x45` replies decrypt under `in_riv` (byte7^1), but the **cap-phase**
/// `wsub=0x25` frames decrypt under the session ks with **byte7 unchanged** (the OUT
/// value) -- see the cold-ref transcript. `byte0^0x80` selects the head. This mirrors
/// `decode-handshake.py`'s scoring so a live trace shows what the dock is actually
/// asking for during the capability exchange we currently skip.
pub(super) fn decode_any(
    ks: &[u8; 16],
    out_riv: &[u8; 8],
    wire: &[u8],
) -> Option<(&'static str, u16, u16, u16, [u8; 24])> {
    if wire.len() <= 16 {
        return None;
    }
    let seq = u32::from_le_bytes([wire[12], wire[13], wire[14], wire[15]]);
    let head = &wire[16..wire.len().min(48)];
    let out0 = *out_riv;
    let in0 = in_riv(out_riv);
    let mut out1 = out0;
    out1[0] ^= 0x80;
    let mut in1 = in0;
    in1[0] ^= 0x80;
    let variants: [(&'static str, [u8; 8]); 4] =
        [("out/h0", out0), ("in/h0", in0), ("out/h1", out1), ("in/h1", in1)];
    let mut best: Option<(i32, &'static str, u16, u16, u16, [u8; 24])> = None;
    for (tag, riv) in variants.iter() {
        let Ok(pt) = open_in(ks, riv, seq, head) else { continue };
        if pt.len() < 8 {
            continue;
        }
        let id = u16::from_le_bytes([pt[0], pt[1]]);
        let sub = u16::from_le_bytes([pt[2], pt[3]]);
        let ctr = u16::from_le_bytes([pt[4], pt[5]]);
        let pad = u16::from_le_bytes([pt[6], pt[7]]);
        let mut sc = 0i32;
        if is_known_sub(sub) {
            sc += 50;
        }
        if pad == 0 {
            sc += 10;
        }
        if ctr < 0x400 {
            sc += 5;
        }
        if best.map_or(true, |b| sc > b.0) {
            // Keep the first 24 plaintext bytes so the live trace shows the decoded
            // structure (e.g. the `..4c..de..` cap-descriptor template that, in the
            // capture, is session-independent -- its absence flags a ks/riv mismatch).
            let mut sample = [0u8; 24];
            let n = pt.len().min(24);
            sample[..n].copy_from_slice(&pt[..n]);
            best = Some((sc, tag, id, sub, ctr, sample));
        }
    }
    best.map(|(_, tag, id, sub, ctr, sample)| (tag, id, sub, ctr, sample))
}

/// cursor create (sec 8.6.1): `id=0x1b sub=0x42`, advertises `w x h`.
pub(super) fn cursor_create(counter: u16, w: u16, h: u16) -> Result<KVec<u8>> {
    let mut b = KVec::with_capacity(32, GFP_KERNEL)?;
    header(&mut b, 0x1b, 0x42, counter)?;
    pad_to(&mut b, 20)?;
    b.extend_from_slice(&[0x00, 0x02, 0x00], GFP_KERNEL)?; // marker seen in captures
    b.extend_from_slice(&w.to_le_bytes(), GFP_KERNEL)?;
    b.extend_from_slice(&h.to_le_bytes(), GFP_KERNEL)?;
    Ok(b)
}

/// cursor move (sec 8.6.1): `id=0x1a sub=0x43`, head id @22, X @24, Y @26 (LE).
pub(super) fn cursor_move(counter: u16, head: u8, x: u16, y: u16) -> Result<KVec<u8>> {
    let mut b = KVec::with_capacity(28, GFP_KERNEL)?;
    header(&mut b, 0x1a, 0x43, counter)?;
    pad_to(&mut b, 22)?;
    b.push(head, GFP_KERNEL)?; // off22 head/monitor id
    b.push(1, GFP_KERNEL)?; // off23 flag
    b.extend_from_slice(&x.to_le_bytes(), GFP_KERNEL)?; // off24
    b.extend_from_slice(&y.to_le_bytes(), GFP_KERNEL)?; // off26
    Ok(b)
}

/// cursor image (sec 8.6.1): `id=0x1c sub=0x41`. Mirrors [`cursor_create`]'s header (the
/// `00 02 00` marker + `w`,`h` at off20) and appends the `w*h` BGRA bitmap. `bgra` must be
/// `w*h*4` bytes -- DRM hands the driver a 64x64 ARGB8888 cursor buffer and the caller swaps
/// it
/// to BGRA. The image sub-layout past the create-style header is capture-unconfirmed (only the
/// id and the shared header are decoded); re-check against a capture once CP engages.
pub(super) fn cursor_image(counter: u16, w: u16, h: u16, bgra: &[u8]) -> Result<KVec<u8>> {
    if bgra.len() != w as usize * h as usize * 4 {
        return Err(EINVAL);
    }
    let mut b = KVec::with_capacity(32 + bgra.len(), GFP_KERNEL)?;
    header(&mut b, 0x1c, 0x41, counter)?;
    pad_to(&mut b, 20)?;
    b.extend_from_slice(&[0x00, 0x02, 0x00], GFP_KERNEL)?; // marker (mirrors cursor_create)
    b.extend_from_slice(&w.to_le_bytes(), GFP_KERNEL)?;
    b.extend_from_slice(&h.to_le_bytes(), GFP_KERNEL)?;
    b.extend_from_slice(bgra, GFP_KERNEL)?;
    Ok(b)
}

/// DisplayLink "Dl3Cmac" CP-message integrity tag (16 bytes) -- **FULLY SOLVED + CROSS-SESSION
/// VERIFIED 2026-06-11** (`captures/DL3CMAC-FULLY-SOLVED-20260611.md`):
/// `tag = AES-CMAC(ks, mac_nonce(8) || BE64(wire_seq) || ciphertext)` where
/// - `mac_nonce` = the CTR stream `riv` **with `byte0 ^= 0x80`** (this byte0 flip is the bit
///   prior writeups missed -- they tried `riv` / `riv^1@byte7` and OUT never verified),
/// - `wire_seq` = the AES-CTR block counter (frame header off-12), zero-extended to BE64,
/// - `ciphertext` = the AES-CTR ciphertext content (encrypt-then-MAC), tag appended IN CLEAR.
/// `K_dl3 = ks`. Proven: 110/115 OUT + 128/135 IN corpus frames AND cold-ref msg0 (a different
/// session) reproduce byte-exact. Pass the CTR `riv` directly; the byte0 flip is applied here.
pub(super) fn dl3cmac_tag(
    ks: &[u8; 16],
    riv: &[u8; 8],
    wire_seq: u64,
    ciphertext: &[u8],
) -> Result<[u8; 16]> {
    let mut mac_nonce = *riv;
    mac_nonce[0] ^= 0x80;
    let mut buf = KVec::with_capacity(16 + ciphertext.len(), GFP_KERNEL)?;
    buf.extend_from_slice(&mac_nonce, GFP_KERNEL)?;
    buf.extend_from_slice(&wire_seq.to_be_bytes(), GFP_KERNEL)?;
    buf.extend_from_slice(ciphertext, GFP_KERNEL)?;
    crypto::aes_cmac(ks, &buf)
}

/// Seal a CP message with a **freshly computed live Dl3Cmac**, reusing DLM's captured wire
/// `header` (so `seq`/`aux` are byte-identical) but recomputing the tail tag for THIS session.
/// `content_pt` is the real inner plaintext WITHOUT the 16-byte tag region. Wire body =
/// `AES-CTR(ks, riv, content_pt)` || `dl3cmac_tag(...)`. This is the live-generation path. See
/// `captures/DL3CMAC-FULLY-SOLVED-20260611.md`.
pub(super) fn seal_livemac(
    ks: &[u8; 16],
    riv: &[u8; 8],
    header: &[u8],
    content_pt: &[u8],
) -> Result<KVec<u8>> {
    let seq = u32::from_le_bytes([header[12], header[13], header[14], header[15]]);
    let mut ct = KVec::with_capacity(content_pt.len(), GFP_KERNEL)?;
    for (i, chunk) in content_pt.chunks(16).enumerate() {
        let mut iv = [0u8; 16];
        iv[..8].copy_from_slice(riv);
        iv[12..].copy_from_slice(&seq.wrapping_add(i as u32).to_be_bytes());
        let ksb = crypto::aes128_ecb(ks, &iv)?;
        for (j, &p) in chunk.iter().enumerate() {
            ct.push(p ^ ksb[j], GFP_KERNEL)?;
        }
    }
    let tag = dl3cmac_tag(ks, riv, seq as u64, &ct)?;
    let mut frame = KVec::with_capacity(16 + ct.len() + 16, GFP_KERNEL)?;
    frame.extend_from_slice(&header[..16], GFP_KERNEL)?;
    frame.extend_from_slice(&ct, GFP_KERNEL)?;
    frame.extend_from_slice(&tag, GFP_KERNEL)?;
    Ok(frame)
}

/// Seal an inner CP message into a wire frame (type=4 sub=0x24, `seq`). DisplayLink
/// CP is **encrypt-then-MAC**: the message content is AES-CTR-encrypted, then a
/// 16-byte Dl3Cmac tag (`AES-CMAC(ks, riv || BE64(seq) || ciphertext)`) is appended.
/// The keystream is `AES_ECB(ks, riv(8) || u32(0) || u32_be(seq + block))` (sec 6.1).
///
/// `inner` is the captured golden plaintext `[content || stale-tag-region(16)]`; we
/// encrypt only `content = inner[..len-16]` and append a **fresh** tag keyed by our
/// live session, so the dock's Dl3Cmac verification passes (the stale replayed tag is
/// why the dock previously dropped our CP). VERIFIED construction (sec 8.6.7).
pub(super) fn seal(
    ks: &[u8; 16],
    riv: &[u8; 8],
    seq: u32,
    inner: &[u8],
) -> Result<KVec<u8>> {
    // The interactive CP stream: session ks, wire sub `0x24`.
    seal_stream(ks, riv, 0x24, seq, inner)
}

/// Build a fully sealed interactive CP frame (`type=4 sub=0x24`) at `wire_seq` over `content`
/// (the inner plaintext, WITHOUT any trailing 16-byte tag placeholder): the 16-byte wire
/// header -- size, `type=4`, `sub=0x24`, the per-`id` [`aux_for_id`] field, and `wire_seq` --
/// followed by [`seal_livemac`] (AES-CTR ciphertext + appended live Dl3Cmac). Shared by the
/// bring-up live loop ([`VinoDriver::send_live_cp`]) and the runtime KMS senders
/// ([`drm_sink::VinoDrmData::send_cp`]) so both produce a byte-identical wire frame.
pub(super) fn seal_interactive(
    ks: &[u8; 16],
    riv: &[u8; 8],
    id: u16,
    wire_seq: u32,
    content: &[u8],
) -> Result<KVec<u8>> {
    let body_len = content.len() + 16; // AES-CTR ciphertext + 16-byte Dl3Cmac
    let size = ((16 + body_len) - 4) as u16;
    let aux = aux_for_id(id, body_len);
    let mut hdr = [0u8; 16];
    hdr[2..4].copy_from_slice(&size.to_le_bytes());
    hdr[4..8].copy_from_slice(&4u32.to_le_bytes()); // type=4
    hdr[8..10].copy_from_slice(&0x24u16.to_le_bytes()); // sub=0x24 (interactive CP)
    hdr[10..12].copy_from_slice(&aux.to_le_bytes());
    hdr[12..16].copy_from_slice(&wire_seq.to_le_bytes());
    seal_livemac(ks, riv, &hdr, content)
}

/// The CP wire-header `aux`@10 (`sub_len_dw`) field is a **strict per-inner-message-id
/// constant** in DLM's CP stream -- verified byte-exact across all 94 captured 1080p CP
/// frames (`cp-hdrwire-1080p.bin`) -- **not** `body.len()/4`, which is what `push_frame`
/// derives. Reproducing it makes a generated CP frame's header byte-identical to DLM, the
/// leading hypothesis for the dock engaging its CP cipher (the dock acks our plaintext cap
/// but emits 0 encrypted replies with the wrong `aux`). See docs/BLOCKER.md and memory
/// `project_cp_aux_field_per_id_constant`. Unknown ids fall back to the dword count so an
/// unrecognised message is still well-formed. This makes the generated `seal`/`seal_stream`
/// path match DLM without a captured-header blob -- the basis for **live** CP generation.
pub(super) fn aux_for_id(id: u16, body_len: usize) -> u16 {
    match id {
        0x14 => 0x0a,
        0x15 => 0x09,
        0x16 => 0x08,
        0x19 => 0x05,
        0x1f => 0x0f,
        0x22 => 0x0c,
        0x26 => 0x08,
        0x2a => 0x04,
        0x32 => 0x0c,
        0x48 => 0x06,
        0x9a => 0x04,
        _ => (body_len / 4) as u16,
    }
}

/// General AES-CTR seal under an arbitrary stream `key`/`riv` and wire sub. `seal`
/// is the session-CP case (`wsub=0x24`); the **cap phase** (CP-HANDSHAKE.md sec 4b)
/// needs `wsub=0x04` sealed under the dock's `id=0x32`-delivered per-head stream key,
/// not the session ks -- which `seal` cannot express. Body construction is identical:
/// AES-CTR(key, riv || 0x00000000 || BE32(seq+block)) over the **whole** inner message
/// (no appended MAC; the inner carries its own encrypted trailer -- verified byte-exact
/// vs DLM, 30/30 wire frames).
pub(super) fn seal_stream(
    key: &[u8; 16],
    riv: &[u8; 8],
    wsub: u16,
    seq: u32,
    inner: &[u8],
) -> Result<KVec<u8>> {
    let mut ct = KVec::with_capacity(inner.len(), GFP_KERNEL)?;
    for (i, chunk) in inner.chunks(16).enumerate() {
        let mut iv = [0u8; 16];
        iv[..8].copy_from_slice(riv);
        iv[12..].copy_from_slice(&seq.wrapping_add(i as u32).to_be_bytes());
        let ksb = crypto::aes128_ecb(key, &iv)?;
        for (j, &p) in chunk.iter().enumerate() {
            ct.push(p ^ ksb[j], GFP_KERNEL)?;
        }
    }
    let mut frame = KVec::with_capacity(16 + ct.len(), GFP_KERNEL)?;
    // DLM-exact `aux`@10: a per-inner-id constant (see `aux_for_id`), not `body/4`. The
    // id is read from the *plaintext* inner (off 0); `push_frame` would derive the wrong
    // value and is the suspected reason the dock won't engage its CP cipher.
    let id = if inner.len() >= 2 { u16::from_le_bytes([inner[0], inner[1]]) } else { 0 };
    super::proto::push_frame_with(&mut frame, 0x04, wsub, aux_for_id(id, ct.len()), seq, &ct)?;
    Ok(frame)
}

/// Derive the dock->host (IN) CP riv from the host->dock (OUT) `riv`. **It is the
/// SAME riv -- no transform.** Proven 2026-06-12 by decrypting a frida-keyed DLM cold
/// session's engaged `sub=0x45` replies (`captures/dlm-coldkeys-20260611-135237`, logged
/// `ks`/`out_riv`): the dock's replies decrypt cleanly ONLY under the raw `out_riv`
/// (`id=0x4c sub=0 ctr=8` to msg0, `id=0x14 sub=0x10` ACKs, `id=0x213` cert, ...); the old
/// `byte7 ^= 1` gives garbage. The earlier "byte7^1 for IN" note was never validated against
/// a real engaged reply (vino never engaged) and was wrong -- it would have made vino
/// misdecode
/// every dock reply (and partly explains old "dock replies garbage under our ks" findings).
pub(super) fn in_riv(out_riv: &[u8; 8]) -> [u8; 8] {
    *out_riv
}

/// Decrypt a dock->host CP frame body (AES-CTR, the same keystream as [`seal`] but
/// keyed with the IN `riv`). `ct` is the ciphertext (wire bytes after the 16-byte
/// cleartext header); `seq` is the wire counter at wire offset 12.
pub(super) fn open_in(
    ks: &[u8; 16],
    in_riv: &[u8; 8],
    seq: u32,
    ct: &[u8],
) -> Result<KVec<u8>> {
    let mut pt = KVec::with_capacity(ct.len(), GFP_KERNEL)?;
    for (i, chunk) in ct.chunks(16).enumerate() {
        let mut iv = [0u8; 16];
        iv[..8].copy_from_slice(in_riv);
        iv[12..].copy_from_slice(&seq.wrapping_add(i as u32).to_be_bytes());
        let ksb = crypto::aes128_ecb(ks, &iv)?;
        for (j, &c) in chunk.iter().enumerate() {
            pt.push(c ^ ksb[j], GFP_KERNEL)?;
        }
    }
    Ok(pt)
}

/// If `wire` is an EDID reply (dock->host EP84, `type=4 sub=0x45`, inner
/// `id=0x194 sub=0x21`), decrypt it with the IN riv and return the embedded EDID
/// blob (base block + extensions). The EDID begins at inner offset 22; its total
/// length is `128 * (1 + extension_count)`, where the extension count is base-block
/// byte 126. Returns `None` for any other frame. See docs/CONTROL-PLANE.md.
pub(super) fn parse_edid_from_reply(
    ks: &[u8; 16],
    out_riv: &[u8; 8],
    wire: &[u8],
) -> Result<Option<KVec<u8>>> {
    // Wire header: [.. type@4 u32 .. sub@8 u16 .. seq@12 u32]; body at off16.
    if wire.len() <= 16 || u16::from_le_bytes([wire[8], wire[9]]) != 0x45 {
        return Ok(None);
    }
    let seq = u32::from_le_bytes([wire[12], wire[13], wire[14], wire[15]]);
    let inner = open_in(ks, &in_riv(out_riv), seq, &wire[16..])?;
    // Inner header: [id u16][sub u16][counter u16][00 00]; EDID payload at off22.
    const EDID_OFF: usize = 22;
    if inner.len() < EDID_OFF + 128 {
        return Ok(None);
    }
    let id = u16::from_le_bytes([inner[0], inner[1]]);
    let sub = u16::from_le_bytes([inner[2], inner[3]]);
    // The get-EDID reply id is `0x194` on the wire (CP-HANDSHAKE.md sec 4f, ground-truthed
    // against the cold-ref capture); older notes wrote the low byte `0x94` alone. Accept
    // both so a real `0x194` reply is not silently dropped (the EDID would never reach the
    // connector even after CP engages).
    if (id != 0x94 && id != 0x194) || sub != 0x21 {
        return Ok(None);
    }
    let edid = &inner[EDID_OFF..];
    // Validate the EDID base-block magic `00 FF FF FF FF FF FF 00`.
    const MAGIC: [u8; 8] = [0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00];
    if edid[..8] != MAGIC {
        return Ok(None);
    }
    let total = ((1 + edid[126] as usize) * 128).min(edid.len());
    let mut out = KVec::with_capacity(total, GFP_KERNEL)?;
    out.extend_from_slice(&edid[..total], GFP_KERNEL)?;
    Ok(Some(out))
}
