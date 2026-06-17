// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (C) 2026 Mike Lothian

//! Vino -- open in-kernel Rust driver for DisplayLink DL3 docks (Dell D6000, ...).
//!
//! This is an `[RFC]` work-in-progress, posted to ask for help. It is a clean-room
//! reverse-engineered replacement for the proprietary DisplayLinkManager userspace
//! daemon + the EVDI kernel module, written natively in Rust against the in-tree USB,
//! crypto and DRM/KMS bindings.
//!
//! This first patch is the skeleton: it binds the dock over USB and runs the plaintext
//! connect handshake (the control-request preamble and the three bulk init messages over
//! the Rust USB bulk + control transfer API). The HDCP 2.2 AKE, the AES-CTR/AES-CMAC
//! control plane, the Vino codec and the DRM/KMS sink are added in the following patches.
//!
//! Device: VID 0x17e9 (DisplayLink) / PID 0x6006 (Dell Universal Dock D6000).

use kernel::{
    alloc::flags::GFP_KERNEL,
    device::{self, Core},
    error::code::ENODEV,
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

/// USB transfer timeout used during bring-up.
fn timeout() -> Delta {
    Delta::from_millis(1000)
}

mod proto;

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
        // WIP scaffold: attempt the plaintext bring-up. Bind regardless of the outcome --
        // there is no display path yet (the HDCP AKE, control plane and DRM sink land in
        // the following patches).
        match VinoDriver::bring_up(dev) {
            Ok(()) => dev_info!(cdev, "vino: plaintext session init OK\n"),
            Err(e) => dev_info!(cdev, "vino: session init incomplete ({e:?}) -- WIP\n"),
        }
    }
}

impl VinoDriver {
    /// Plaintext session bring-up (sec 4): control-request preamble then the three
    /// bulk init messages, reading the single ACK. Best-effort during scaffold
    /// bring-up -- errors are logged, not fatal.
    fn bring_up(dev: &usb::Device) -> Result {
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
