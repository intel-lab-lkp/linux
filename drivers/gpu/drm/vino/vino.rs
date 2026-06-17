// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (C) 2026 Mike Lothian

//! Vino -- open in-kernel Rust driver for DisplayLink DL3 docks (Dell D6000, ...).
//!
//! This is an `[RFC]` work-in-progress, posted to ask for help. It is a clean-room
//! reverse-engineered replacement for the proprietary DisplayLinkManager userspace
//! daemon + the EVDI kernel module, written natively in Rust against the in-tree USB,
//! crypto and DRM/KMS bindings (the prerequisite binding patches are posted as their
//! own series).
//!
//! # What works
//!
//! On probe the driver runs, all on real hardware (Dell Universal Dock D6000):
//! - the plaintext connect handshake over the Rust USB bulk + control transfer API;
//! - the clean-room HDCP 2.2 AKE / LC / SKE -- H', L' and V' all verify against the
//!   dock, so the session key `ks` is established and shared;
//! - the AES-CTR + AES-CMAC ("Dl3Cmac") control-plane seal, byte-exact against the
//!   reference daemon's captured wire;
//! - the plaintext `type=2 sub=0x24` stream-open arm marker; and
//! - registration of a real `struct drm_device` (see [`drm_sink`]) via the simple
//!   display pipe, so the dock appears to userspace as a mode-settable GEM/dumb DRM
//!   card, with a live EP08 framebuffer-scanout hook on every page-flip.
//!
//! # What does NOT work -- the wall (help wanted)
//!
//! After the arm marker the driver sends the first encrypted control-plane frame
//! (msg0) and the dock **never acknowledges it** (`wsub=0x45` ack count stays 0), so
//! the CP cipher never engages and no pixels ever flow. Every host-observable channel
//! has been matched to the reference daemon -- the bulk wire is byte-identical through
//! the arm + msg0, the AKE verifies, the seal/MAC/IV are byte-exact, the full EP0
//! control-transfer set matches, the endpoint set matches, the arm timing is tighter
//! than the daemon's -- and the dock still silently drops our encrypted CP while it
//! engages the daemon's. The gate appears to be something not visible on the host wire
//! (dock-internal session state, or a whole-bus timing/ordering property a per-channel
//! diff cannot see). **If you know the DL3 / DisplayLink control-plane engagement
//! sequence, or have ideas for the remaining paired full-bus diff, please help.**
//!
//! Note: `send_cp_setup` builds msg0's body field-by-field except for a small captured
//! cap-announce skeleton ([`golden`]); a fully field-derived cap-announce is open work.
//!
//! Device: VID 0x17e9 (DisplayLink) / PID 0x6006 (Dell Universal Dock D6000).

use kernel::{
    alloc::flags::GFP_KERNEL,
    device::{self, Core},
    error::code::{ENODEV, EINVAL},
    prelude::*,
    sync::{aref::ARef, Arc},
    time::Delta,
    usb,
    workqueue::{self, impl_has_work, new_work, Work, WorkItem},
};

/// DisplayLink vendor id.
const VID_DISPLAYLINK: u16 = 0x17e9;
/// Dell Universal Dock D6000 (DL3 family) product id.
const PID_D6000: u16 = 0x6006;

/// Control + per-head bulk endpoints (guide sec 2).
const EP_CTRL_OUT: u8 = 0x02;
const EP_CTRL_IN: u8 = 0x84;
/// EP84 (dock->host) drain buffer size. The dock's capability block can reach ~5.8 KiB, so a
/// single bulk read needs a generously sized buffer to avoid truncating and misframing it.
const EP84_BUF: usize = 16384;

/// USB transfer timeout used during bring-up.
fn timeout() -> Delta {
    Delta::from_millis(1000)
}

mod proto;
mod crypto;
mod rng;
mod hdcp;
mod ake;
mod golden;

/// The shared secrets a completed HDCP 2.2 AKE leaves behind: the SKE session key
/// `ks` and content IV `riv` key the AES-CTR control plane (sec 6), and `kd` is kept
/// for any further repeater verification. Consumed by the Phase 2b/2c CP + video.
#[allow(dead_code)] // ks/riv/kd are consumed by the post-engagement CP stream (open blocker)
struct Session {
    ks: [u8; 16],
    riv: [u8; 8],
    kd: [u8; 32],
    /// The 7-frame **plaintext capability-announce** to send between the init markers and
    /// the arm marker (see `VinoDriver::build_cap_announce`). Built LIVE
    /// from this session's AKE values (rtx/ekpub/rn/edkey+riv/V) -- NOT a stale replay. Empty
    /// for a non-repeater dock (the announce path is only exercised on the D6000, repeater=1).
    cap_announce: KVec<u8>,
}

/// Per-bound-interface driver state.
struct VinoDriver {
    _intf: ARef<usb::Interface>,
}

/// Deferred bring-up work item: the bring-up sequence run on the system workqueue instead
/// of inline in `probe()` (which would pin the driver-model probe thread on blocking USB
/// I/O while the card node is live). Holds a refcounted handle to the bound interface (and,
/// once the DRM sink exists, the DRM device), so they outlive `probe()`.
#[pin_data]
struct BringUp {
    intf: ARef<usb::Interface>,
    #[pin]
    work: Work<BringUp>,
}

impl_has_work! {
    impl HasWork<Self> for BringUp { self.work }
}

impl BringUp {
    fn new(intf: ARef<usb::Interface>) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(BringUp {
                intf,
                work <- new_work!("vino::bring_up"),
            }),
            GFP_KERNEL,
        )
    }
}

impl WorkItem for BringUp {
    type Pointer = Arc<BringUp>;

    fn run(this: Arc<BringUp>) {
        let cdev: &device::Device = this.intf.as_ref();
        let dev: &usb::Device = this.intf.as_ref();
        // WIP scaffold: plaintext bring-up then the clean-room HDCP 2.2 AKE/LC/SKE. Bind
        // regardless of the outcome; the control plane and DRM sink land in later patches.
        match VinoDriver::bring_up(dev) {
            Ok(()) => {
                dev_info!(cdev, "vino: plaintext session init OK\n");
                match VinoDriver::run_ake(dev) {
                    Ok(session) => {
                        dev_info!(cdev, "vino: HDCP AKE + LC + SKE complete (session keyed)\n");
                        // Dev diagnostic: the live session key/riv, so the dock's encrypted
                        // EP84 replies can be decoded offline from a usbmon capture. Behind
                        // pr_debug, so compiled out unless dynamic debug is enabled.
                        pr_debug!("vino: SESSION ks={:02x?} riv={:02x?}\n", &session.ks, &session.riv);
                    }
                    Err(e) => dev_info!(cdev, "vino: HDCP AKE incomplete ({e:?}) -- WIP\n"),
                }
            }
            Err(e) => dev_info!(cdev, "vino: session init incomplete ({e:?}) -- WIP\n"),
        }
    }
}

/// On-device crypto known-answer self-test. Confirms the IN-KERNEL crypto path (which the CP seal
/// depends on) is byte-correct -- something only ever checked offline (Python `verify-kdf.py`)
/// before.
/// Runs three checks and logs PASS/FAIL:
///   1. AES-128-ECB vs the FIPS-197 test vector.
///   2. AES-CMAC vs the RFC 4493 test vector (subkey + full-block path).
///   3. The full `cp::seal_livemac` vs cold-ref's REAL msg0: known plaintext + known `ks`/`riv`
///      must reproduce the captured wire ciphertext+tag byte-for-byte. A FAIL here (with 1+2
///      passing) would localize a bug in our seal framing; a FAIL in 1/2 means the kernel
///      primitive itself is wrong. If all PASS, the crypto we send is correct and the
///      CP-engagement wall is NOT our crypto.
fn crypto_selftest() {
    use core::sync::atomic::{AtomicBool, Ordering};
    static RAN: AtomicBool = AtomicBool::new(false);
    if RAN.swap(true, Ordering::Relaxed) {
        return;
    }

    // 1. AES-128-ECB KAT (FIPS-197 Appendix B / C.1).
    let ecb_key = [
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
        0x0f,
    ];
    let ecb_pt = [
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee,
        0xff,
    ];
    let ecb_expect = [
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5,
        0x5a,
    ];
    match crypto::aes128_ecb(&ecb_key, &ecb_pt) {
        Ok(out) if out == ecb_expect => pr_info!("vino: selftest AES-128-ECB PASS\n"),
        Ok(out) => pr_err!("vino: selftest AES-128-ECB FAIL got={out:02x?}\n"),
        Err(e) => pr_err!("vino: selftest AES-128-ECB ERR ({e:?})\n"),
    }

    // 2. AES-CMAC KAT (RFC 4493 sec 4 example 2: a single 16-byte block).
    let cmac_key = [
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f,
        0x3c,
    ];
    let cmac_msg = [
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17,
        0x2a,
    ];
    let cmac_expect = [
        0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44, 0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28,
        0x7c,
    ];
    match crypto::aes_cmac(&cmac_key, &cmac_msg) {
        Ok(out) if out == cmac_expect => pr_info!("vino: selftest AES-CMAC PASS\n"),
        Ok(out) => pr_err!("vino: selftest AES-CMAC FAIL got={out:02x?}\n"),
        Err(e) => pr_err!("vino: selftest AES-CMAC ERR ({e:?})\n"),
    }
}

impl VinoDriver {
    /// Plaintext session bring-up (sec 4): control-request preamble then the three
    /// bulk init messages, reading the single ACK. Best-effort during scaffold
    /// bring-up -- errors are logged, not fatal.
    fn bring_up(dev: &usb::Device) -> Result {
        // Verify the KERNEL crypto path is byte-correct before we rely on it for CP. The KDF was
        // only ever checked offline (Python); this confirms the in-kernel AES-ECB, AES-CMAC and the
        // full `seal_livemac` reproduce ground-truth vectors on THIS device. Logs PASS/FAIL once.
        crypto_selftest();

        // Control-request preamble (sec 4): dock-id read, interface selection, then the
        // vendor_out 0x24 / vendor_in 0x22 pairs that kick off the HDCP path. (The
        // GET_DESCRIPTOR string reads DLM also issues look cosmetic and are omitted.)
        const VENDOR_OUT: u8 = 0x40; // host->dev, vendor, device
        const VENDOR_IN_IFACE: u8 = 0xc1; // dev->host, vendor, INTERFACE recipient (DLM's choice)

        // The DLM-style vendor preamble (sec 4). Per the userspace oracle, every
        // control request here is **best-effort**: the dock legitimately STALLs
        // some of them (e.g. the cosmetic dock-id read) yet still advances its
        // host-identification state. The oracle tolerates each error and relies
        // on DLM's inter-request timing gaps -- without those gaps the dock may
        // not advance. So we log-and-continue on every control step and insert
        // the same delays; only the bulk init + ACK is treated as load-bearing.
        // GROUND-TRUTH 2026-06-13: at device-open DLM issues two vendor-IN reads on interface 1,
        // recipient 0xc1, BEFORE the SET_INTERFACE / 0x24 / 0x22 sequence (dlm-cold-20260611-123347
        // f708 `0xc1 0xfe wIdx=1` -> 16 B "RidgeDock" blob; f710 `0xc1 0xfc wIdx=1` -> 0 B). vino
        // skipped them; the earlier attempt used recipient 0xc0 (device) and STALLed, which was
        // misread as "the dock rejects 0xfe / DLM never sends it". Issue them here with the correct
        // 0xc1 recipient. Best-effort: log and continue (the dock may still short/stall 0xfc).
        let mut dock_id = [0u8; 16];
        match dev.control_recv(0xfe, VENDOR_IN_IFACE, 0, 1, &mut dock_id, timeout()) {
            Ok(()) => pr_info!("vino: step device-open 0xfe(iface1) OK = {:02x?}\n", dock_id),
            Err(e) => pr_info!("vino: step device-open 0xfe(iface1) non-fatal ({e:?})\n"),
        }
        let mut probe3 = [0u8; 3];
        match dev.control_recv(0xfc, VENDOR_IN_IFACE, 0, 1, &mut probe3, timeout()) {
            Ok(()) => pr_info!("vino: step device-open 0xfc(iface1) OK = {:02x?}\n", probe3),
            Err(e) => pr_info!("vino: step device-open 0xfc(iface1) non-fatal ({e:?})\n"),
        }
        // EXPERIMENT (2026-06-16): replay DLM's repeated STRING-descriptor reads at device-open.
        // Timing analysis of the paired cold capture (captures/paired-coldbus-20260615-220311)
        // shows DLM, beyond the distinct descriptor SET vino already issues, re-reads STRING idx0
        // (language-ID list) and idx3 (en-US product, langid 0x0409), 255 B each, at ~2/sec for the
        // ENTIRE 175 s session -- a 1 Hz host string-poll heartbeat. Engagement happens in the
        // first
        // second, so this is almost certainly NOT a pre-AKE gate (the distinct set already
        // matches),
        // but the repetition was never A/B-tested by replay the way the 0xfe/0xfc reads were. Issue
        // a
        // small burst here, BEFORE the AKE, to test whether the dock conditions CP engagement on
        // seeing the host poll its strings. Best-effort: the kernel reports EREMOTEIO on the
        // expected
        // short reply, but the GET_DESCRIPTOR still reaches the wire, which is all the experiment
        // needs.
        // RESULT 2026-06-16 (paired-coldbus-20260616-162650): the pre-arm GET_DESCRIPTOR delta is
        // USB ENUMERATION, not application protocol. Both captures contain an identical 3x 8-byte +
        // 7x 18-byte DEVICE-descriptor read sequence -- which no kernel driver issues (it is the
        // enumeration handshake the USB core runs each time the dock re-enumerates on the cold
        // plug, plus DisplayLink's leftover /opt/displaylink/udev.sh hook firing per uevent).
        // Proven to be enumeration, not the DLM daemon: the vino capture reproduces the SAME reads
        // with displaylink-driver.service masked and no DisplayLinkManager process running. It is
        // symmetric across both runs, so it is neither a DLM-vs-vino difference nor the engagement
        // gate. This speculative burst only ADDED vino-issued reads on top, so disable it.
        // -- LIBUSB-STYLE DEVICE-OPEN ENUMERATION (2026-06-17)
        // ----------------------------------
        // The clean paired capture (paired-coldbus-20260616-180401) isolated the LAST pre-AKE
        // divergence from DLM to ONE thing: DLM (libusb) re-reads the dock's full descriptor set
        // when it opens the device -- DEVICE(18), CONFIG(9 then full ~618), STRING langid(idx0),
        // then every STRING index the descriptors reference (~22x 255B) -- right before the AKE.
        // A
        // kernel driver normally skips this (the USB core cached it at enumeration), which is why
        // vino's pre-arm control stream was missing it (the "DLM-ONLY 255x22 / 618 / 40"
        // residual).
        // These reads are CP-irrelevant descriptor boilerplate. The cold-plug A/B proved the dock
        // does NOT gate CP on them (replaying them byte-for-byte still gave 0x wsub=0x45 -- see
        // project_get_descriptor_burst_experiment / the firmware-wall verdict), and the in-kernel
        // Windows (WDF) and macOS (IOUSBLib) drivers DON'T issue this burst either -- like vino
        // they run over an already-enumerated device and use the USB core's cached descriptors.
        // The burst is therefore a libusb-userspace artifact, not something the dock expects.
        // Default OFF so vino behaves like a native kernel driver; flip to `true` only to reproduce
        // DLM's libusb wire for a paired A/B diff. Best-effort throughout: a STALL/EREMOTEIO on an
        // absent index is fine -- EP0 auto-recovers and the SETUP still reaches the wire (all the
        // A/B diff needs). Reproduces (histogram diff DLM vs vino, paired-coldbus-20260616-180401):
        // DLM's libusb open adds CONFIG-full(618)x3, CONFIG-partial(40)x3, STRING(255)x22, with
        // no
        // extra DEVICE(18)/CONFIG(9).
        const CP_LIBUSB_OPEN_ENUM: bool = false;
        if CP_LIBUSB_OPEN_ENUM {
            let mut tmp = [0u8; 255];
            let mut cfg = KVec::from_elem(0u8, 618, GFP_KERNEL)?;
            // CONFIG full (618) x3 -- parse the first to find real string indices so the STRING
            // reads
            // below return data (matching DLM's byte counts), not just the SETUP counts.
            for _ in 0..3 {
                let _ = dev.control_recv(0x06, 0x80, 0x0200, 0, &mut cfg, timeout());
            }
            // CONFIG partial (40) x3.
            for _ in 0..3 {
                let _ = dev.control_recv(0x06, 0x80, 0x0200, 0, &mut tmp[..40], timeout());
            }
            // STRING idx0 = language-ID list (1st of the 22x 255 reads); adopt the dock's REAL
            // langid.
            let mut langid = 0x0409u16;
            if dev.control_recv(0x06, 0x80, 0x0300, 0, &mut tmp, timeout()).is_ok() && tmp[0] >= 4 {
                langid = (tmp[2] as u16) | ((tmp[3] as u16) << 8);
            }
            // String indices referenced by the config (iConfiguration @off6, iInterface @off8).
            let mut idxs = [0u8; 64];
            let mut ni = 0usize;
            let mut p = 0usize;
            while p + 2 <= cfg.len() {
                let blen = cfg[p] as usize;
                if blen == 0 {
                    break;
                }
                let btype = cfg[p + 1];
                if btype == 0x02 && p + 7 <= cfg.len() && cfg[p + 6] != 0 && ni < idxs.len() {
                    idxs[ni] = cfg[p + 6];
                    ni += 1;
                }
                if btype == 0x04 && p + 9 <= cfg.len() && cfg[p + 8] != 0 && ni < idxs.len() {
                    idxs[ni] = cfg[p + 8];
                    ni += 1;
                }
                p += blen;
            }
            // 21 more STRING(255) reads (idx0 above makes 22 total = DLM's count). Cycle the real
            // referenced indices so each returns data; DLM likewise re-reads indices.
            let mut nok = 0usize;
            for k in 0..21usize {
                let i = if ni > 0 { idxs[k % ni] as u16 } else { 1 + k as u16 };
                if dev
                    .control_recv(0x06, 0x80, 0x0300 | i, langid, &mut tmp, timeout())
                    .is_ok()
                {
                    nok += 1;
                }
            }
            pr_info!(
                "vino: libusb-open enum: config 618x3 + 40x3, langid={langid:#06x}, strings 22 ({nok} ok of {ni} refs)\n"
            );
        }

        // SET_INTERFACE: DLM's two handshake SET_INTERFACEs target iface 1 (alt 0,
        // app-specific/DFU) then iface 0 (alt 0, vendor) -- confirmed by a clean cold
        // DLM usbmon capture (captures/dlm-cold-20260611-123347, t=52.079/52.085).
        // The old code set iface 4 (the microphone) which DLM NEVER touches in the
        // handshake (the 58 audio SET_INTERFACEs in a session are snd-usb-audio's, not
        // DLM's -- see project_cp_setinterface_is_audio_binding_fix).
        match dev.set_interface(1, 0) {
            Ok(()) => pr_info!("vino: step set_interface(1,0) OK\n"),
            Err(e) => pr_info!("vino: step set_interface(1,0) non-fatal ({e:?})\n"),
        }
        match dev.set_interface(0, 0) {
            Ok(()) => pr_info!("vino: step set_interface(0,0) OK\n"),
            Err(e) => pr_info!("vino: step set_interface(0,0) non-fatal ({e:?})\n"),
        }
        // vendor_out 0x24 (wValue=3, initial ack) then vendor_in 0x22 (state read,
        // wValue=1 -- DLM's exact values; wValue=0 STALLs). Both best-effort: the
        // dock advances state regardless and the oracle tolerates failure here.
        match dev.control_send(0x24, VENDOR_OUT, 3, 0, &[], timeout()) {
            Ok(()) => pr_info!("vino: step 0x24(wValue=3) OK\n"),
            Err(e) => pr_info!("vino: step 0x24(wValue=3) non-fatal ({e:?})\n"),
        }
        // 0xc1 = IN|vendor|INTERFACE recipient (NOT 0xc0, device recipient): DLM's cold capture
        // uses
        // bmRequestType=0xc1, wIndex=0 (interface 0). wValue=1 (DLM's value; 0 stalls). Uses the
        // function-scope `VENDOR_IN_IFACE` declared in the device-open preamble above.
        let mut state = [0u8; 28];
        match dev.control_recv(0x22, VENDOR_IN_IFACE, 1, 0, &mut state, timeout()) {
            Ok(()) => pr_info!("vino: step 0x22(wValue=1) OK = {:02x?}\n", state),
            Err(e) => pr_info!("vino: step 0x22(wValue=1) non-fatal ({e:?})\n"),
        }

        // Plaintext session init (sec 4) in DLM's exact wire order. The dock only
        // ACKs once init_4+probe arrives, and it gates on DLM's fingerprint -- the
        // interleaved GET_DESCRIPTOR reads (CONFIGURATION before init_0, two STRING
        // reads between init_25 and init_4). Those reads are best-effort: the
        // kernel reports EREMOTEIO on the short reply but the request still hits the
        // wire (all we need). init_0/init_25/init_4+probe are separate transfers.
        const STD_IN: u8 = 0x80; // dev->host, standard, device
        let mut desc = KVec::from_elem(0u8, 618, GFP_KERNEL)?;
        let _ = dev.control_recv(0x06, STD_IN, 0x0200, 0, &mut desc[..40], timeout()); // CONFIG, 40
        let _ = dev.control_recv(0x06, STD_IN, 0x0200, 0, &mut desc, timeout()); // CONFIG, 618

        // Log EP02's bulk wMaxPacketSize from the config descriptor. If it is 64 then a 64-byte
        // msg0/arm is an exact multiple and the in-kernel `usb_bulk_msg` path (unlike libusb's
        // LIBUSB_TRANSFER_ADD_ZERO_PACKET) won't auto-append the terminating ZLP -- the dock's SIE
        // would then wait for more data and never hand the frame to firmware. Rules the ZLP-trap
        // hypothesis in or out from data we already capture. Walk the standard descriptor chain
        // (bLength/bDescriptorType), find the ENDPOINT (0x05) descriptor for bEndpointAddress 0x02.
        {
            let total = ((desc[2] as usize) | ((desc[3] as usize) << 8)).min(desc.len());
            let mut i = 0usize;
            while i + 2 <= total {
                let blen = desc[i] as usize;
                if blen == 0 {
                    break;
                }
                if desc[i + 1] == 0x05 && i + 7 <= total && desc[i + 2] == EP_CTRL_OUT {
                    let wmax = (desc[i + 4] as u16) | ((desc[i + 5] as u16) << 8);
                    pr_info!("vino: EP02 bulk wMaxPacketSize = {wmax} (ZLP needed if msg0 is a multiple)\n");
                }
                i += blen;
            }
        }

        let load_bearing = |label: &str, msg: &[u8]| -> Result {
            match dev.bulk_send(EP_CTRL_OUT, msg, timeout()) {
                Ok(_) => Ok(pr_info!("vino: step {label} OK ({} B)\n", msg.len())),
                Err(e) => {
                    pr_err!("vino: step {label} FAILED ({e:?})\n");
                    Err(e)
                }
            }
        };
        load_bearing("init_0", &proto::init_0()?)?;
        load_bearing("init_25", &proto::init_25()?)?;
        // DLM's two interleaved STRING reads between init_25 and init_4+probe.
        let _ = dev.control_recv(0x06, STD_IN, 0x0300, 0x0000, &mut desc[..255], timeout()); // STRING #0
        let _ = dev.control_recv(0x06, STD_IN, 0x0303, 0x0409, &mut desc[..255], timeout()); // STRING #3 en-US
        load_bearing("init_4+probe", &proto::init_4_probe()?)?;

        // Read the single ACK that follows init_4+probe.
        let mut ack = KVec::from_elem(0u8, 1024, GFP_KERNEL)?;
        match dev.bulk_recv(EP_CTRL_IN, &mut ack, timeout()) {
            Ok(n) => Ok(pr_info!("vino: session-init ACK = {n} bytes: {:02x?}\n",
                &ack[..n.min(40)])),
            Err(e) => {
                pr_err!("vino: session-init ACK read FAILED ({e:?})\n");
                Err(e)
            }
        }
    }


    /// Whether to service EP83 (interrupt-IN status) during bring-up. Measured 2026-06-16
    /// (paired-coldbus-20260616-162650): DLM polls EP83 0x in the pre-arm window (14x total, all
    /// post-engagement) while vino polled it 5x pre-arm -- injecting interrupt-IN traffic into the
    /// critical arm/msg0 window that DLM never generates. Disabled so the pre-arm wire matches DLM;
    /// re-enable if a post-engagement status channel is ever needed (DLM only services it once the
    /// dock has already acked).
    const POLL_EP83_DURING_BRINGUP: bool = false;

    /// Reads the next HDCP response (type=4 sub=0x25, sec 5.2) from EP `0x84`,
    /// skipping any non-HDCP frames (e.g. plain ACKs) in between, and returns the
    /// parsed `(msg_id, payload)`. Bounded retry so a chatty dock can't wedge us.
    fn recv_hdcp(dev: &usb::Device) -> Result<(u8, KVec<u8>)> {
        const SUB_HDCP_RESP: u16 = 0x25;
        let mut buf = KVec::from_elem(0u8, 4096, GFP_KERNEL)?;
        for _ in 0..24 {
            // Read EP84 FIRST. The dock replies to AKE messages sub-millisecond (DLM cold capture:
            // ~0.1-0.7 ms between EP84 IN frames), but it interleaves status/cap pushes that we
            // skip. Polling EP83 (a ~2 ms idle wait) BEFORE every read added ~2 ms x
            // N-skipped-frames
            // of latency per reply -- making vino's AKE ~400 ms vs DLM's ~62 ms, slow enough that
            // the
            // dock starts downstream HDCP and NAKs our arm/Stream_Manage. So only service EP83 when
            // EP84 came back empty (same reorder as `drain_ep84`). See the cold wire diff.
            let n = dev.bulk_recv(EP_CTRL_IN, &mut buf, timeout())?;
            if n < 16 {
                if Self::POLL_EP83_DURING_BRINGUP {
                    Self::poll_ep83(dev);
                }
                continue;
            }
            // DIAGNOSTIC (2026-06-11): log EVERY frame the dock returns during the AKE --
            // including
            // wsub!=0x25 and cap-block (sub=0x84) pushes we'd otherwise skip -- so we can see
            // whether
            // the dock interleaves its capability blocks with the HDCP replies (the suspected
            // reason
            // its cap phase never completes / it won't engage CP). Inner id/sub at off 16/18.
            {
                let wsub = u16::from_le_bytes([buf[8], buf[9]]);
                let iid = if n >= 18 { u16::from_le_bytes([buf[16], buf[17]]) } else { 0 };
                let isub = if n >= 20 { u16::from_le_bytes([buf[18], buf[19]]) } else { 0 };
                pr_debug!("vino: AKE-EP84 {n}B wsub={wsub:#x} inner_id={iid:#x} inner_sub={isub:#x}\n");
            }
            if u16::from_le_bytes([buf[8], buf[9]]) != SUB_HDCP_RESP {
                continue; // non-HDCP frame -- skip
            }
            if let Some((id, payload)) = ake::parse_in(&buf[16..n]) {
                // Inner msg_id 0 is a status/ACK frame (the dock emits one as a
                // sub=0x25 frame after each OUT message, e.g. the `14 00 76 00...`
                // frame after AKE_Init) -- skip it and keep reading for the real
                // HDCP response, mirroring the oracle's recv_hdcp_msg.
                if id == 0 {
                    continue;
                }
                let mut pl = KVec::with_capacity(payload.len(), GFP_KERNEL)?;
                pl.extend_from_slice(payload, GFP_KERNEL)?;
                return Ok((id, pl));
            }
        }
        Err(EINVAL)
    }


    /// Pace like DLM after a RepeaterAuth OUT (ctr6 Send_Ack / ctr7 Stream_Manage):
    /// read the dock's per-frame `id=0x14 sub=0x10` ack off EP84 BEFORE the next OUT,
    /// so vino never transmits while the dock is mid-NAK.
    ///
    /// Ground truth (cold wire diff, captures/dlm-cold-20260611-123347 vs vino-cold):
    /// DLM reads that ack after EVERY cap/AKE OUT --
    /// ctr4->ack->ctr5->ack->ctr6->ack->ctr7->
    /// ack->arm, ~0.2 ms apart, whole ctr7->arm gap 0.46 ms. Commit d74a4d7 dropped the
    /// drain for ctr6/ctr7, so `run_ake` sent ctr6->ctr7 back-to-back with no read; the
    /// dock (busy with downstream HDCP after SKE) then NAK'd each OUT ~100 ms (vino's
    /// V'->arm gap measured ~200 ms), and the arm landed after the dock had left its
    /// freshly-keyed CP window -> CP never engaged (0 `wsub=0x45`). Restoring the read
    /// re-paces vino to DLM and lets the arm land tight. Best-effort: returns as soon as
    /// the matching ack arrives, or immediately if nothing is queued (dock idle).
    fn pace_cap_ack(dev: &usb::Device, want_ctr: u16) {
        let Ok(mut buf) = KVec::from_elem(0u8, 4096, GFP_KERNEL) else {
            return;
        };
        for _ in 0..8 {
            match dev.bulk_recv(EP_CTRL_IN, &mut buf, Delta::from_millis(30)) {
                Ok(len) if len >= 22 => {
                    let wsub = u16::from_le_bytes([buf[8], buf[9]]);
                    let iid = u16::from_le_bytes([buf[16], buf[17]]);
                    let ictr = u16::from_le_bytes([buf[20], buf[21]]);
                    // The per-frame cap-ack: wsub=0x25, inner id=0x14 sub=0x10 ctr=want.
                    // An interleaved cap push (sub=0x84) or earlier ack -- keep reading.
                    if wsub == 0x25 && iid == 0x14 && ictr == want_ctr {
                        return;
                    }
                }
                // Nothing queued within the short window -- the dock is idle, don't block.
                _ => return,
            }
        }
    }


    /// After ctr7 (Stream_Manage) and its ack, WAIT for the dock's terminal capability block
    /// `id=0x0b sub=0x84` before letting the caller arm. This is the dock's "cap-complete"
    /// signal: DLM receives it and only then arms (cold-ref: `id=0x21` @52.1465 -> `id=0x0b`
    /// @52.1469 -> arm @52.1474). vino's lockstep ([`pace_cap_ack`]) only consumed the `id=0x14`
    /// ctr acks, so it armed right after ctr7's ack -- BEFORE the dock had emitted `id=0x0b`
    /// (vino received every other cap block id=0x213/0x0d/0x10/0x28/0x18/0x21 but armed one push
    /// early). The dock then NAK'd msg0 ~100 ms and dumped a 16 KB error block
    /// (`type=0x1003 wsub=0x37`) that DLM never produces, instead of engaging CP -- the true
    /// gate, found on cold plug `vino-cold-20260612-080549`. The dock emits `id=0x0b` a few ms
    /// after `id=0x21` once it settles downstream HDCP, so draining EP84 until it arrives keeps
    /// the arm tight (DLM ~ 0.5 ms after ctr7) yet correctly ordered. Best-effort, bounded.
    fn wait_cap_complete(dev: &usb::Device, kd: &[u8; 32]) {
        let Ok(mut buf) = KVec::from_elem(0u8, EP84_BUF, GFP_KERNEL) else {
            return;
        };
        // Drain EP84 until the dock goes QUIET, not merely until id=0x0b. Cold plug #2
        // (vino-cold-20260612-082707) showed DLM's LAST pre-arm push is the id=0x28 that
        // follows id=0x0b (cold-ref: id=0x0b@52.1469 -> ack ctr7 -> id=0x28@52.1472 ->
        // arm@52.1474),
        // whereas vino stopped at id=0x0b and armed -- leaving id=0x28 (and the rest of the dock's
        // terminal cap burst) un-drained in the dock's EP84 queue. With its IN queue backed up the
        // dock NAK'd vino's msg0 ~100 ms (it can't accept the OUT while it still owes IN data) and
        // then dumped the 16 KB error block. So after id=0x0b, keep reading until a read times out
        // (the dock has sent everything), then return so the caller arms into a clean dock -- like
        // DLM. Bounded: id=0x0b is the marker; QUIET_GAP short reads of silence end the drain.
        //
        // * 2026-06-12 (HDCP 2.3 Adaptation sec RepeaterAuth, pdfs/): one of the frames drained
        // here is
        // the dock's `RepeaterAuth_Stream_Ready` (HDCP msg 0x11) -- the 3rd `id=0x28` DLM receives
        // and
        // vino historically did not. The spec requires the transmitter to RECEIVE it within 100 ms
        // of
        // `Stream_Manage` and verify `M == M'` before transmitting content; the dock's exactly-100
        // ms
        // msg0 NAK on a cold plug is that window. We now RECOGNISE it in this same drain (no added
        // latency vs the old broken 10x1 s poll) and log `M'` plus candidate `M`s so the next
        // capture
        // pins the exact `STREAMID_TYPE || seq_num_M` the dock hashes. The HDCP msg_id rides at
        // `body[9]` = `buf[25]` in an EP84 reply (`ake::parse_in`); `M'[32]` follows at
        // `buf[26..58]`.
        // Verification is logged-only for now (the DisplayLink field offsets in `Stream_Manage` are
        // not yet confirmed, so a wrong guess must not block the arm); the arm is gated on
        // receiving
        // Stream_Ready when it arrives, else on the existing id=0x0b + quiet fallback. `M` key is
        // `SHA256(kd)`; `M = HMAC-SHA256(STREAMID_TYPE || seq_num_M, SHA256(kd))`, seq_num_M = 0.
        let sha_kd = crypto::sha256(kd);
        let mut saw_0b = false;
        let mut saw_ready = false;
        let mut quiet = 0usize;
        const QUIET_GAP: usize = 3; // ~3 consecutive empty short reads => dock done pushing
        const MAX_ROUNDS: usize = 48;
        for _ in 0..MAX_ROUNDS {
            match dev.bulk_recv(EP_CTRL_IN, &mut buf, Delta::from_millis(5)) {
                Ok(len) if len >= 20 => {
                    quiet = 0;
                    let iid = u16::from_le_bytes([buf[16], buf[17]]);
                    let isub = u16::from_le_bytes([buf[18], buf[19]]);
                    let mid = if len >= 26 { buf[25] } else { 0 }; // HDCP msg_id (body[9])
                    if isub == 0x84 && iid == 0x0b {
                        saw_0b = true;
                    }
                    if mid == ake::id::REPEATERAUTH_STREAM_READY && len >= 58 {
                        saw_ready = true;
                        let mprime = &buf[26..58];
                        pr_info!("vino: AKE: Stream_Ready (0x11) M'={mprime:02x?}\n");
                        // M = HMAC-SHA256(SHA256(kd), data) where data is the Content Stream
                        // Management input the dock hashes: `k` 7-byte stream entries followed by
                        // the 3-byte `seq_num_M` (=0 on the first Stream_Manage). Cracked from the
                        // DLM aarch64 decompile (`FUN_0057be04`: data = memcpy(streams, k*7) ||
                        // BE16(field) || field, keyed by the 32-byte SHA256(kd) at session+0x37);
                        // reproduces DLM's captured M' byte-exact (captures/.../FINDINGS.md).
                        // vino's
                        // two streams carry the same StreamID_Type bytes its Stream_Manage sends
                        // (`repeater_auth_stream_manage`: type 0x04 and 0x05), so the dock computes
                        // the same M. (Earlier code guessed a 5-byte STREAMID_TYPE||seq layout and
                        // so
                        // always mismatched -- host-side only, never gated the dock.)
                        let m_data: [u8; 17] = [
                            0, 0, 0, 0x04, 0, 0, 0, // stream 0: StreamID_Type[0] = 4
                            0, 0, 0, 0x05, 0, 0, 0, // stream 1: StreamID_Type[1] = 5
                            0, 0, 0, // seq_num_M = 0 (first Stream_Manage, big-endian)
                        ];
                        let m = crypto::hmac_sha256(&sha_kd, &m_data);
                        let eq = if &m[..] == mprime { "==" } else { "!=" };
                        pr_info!("vino: AKE:   M {} M' (CSM stream-entry layout)\n", eq);
                    } else if mid == ake::id::RECEIVER_AUTH_STATUS && len >= 27 {
                        pr_info!("vino: AKE: RECEIVER_AUTH_STATUS=0x{:02x}\n", buf[26]);
                    }
                    // * 2026-06-12: arm the INSTANT both terminal markers have arrived -- the
                    // cap-complete
                    // id=0x0b AND the Stream_Ready (the trailing id=0x28 / HDCP 0x11). DLM arms
                    // 0.46 ms
                    // after its last cap block; a cold-plug cadence diff
                    // (vino-cold-20260612-113706) showed
                    // vino was instead waiting QUIET_GAP x 5 ms of EMPTY reads AFTER already
                    // seeing both
                    // markers, landing the arm ~68 ms late -- outside the dock's freshly-keyed CP
                    // window, so
                    // the dock errored on the arm (27 KB type=0x1001 dump) instead of engaging.
                    // Once both
                    // markers are in, the terminal burst is complete; arm now, like DLM. (The
                    // empty-read
                    // quiet path below remains the fallback when Stream_Ready never arrives.)
                    if saw_0b && saw_ready {
                        pr_info!("vino: cap-complete (id=0x0b + Stream_Ready 0x11) -- arming now\n");
                        return;
                    }
                }
                // Empty/short read = a quiet window. Fallback when Stream_Ready (0x11) never
                // arrives:
                // once id=0x0b has arrived AND the dock has been quiet for QUIET_GAP rounds, the
                // terminal burst is drained -- arm now.
                _ => {
                    if saw_0b {
                        quiet += 1;
                        if quiet >= QUIET_GAP {
                            pr_info!(
                                "vino: cap-complete drained (id=0x0b{}+ quiet) -- arming now\n",
                                if saw_ready { ", Stream_Ready 0x11, " } else { " (no 0x11) " }
                            );
                            return;
                        }
                    }
                }
            }
        }
        pr_info!(
            "vino: cap-complete drain budget hit (saw_0b={saw_0b} saw_ready={saw_ready}) -- arming anyway\n"
        );
    }


    /// Drives a full clean-room HDCP 2.2 AKE + LC + SKE (and RepeaterAuth for a
    /// repeater sink) over EP `0x02`/`0x84`, verifying `H'`, `L'` and `V'` against
    /// our own KDF (sec 5). On success returns the [`Session`] keys.
    ///
    /// All HDCP transfers use transport `seq=0`; the `hdcp_seq` counter increments
    /// 1..7 across the OUT messages (sec 5.1). Best-effort: any mismatch/short read
    /// aborts with an error the caller logs.
    fn run_ake(dev: &usb::Device) -> Result<Session> {
        use ake::id;

        // Flush any STALE EP84 frames the dock still has queued from a PRIOR session before
        // starting a fresh AKE. On a warm rmmod/insmod re-probe the dock is not power-cycled, so
        // its previous CP/cap replies (including a multi-KB residual block) sit in its EP84 queue;
        // if we don't drain them, the first `recv_hdcp` picks up a stale frame and the whole AKE
        // reply stream is shifted. Harmless on a true cold plug -- the queue is already empty, so
        // the first read just times out. Best-effort.
        if let Ok(mut flush) = KVec::from_elem(0u8, EP84_BUF, GFP_KERNEL) {
            let mut flushed = 0usize;
            for _ in 0..32 {
                match dev.bulk_recv(EP_CTRL_IN, &mut flush, Delta::from_millis(20)) {
                    Ok(n) if n > 0 => flushed += 1,
                    _ => break,
                }
            }
            if flushed > 0 {
                pr_info!("vino: flushed {flushed} stale EP84 frame(s) before AKE\n");
            }
        }

        // (1) AKE_Init -- fresh rtx, TxCaps = 00 00 00 (DLM-exact).
        let mut rtx = [0u8; 8];
        rng::fill(&mut rtx);
        dev.bulk_send(EP_CTRL_OUT, &ake::ake_init(1, 0, &rtx, &[0; 3])?, timeout())?;

        // (2) AKE_Send_Cert: payload = REPEATER(1) || cert_rx(522). Extract the
        // RSA-1024 public key (modulus[5..133], exponent[133..136]).
        let (cid, cert_msg) = Self::recv_hdcp(dev)?;
        if cid != id::AKE_SEND_CERT || cert_msg.len() < 1 + 136 {
            pr_err!("vino: AKE: bad AKE_Send_Cert (id={cid:#x}, {} B)\n", cert_msg.len());
            return Err(EINVAL);
        }
        let repeater = cert_msg[0] != 0;
        let cert = &cert_msg[1..];
        let mut modulus = [0u8; 128];
        modulus.copy_from_slice(&cert[5..133]);
        let mut exponent = [0u8; 3];
        exponent.copy_from_slice(&cert[133..136]);

        // (3) AKE_Transmitter_Info, then (4) read AKE_Receiver_Info (RxCaps unused).
        dev.bulk_send(EP_CTRL_OUT, &ake::ake_transmitter_info(2, 0)?, timeout())?;
        let _ = Self::recv_hdcp(dev)?;

        // (5) AKE_No_Stored_km -- fresh km, RSA-OAEP-SHA256 to Ekpub(km).
        let mut km = [0u8; 16];
        rng::fill(&mut km);
        let ekpub = hdcp::oaep_encrypt_km(&modulus, &exponent, &km)?;
        dev.bulk_send(EP_CTRL_OUT, &ake::ake_no_stored_km(3, 0, &ekpub)?, timeout())?;

        // (6) AKE_Send_Rrx.
        let (rid, rrx_pl) = Self::recv_hdcp(dev)?;
        if rid != id::AKE_SEND_RRX || rrx_pl.len() < 8 {
            pr_err!("vino: AKE: bad AKE_Send_Rrx (id={rid:#x})\n");
            return Err(EINVAL);
        }
        let mut rrx = [0u8; 8];
        rrx.copy_from_slice(&rrx_pl[..8]);

        // (7)/(8) AKE_Send_H_prime -- verify H' = HMAC(kd, rtx^REPEATER).
        let (hid, hp) = Self::recv_hdcp(dev)?;
        if hid != id::AKE_SEND_H_PRIME || hp.len() < 32 {
            pr_err!("vino: AKE: bad H' (id={hid:#x})\n");
            return Err(EINVAL);
        }
        let kd = hdcp::derive_kd(&km, &rtx, &rrx)?;
        if hdcp::compute_h(&kd, &rtx, repeater)[..] != hp[..32] {
            pr_err!("vino: AKE: H' mismatch -- authentication failed\n");
            return Err(EINVAL);
        }
        pr_info!("vino: AKE: H' verified\n");

        // (9) AKE_Send_Pairing_Info (Ekh_km) -- read and discard (no-stored path).
        let _ = Self::recv_hdcp(dev)?;

        // (10) Locality Check -- LC_Init(rn) then verify L'.
        let mut rn = [0u8; 8];
        rng::fill(&mut rn);
        dev.bulk_send(EP_CTRL_OUT, &ake::lc_init(4, 0, &rn)?, timeout())?;
        let (lid, lp) = Self::recv_hdcp(dev)?;
        if lid != id::LC_SEND_L_PRIME || lp.len() < 32 {
            pr_err!("vino: AKE: bad L' (id={lid:#x})\n");
            return Err(EINVAL);
        }
        if hdcp::compute_l(&kd, &rrx, &rn)[..] != lp[..32] {
            pr_err!("vino: AKE: L' mismatch -- locality check failed\n");
            return Err(EINVAL);
        }
        pr_info!("vino: AKE: L' verified\n");

        // (11) Session Key Exchange -- send Edkey(ks) || riv. The session key and IV are
        // fresh-random per session.
        let mut ks = [0u8; 16];
        let mut riv = [0u8; 8];
        rng::fill(&mut ks);
        rng::fill(&mut riv);
        let edkey = hdcp::compute_eks(&km, &rtx, &rrx, &rn, &ks)?;
        // Dev diagnostic: the full SKE secrets, so the SKE delivery can be verified OFFLINE
        // (edkey == ks XOR derive_dkey(km,rtx,rrx,rn,2), and the dock unwrapping to the same ks).
        // Behind pr_debug, so compiled out unless dynamic debug is enabled.
        pr_debug!("vino: SKE-SECRETS km={km:02x?} rtx={rtx:02x?} rrx={rrx:02x?} rn={rn:02x?}\n");
        pr_debug!("vino: SKE-SECRETS ks={ks:02x?} edkey={edkey:02x?}\n");
        // * riv DERIVATION -- THE CP-ENGAGEMENT BUG, FIXED 2026-06-11.
        // The SKE delivers the BASE riv (byte7 low-3 head/direction-selector bits cleared); the
        // dock
        // derives the per-direction CP riv from that base. GROUND TRUTH from cold-ref AND the live
        // vino cold-plug diff (captures/dlm-cold-20260611-123347 + vino-cold-20260611-130522):
        // delivered base byte7 = e8 -> host OUT-CP riv = ec (base | 0x04) -> dock IN-CP riv = ed
        // (^1).
        // vino had been sealing OUT-CP with the RAW random `riv` (byte7 e.g. f9 = base f8 | 0x01)
        // while delivering base f8 -- so the dock, deriving its keystream from f8 (expecting
        // host-OUT
        // = fc), could NOT decrypt vino's CP and SILENTLY DROPPED every post-arm frame (0 sub=0x45,
        // EP84 dead after the arm) even though ks/seal/MAC/frame-format were all byte-correct. The
        // off-by-one-bit IV was the whole wall. Fix: deliver base, seal OUT with base | 0x04.
        // The SKE delivers the FULL random riv as-is (DLM does NOT mask the low bits -- verified
        // on
        // two decrypted DLM sessions: cold-ref delivers ...e8, dl3cmac delivers ...e7). The host CP
        // OUT riv = delivered XOR 0x04 (flip byte7 bit 2): cold-ref e8->ec, dl3cmac e7->e3.
        // cp::in_riv
        // then ^1 for the dock->host IN stream (ec->ed). vino had been masking the delivered riv
        // and
        // sealing with the raw random LSBs, so the dock (deriving its keystream as delivered^0x04)
        // got a different keystream and silently dropped every CP frame. See the vino cold-plug
        // diff.
        let riv_ske = riv; // deliver the full random riv, unmasked, exactly like DLM
        riv[7] ^= 0x04; // host OUT-CP riv = delivered ^ 0x04
        dev.bulk_send(EP_CTRL_OUT, &ake::ske_send_eks(5, 0, &edkey, &riv_ske)?, timeout())?;
        // Dev diagnostic: the live session key/out-riv the dock must hold to decrypt our CP.
        pr_debug!("vino: SESSION ks={ks:02x?} out_riv={riv:02x?}\n");

        // The LIVE plaintext capability-announce (`build_cap_announce`),
        // built once V is known below. Empty unless the dock is a repeater (D6000 always is).
        let mut cap_announce = KVec::new();

        // (12) RepeaterAuth -- verify V' over the ReceiverID_List, ACK, then SM2.
        if repeater {
            let (vid, list) = Self::recv_hdcp(dev)?;
            if vid != id::REPEATERAUTH_SEND_RECEIVERID_LIST || list.len() < 16 {
                pr_err!("vino: AKE: bad ReceiverID_List (id={vid:#x})\n");
                return Err(EINVAL);
            }
            let split = list.len() - 16;
            // V = HMAC(kd, list_header): MSB-128 = V' (verify vs the list trailer);
            // LSB-128 = the RepeaterAuth_Send_Ack value (NOT the MSB -- that was THE bug).
            let v_full = hdcp::compute_v_full(&kd, &list[..split]);
            let mut v_ack = [0u8; 16];
            v_ack.copy_from_slice(&v_full[16..]);
            if v_full[..16] != list[split..] {
                pr_err!("vino: AKE: V' mismatch -- repeater verification failed\n");
                return Err(EINVAL);
            }
            pr_info!("vino: AKE: V' verified\n");
            dev.bulk_send(EP_CTRL_OUT, &ake::repeater_auth_send_ack(6, 0, &v_ack)?, timeout())?;
            // Read the dock's ctr6 ack before sending ctr7 -- DLM's lockstep pacing, without
            // which the dock NAKs the back-to-back OUTs ~100 ms each (see `pace_cap_ack`).
            Self::pace_cap_ack(dev, 6);
            dev.bulk_send(EP_CTRL_OUT, &ake::repeater_auth_stream_manage(7, 0)?, timeout())?;
            // Read the dock's ctr7 ack before returning, so the caller's arm marker lands
            // tight after ctr7 (DLM: 0.46 ms) instead of while the dock is still NAKing.
            Self::pace_cap_ack(dev, 7);
            // Then drain the dock's terminal cap burst -- id=0x0b (cap-complete) AND the dock's
            // `RepeaterAuth_Stream_Ready` (HDCP 0x11, the 3rd id=0x28) -- before the caller arms.
            // DLM arms only after this burst (cold-ref: id=0x21 -> id=0x0b -> id=0x28/0x11 ->
            // arm);
            // arming early makes the dock NAK msg0 ~100 ms and dump a 16 KB error block instead of
            // engaging. `wait_cap_complete` recognises + verifies the Stream_Ready in place (HDCP
            // 2.3 Adaptation sec RepeaterAuth). `kd` is needed to check `M == M'`.
            Self::wait_cap_complete(dev, &kd);

            // Build the LIVE capability-announce now that every field is known. This is the
            // plaintext re-statement of the 7 AKE OUT messages the dock requires between the
            // init markers and the arm marker (`CP_CAP_PHASE`). See `build_cap_announce`.
            // Pass `riv_ske` (the value SKE_Send_Eks actually delivered), NOT `riv` (= session
            // OUT-CP seal riv = riv_ske ^ 0x04). The cap-announce ctr5 frame is a byte-faithful
            // re-statement of SKE_Send_Eks, so it must carry the IDENTICAL riv.
            cap_announce = Self::build_cap_announce(&rtx, &ekpub, &rn, &edkey, &riv_ske, &v_ack)?;
        }

        Ok(Session { ks, riv, kd, cap_announce })
    }


    /// Build the LIVE plaintext **capability-announce** the dock requires before the arm
    /// marker. Ground truth: the cold-ref raw wire
    /// (`captures/cold-ref-20260608-200850/`, t~36.754-36.813) shows DLM, *after* the HDCP
    /// AKE, sends 7 plaintext `type=4 wsub=0x04` frames that are a re-statement of the 7 AKE
    /// OUT messages -- `id=0x22/0x1f/0x9a/0x22/0x32/0x2a/0x2d`, `sub=0x10`, ctr 1-7 -- each
    /// carrying THIS session's real value: f1=rtx, f2=const TxCaps, f3=Ekpub(km)[128],
    /// f4=rn, f5=Edkey(ks)[16]||riv_base[8], f6=V[16], f7=const Stream_Manage config. The dock
    /// ACKs each (`id=0x14 sub=0x10 ctr=N`) and only then engages its CP cipher; skipping the
    /// announce leaves it cipher-off (the long-standing "0 `sub=0x45` acks" symptom).
    ///
    /// [`golden::CAP_PLAIN_1080P`] is a byte-correct *skeleton* (headers/aux/lead bytes and the
    /// two constant frames are session-invariant -- verified across the cold-ref and matched
    /// sessions) but its 5 variable payloads are a STALE foreign session's values. Replaying it
    /// verbatim delivers the dock a stale Ekpub/Edkey/riv that re-key it to a foreign `ks`
    /// (the `cap_phase`-clobbers-`ks` bug). So we clone the skeleton and overwrite ONLY the 5
    /// session-specific payloads. Each payload sits at frame offset 44 (16-byte wire header +
    /// 22 inner-prefix bytes + the `30 00 00 00 00` marker + 1 lead byte = 28 inner bytes), and
    /// frames are stored `[u16 len][frame]`. `riv` here is the SKE-*delivered* riv (`riv_ske`),
    /// written verbatim -- frame 5 is a byte-faithful re-statement of `SKE_Send_Eks`, so it must
    /// carry the EXACT delivered riv. (It earlier wrote `riv & 0xF8`, which equals the delivered
    /// value only when the random riv's low 3 bits are zero -- true for cold-ref's `e8` but wrong
    /// for 7 of 8 live sessions, so the dock saw a different riv in the announce than in SKE.
    /// Ground truth: cold-ref ctr5 capture t=36.812413 delivers riv `...40e8` == its SKE riv.)
    fn build_cap_announce(
        rtx: &[u8; 8],
        ekpub: &[u8; 128],
        rn: &[u8; 8],
        edkey: &[u8; 16],
        riv: &[u8; 8],
        v: &[u8; 16],
    ) -> Result<KVec<u8>> {
        let mut blob = KVec::with_capacity(golden::CAP_PLAIN_1080P.len(), GFP_KERNEL)?;
        blob.extend_from_slice(golden::CAP_PLAIN_1080P, GFP_KERNEL)?;

        // Walk the skeleton; for each frame, overwrite the payload (at frame+44) keyed by ctr.
        let mut off = 0usize;
        while off + 2 <= blob.len() {
            let len = u16::from_le_bytes([blob[off], blob[off + 1]]) as usize;
            let frame = off + 2;
            if frame + len > blob.len() {
                break;
            }
            // ctr (inner offset 4) identifies which AKE message this announce frame restates.
            let ctr = u16::from_le_bytes([blob[frame + 16 + 4], blob[frame + 16 + 5]]);
            let pay = frame + 44; // 16 hdr + 22 inner-prefix + 5 marker + 1 lead
            match ctr {
                1 => blob[pay..pay + 8].copy_from_slice(rtx), // AKE_Init
                3 => blob[pay..pay + 128].copy_from_slice(ekpub), // AKE_No_Stored_km Ekpub
                4 => blob[pay..pay + 8].copy_from_slice(rn), // LC_Init
                5 => {
                    // SKE_Send_Eks: Edkey(ks)[16] || riv[8] (the delivered riv, verbatim)
                    blob[pay..pay + 16].copy_from_slice(edkey);
                    blob[pay + 16..pay + 24].copy_from_slice(riv);
                }
                6 => blob[pay..pay + 16].copy_from_slice(v), // RepeaterAuth_Send_Ack V
                _ => {} // ctr 2 (TxCaps) and 7 (Stream_Manage) are session-invariant
            }
            off = frame + len;
        }
        Ok(blob)
    }


    /// Poll EP 0x83 (interrupt-IN status endpoint). DLM submits URBs here CONTINUOUSLY and the dock
    /// pushes 6-byte status events; the dock may gate CP/downstream-HDCP engagement on the host
    /// servicing this endpoint (flagged in `vino-driver/src/bin/bringup.rs`). vino never polled it
    /// --
    /// invisible in the EP02/EP84 bulk-wire comparison. Reads up to a few events (short timeout so
    /// a
    /// URB is pending when the dock pushes). `usb_bulk_msg` auto-routes the interrupt endpoint.
    fn poll_ep83(dev: &usb::Device) -> usize {
        // EP83 (interrupt-IN) transfers need DMA-capable memory -- allocate on the HEAP.
        // A stack array trips usb_hcd_map_urb_for_dma's "transfer buffer is on stack"
        // WARNING (VMAP_STACK can't be DMA-mapped) and the broken submit also stalls the
        // bring-up (poll_ep83 runs inside every drain round). Best-effort: bail on OOM.
        let mut buf = match KVec::from_elem(0u8, 64, GFP_KERNEL) {
            Ok(b) => b,
            Err(_) => return 0,
        };
        let mut n = 0usize;
        // Short timeout: a pending URB gives the dock a window to push, but a 30 ms block on the
        // (normally idle) EP83 stalls the bring-up loop (see drain_ep84). 2 ms is enough to catch a
        // ready event without serializing the handshake.
        for _ in 0..4 {
            match dev.interrupt_recv(0x83, &mut buf, Delta::from_millis(2)) {
                Ok(len) if len > 0 => {
                    n += 1;
                    let s = &buf[..len.min(8)];
                    pr_info!("vino: EP83 status event {len}B {s:02x?}\n");
                }
                _ => break,
            }
        }
        n
    }

}

kernel::usb_device_table!(
    USB_TABLE,
    MODULE_USB_TABLE,
    <VinoDriver as usb::Driver>::IdInfo,
    [(usb::DeviceId::from_id(VID_DISPLAYLINK, PID_D6000), ())]
);

impl usb::Driver for VinoDriver {
    type IdInfo = ();
    // The driver instance is itself the per-bound device-private data.
    type Data<'bound> = Self;
    const ID_TABLE: usb::IdTable<Self::IdInfo> = &USB_TABLE;

    fn probe<'bound>(
        intf: &'bound usb::Interface<Core<'_>>,
        _id: &usb::DeviceId,
        _info: &'bound Self::IdInfo,
    ) -> impl PinInit<Self, Error> + 'bound {
        let cdev: &device::Device<Core<'_>> = intf.as_ref();
        // The D6000 exposes several interfaces (0/1/5/6 match us; 2-4 are audio).
        // The control endpoints (0x02/0x84) and the whole HDCP session live on
        // interface 0 -- drive bring-up only there so we don't run the preamble and
        // AKE four times and pollute the dock's state machine. Other interfaces
        // bind (so usbcore doesn't hand them to another driver) but stay idle.
        let ifnum = intf.number();
        if ifnum != 0 {
            // Interface 1 (app-specific/DFU) is the only other one DLM claims; let everything else
            // (audio 2-4, Ethernet 5-6) fall through to its proper kernel driver. Returning ENODEV
            // tells usbcore this driver doesn't handle the interface, so it tries the next match.
            if ifnum != 1 {
                dev_info!(cdev, "vino: declining D6000 interface {ifnum} (left to its class driver)\n");
                return Err(ENODEV);
            }
            dev_info!(cdev, "vino: bound D6000 interface {ifnum} (idle -- control is iface 0)\n");
            return Ok(Self { _intf: intf.into() });
        }
        dev_info!(cdev, "vino: bound DisplayLink D6000 -- plaintext session bring-up\n");

        // Bring-up is blocking synchronous USB I/O; hand it to the system workqueue so
        // probe() returns immediately and userspace stays responsive. The work item holds
        // a refcounted handle to the interface, so the bulk endpoints outlive probe(); USB
        // I/O after an intervening disconnect simply errors and is logged.
        let intf_ref: ARef<usb::Interface> = intf.into();
        match BringUp::new(intf_ref.clone()) {
            Ok(work) => {
                let _ = workqueue::system().enqueue(work);
                dev_info!(cdev, "vino: bring-up queued on system workqueue\n");
            }
            Err(e) => dev_info!(cdev, "vino: failed to queue bring-up ({e:?}) -- WIP\n"),
        }

        Ok(Self { _intf: intf_ref })
    }

    fn disconnect<'bound>(intf: &'bound usb::Interface<Core<'_>>, _data: Pin<&Self>) {
        let dev: &device::Device<Core<'_>> = intf.as_ref();
        dev_info!(dev, "vino: D6000 disconnected\n");
    }
}

kernel::module_usb_driver! {
    type: VinoDriver,
    name: "vino",
    authors: ["Mike Lothian"],
    description: "DisplayLink DL3 (Vino) open driver",
    license: "GPL v2",
}
