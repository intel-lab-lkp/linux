// SPDX-License-Identifier: GPL-2.0

//! Phase 3 (DRM/KMS sink): register a real `struct drm_device` with a full atomic
//! mode-setting pipeline so the dock appears to userspace as a `card`/`renderD` node
//! that can be `drmModeSetCrtc`'d. A hand-rolled atomic pipeline: one CRTC driven by a
//! primary plane (`primary_atomic_update` -> EP08 scanout) and a cursor plane
//! (`cursor_atomic_update` -> cursor CP), a virtual encoder, and a virtual connector whose
//! mode list comes from the dock's real EDID (falling back to 1080p), with GEM-shmem dumb
//! buffers and `drm_gem_fb_create` framebuffers. (Earlier this was `drm_simple_display_pipe`,
//! swapped out because that helper is primary-plane-only and can't carry a cursor plane.)
//! The scanout/cursor writes are gated on the CP-arming blocker (see `docs/BLOCKER.md`). The
//! KMS C bindings are pulled in by
//! `patches/drm/0001` (bindings_helper.h headers + the `Driver::FEAT_MODESET` /
//! `Driver::FEAT_ATOMIC` flags + a public `Device::as_raw`); see `patches/README.md`.

use core::ptr;
use kernel::{
    bindings, drm,
    error::{
        code::{EINVAL, ENOMEM},
        to_result,
    },
    prelude::*,
    sync::{aref::ARef, new_mutex, Mutex},
    types::Opaque,
};

/// Fallback connector mode advertised by `get_modes` when the dock has not delivered a real
/// downstream EDID yet. The live scanout geometry follows the actual framebuffer/negotiated
/// mode (see `scanout_one`), so this is only the no-EDID default, not a hard scanout limit.
const FALLBACK_W: i32 = 1920;
const FALLBACK_H: i32 = 1080;

/// The DRM driver marker type.
pub(super) struct VinoDrmDriver;

/// Convenience alias for our concrete `drm::Device`.
pub(super) type VinoDrmDevice = drm::Device<VinoDrmDriver>;

/// `DRM_FORMAT_XRGB8888` (`fourcc_code('X','R','2','4')`); the dock scans out 32bpp.
const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258;
/// `DRM_FORMAT_ARGB8888` (`fourcc_code('A','R','2','4')`); the cursor sprite carries alpha.
const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241;
/// Primary-plane format list (opaque 32bpp scanout).
static PRIMARY_FORMATS: [u32; 1] = [DRM_FORMAT_XRGB8888];
/// Cursor-plane format list (alpha sprite). ARGB8888 little-endian memory order is
/// `B,G,R,A` per pixel -- already the BGRA byte layout `cp::cursor_image` wants.
static CURSOR_FORMATS: [u32; 1] = [DRM_FORMAT_ARGB8888];
/// Hardware cursor sprite size (the dock cursor is 64x64; `DRM_CAP_CURSOR_WIDTH/HEIGHT`).
const CURSOR_SIZE: u32 = 64;
/// `GAMMA_LUT` size advertised on the CRTC. 256 entries matches the 8-bit scanout channels;
/// the LUT is applied host-side in the scanout conversion (see `read_gamma_lut`).
const GAMMA_SIZE: u32 = 256;

/// Per-mode pixel-clock ceiling (kHz) for a single head -- about 4K@60 (CEA 594 MHz).
/// `mode_valid` prunes any single mode above this from a connector's advertised list.
const MAX_HEAD_CLOCK_KHZ: i32 = 600_000;
/// Combined pixel-clock budget (kHz) summed over all *active* heads -- the dock's DL3 link
/// ceiling. This is a deliberately conservative *raw* pixel-rate proxy (DisplayLink compresses
/// the stream, so the true USB budget is higher and content-dependent; the WHT codec exists for
/// the tight cases). At 1 GHz it admits one 4K@60, 4K@60 + QHD@60, or dual-QHD@60, and rejects
/// dual-4K -- matching the D6000's real multi-monitor envelope. Tune to taste / per dock
/// model.
const MAX_TOTAL_CLOCK_KHZ: i64 = 1_000_000;

/// Mutable scanout state, guarded because the atomic `update` callback may run
/// concurrently with itself across heads. Holds the stateful Vino encoder (created
/// lazily on the first flip, once the buffer geometry is known) and the EP08 frame
/// sequence counter.
pub(super) struct ScanoutState {
    enc: Option<super::video::Encoder>,
    /// Reusable `width*height` RGB565 conversion buffer, allocated once alongside `enc`.
    /// Previously `encode_and_send` did a fresh `KVec::with_capacity(w*h)` on every pageflip;
    /// at 1080p that is a ~4 MiB *contiguous* kmalloc (order 11), above the allocator's limit,
    /// so the page allocator WARNed and returned `ENOMEM` every frame. vmalloc-backed +
    /// persistent: virtually-contiguous (no high-order page need) and allocated once.
    cur: VVec<u16>,
    seq: u32,
    /// Geometry (`width`, `height`) the encoder/`cur` were allocated for. The scanout follows
    /// the live framebuffer size, so a mode switch re-allocates them when this no longer
    /// matches.
    dims: (usize, usize),
    /// The [`super::cp::Timing`] of the mode the compositor last enabled on the CRTC, captured
    /// in [`crtc_atomic_enable`] via [`super::cp::timing_from_drm_mode`]. This is the
    /// multi-mode hook:
    /// userspace can pick *any* mode from the EDID-derived list and the chosen
    /// `drm_display_mode`
    /// is recorded here so the live mode-set CP message reflects it (rather than always the
    /// EDID-preferred timing). The CP send itself is gated on the engagement wall + session
    /// plumbing (see the doc note in `crtc_atomic_enable`).
    active_timing: Option<super::cp::Timing>,
    /// Size of the last EP08 frame produced, used to pre-reserve the next frame's
    /// buffer. The encoded stream size is stable frame-to-frame (it tracks the damage
    /// area), so seeding `KVec::with_capacity` from it makes the encode grow the buffer
    /// at most once instead of reallocating repeatedly as runs are appended.
    hint: usize,
}

/// The live CP session the bring-up work item publishes once the dock engages the cipher
/// (`acks > 0`), so the KMS callbacks can seal+send runtime CP messages -- a mode-set when the
/// compositor switches mode, a cursor message on pointer motion -- that continue the SAME
/// keystream the bring-up setup left off at. `wire_seq` is the AES-CTR block counter (advanced
/// by the content blocks of each send; the appended Dl3Cmac tag is not part of the keystream)
/// and `counter` the dock-echoed inner CP counter. Both advance per send under the mutex.
pub(super) struct CpLink {
    ks: [u8; 16],
    riv: [u8; 8],
    wire_seq: u32,
    counter: u16,
}

/// Number of display heads the D6000 dock drives. The protocol routes video by endpoint
/// (head 0 -> EP 0x08, head 1 -> EP 0x0a; heads 2/3 -> 0x0b/0x0c are documented but their CP
/// riv / EDID-request encoding is unconfirmed, so only 2 heads are wired). The DL3 CP stream
/// selects the head via the riv `byte0 ^ 0x80` (head 0 base / head 1 flipped).
pub(super) const NHEADS: usize = 2;
/// Per-head video bulk-OUT endpoint (`PROTOCOL.md`). Index by [`Head::index`].
const HEAD_EP: [u8; NHEADS] = [0x08, 0x0a];

/// One display head: its own CRTC + primary plane (scanout) + cursor plane + encoder +
/// connector, plus per-head scanout state and cached monitor EDID. The vtables are shared
/// across heads (in [`VinoDrmData`]); the callbacks recover the head from the C object
/// pointer. All C objects are zeroed at init and filled by [`kms_init`].
#[pin_data]
pub(super) struct Head {
    /// 0-based head index. Selects the video EP ([`HEAD_EP`]) and the CP riv (head 1 flips
    /// the riv `byte0`); see the scanout EP and [`VinoDrmData::send_cp`].
    index: u8,
    /// One-shot: this head's cursor `create` (sprite dimensions) was sent before its first
    /// image upload (per head -- the global one would skip head 1's create).
    cursor_primed: core::sync::atomic::AtomicBool,
    /// Last DDC/CI brightness/contrast (0..=100) set on this head's monitor via the connector
    /// properties; replayed on DPMS-on. Stored here (not in connector state) because DDC/CI is
    /// a side-band action on the physical monitor, not part of the atomic scanout pipeline.
    brightness: core::sync::atomic::AtomicU32,
    contrast: core::sync::atomic::AtomicU32,
    #[pin]
    scanout: Mutex<ScanoutState>,
    /// This head's downstream-monitor EDID (`None` until the CP channel delivers it). Only
    /// head 0's EDID is read during bring-up; per-head EDID requests are unconfirmed, so
    /// head 1 falls back to a fixed CVT mode in `get_modes`.
    #[pin]
    cached_edid: Mutex<Option<KVec<u8>>>,
    #[pin]
    crtc: Opaque<bindings::drm_crtc>,
    #[pin]
    primary: Opaque<bindings::drm_plane>,
    #[pin]
    cursor: Opaque<bindings::drm_plane>,
    #[pin]
    encoder: Opaque<bindings::drm_encoder>,
    #[pin]
    connector: Opaque<bindings::drm_connector>,
}

// SAFETY: as for `VinoDrmData` below -- the embedded C KMS objects are written only during
// single-threaded probe and thereafter serialised by the DRM core's own locks.
unsafe impl Send for Head {}
// SAFETY: see the `Send` impl above.
unsafe impl Sync for Head {}

impl Head {
    fn new(index: u8) -> impl PinInit<Self, Error> {
        fn z<T>() -> impl PinInit<Opaque<T>, Error> {
            // SAFETY: an all-zero C KMS object is a valid starting point (all callback
            // pointers NULL); `kms_init` populates the rest via raw pointers under `unsafe`.
            Opaque::try_ffi_init(|p: *mut T| {
                unsafe { ptr::write_bytes(p, 0, 1) };
                Ok(())
            })
        }
        try_pin_init!(Self {
            index,
            cursor_primed: core::sync::atomic::AtomicBool::new(false),
            brightness: core::sync::atomic::AtomicU32::new(100),
            contrast: core::sync::atomic::AtomicU32::new(100),
            scanout <- new_mutex!(ScanoutState {
                enc: None,
                cur: VVec::new(),
                seq: 0,
                dims: (0, 0),
                active_timing: None,
                hint: 0,
            }),
            cached_edid <- new_mutex!(Option::<KVec<u8>>::None),
            crtc <- z(),
            primary <- z(),
            cursor <- z(),
            encoder <- z(),
            connector <- z(),
        })
    }

    /// This head's video bulk-OUT endpoint.
    fn video_ep(&self) -> u8 {
        HEAD_EP[self.index as usize]
    }

    /// Fire a hotplug on this head's connector so the compositor re-probes [`detect`].
    fn fire_hotplug(&self) {
        // SAFETY: called after `drm_dev_register`; the embedded connector is initialised
        // and its `dev` is our live drm_device. Safe from process context.
        unsafe {
            let dev = (*self.connector.get()).dev;
            if !dev.is_null() {
                bindings::drm_kms_helper_hotplug_event(dev);
            }
        }
    }
}

/// DRM device-private data. Holds [`NHEADS`] display [`Head`]s (each a CRTC + primary +
/// cursor plane + encoder + connector) and the shared KMS vtables inline, so they keep
/// stable addresses for the device's lifetime. All C objects zeroed at init; filled by
/// [`kms_init`]. Also keeps the bound USB interface (to reach the video EPs) and the engaged
/// CP session.
#[pin_data]
pub(super) struct VinoDrmData {
    intf: ARef<super::usb::Interface>,
    /// The engaged CP session for runtime KMS-driven sends (`None` until
    /// [`VinoDrmData::publish_session`]). See [`CpLink`] and [`VinoDrmData::send_cp`].
    #[pin]
    cp_link: Mutex<Option<CpLink>>,
    #[pin]
    head0: Head,
    #[pin]
    head1: Head,
    // Shared vtables (one set for all heads; the callbacks recover the head from the C
    // object pointer). One `drm_plane_funcs` for both planes; per-plane helper funcs because
    // the primary's `atomic_update` scans out while the cursor's sends cursor CP.
    #[pin]
    conn_funcs: Opaque<bindings::drm_connector_funcs>,
    #[pin]
    conn_helper: Opaque<bindings::drm_connector_helper_funcs>,
    #[pin]
    crtc_funcs: Opaque<bindings::drm_crtc_funcs>,
    #[pin]
    crtc_helper: Opaque<bindings::drm_crtc_helper_funcs>,
    #[pin]
    plane_funcs: Opaque<bindings::drm_plane_funcs>,
    #[pin]
    primary_helper: Opaque<bindings::drm_plane_helper_funcs>,
    #[pin]
    cursor_helper: Opaque<bindings::drm_plane_helper_funcs>,
    #[pin]
    encoder_funcs: Opaque<bindings::drm_encoder_funcs>,
    #[pin]
    mode_cfg_funcs: Opaque<bindings::drm_mode_config_funcs>,
    /// The custom 0..=100 connector range properties for DDC/CI brightness/contrast, created
    /// and attached in [`kms_init`]. Stored so the connector `atomic_set_property` /
    /// `atomic_get_property` callbacks can identify them by pointer. Written once during
    /// single-threaded probe, read-only thereafter (`AtomicPtr` for `Sync` without `unsafe`).
    brightness_prop: core::sync::atomic::AtomicPtr<bindings::drm_property>,
    contrast_prop: core::sync::atomic::AtomicPtr<bindings::drm_property>,
}

// SAFETY: the embedded C KMS objects are written only during single-threaded
// `probe()` (before `drm_dev_register`), and thereafter are owned and serialised by
// the DRM core under its own modeset/atomic locks -- Rust never aliases them again.
// This is the conventional assertion for drivers embedding C KMS state until the
// kernel grows safe Rust KMS abstractions.
unsafe impl Send for VinoDrmData {}
// SAFETY: see the `Send` impl above.
unsafe impl Sync for VinoDrmData {}

impl VinoDrmData {
    /// Zero-initialise all embedded C objects (so each `Option<fn>` vtable slot is
    /// `None`); [`kms_init`] then fills in only the callbacks we implement. `intf`
    /// is the bound USB interface, kept so the scanout path can reach EP08.
    pub(super) fn new(intf: ARef<super::usb::Interface>) -> impl PinInit<Self, Error> {
        fn z<T>() -> impl PinInit<Opaque<T>, Error> {
            // SAFETY: an all-zero C KMS object / funcs table is a valid starting
            // point (all callback pointers NULL); the `_init` helpers populate the
            // rest, and we only read it back through raw pointers under `unsafe`.
            Opaque::try_ffi_init(|p: *mut T| {
                unsafe { ptr::write_bytes(p, 0, 1) };
                Ok(())
            })
        }
        try_pin_init!(Self {
            intf,
            cp_link <- new_mutex!(Option::<CpLink>::None),
            head0 <- Head::new(0),
            head1 <- Head::new(1),
            conn_funcs <- z(),
            conn_helper <- z(),
            crtc_funcs <- z(),
            crtc_helper <- z(),
            plane_funcs <- z(),
            primary_helper <- z(),
            cursor_helper <- z(),
            encoder_funcs <- z(),
            mode_cfg_funcs <- z(),
            brightness_prop: core::sync::atomic::AtomicPtr::new(ptr::null_mut()),
            contrast_prop: core::sync::atomic::AtomicPtr::new(ptr::null_mut()),
        })
    }

    /// The display heads, in index order.
    fn heads(&self) -> [&Head; NHEADS] {
        [&self.head0, &self.head1]
    }

    /// Recover the [`Head`] that owns a given C KMS object, by pointer identity. Used by the
    /// connector/CRTC/plane callbacks (which receive a raw C pointer) to find their head.
    fn head_by_connector(&self, c: *mut bindings::drm_connector) -> Option<&Head> {
        self.heads().into_iter().find(|h| h.connector.get() == c)
    }
    fn head_by_crtc(&self, c: *mut bindings::drm_crtc) -> Option<&Head> {
        self.heads().into_iter().find(|h| h.crtc.get() == c)
    }
    fn head_by_primary(&self, p: *mut bindings::drm_plane) -> Option<&Head> {
        self.heads().into_iter().find(|h| h.primary.get() == p)
    }
    fn head_by_cursor(&self, p: *mut bindings::drm_plane) -> Option<&Head> {
        self.heads().into_iter().find(|h| h.cursor.get() == p)
    }

    /// Cache the dock's EDID (read during probe) on head 0 for [`get_modes`] to install, then
    /// fire a hotplug so the compositor re-probes the connector -- which now reports connected
    /// (see [`detect`]) and exposes the monitor's real mode list. Only head 0's downstream EDID
    /// is read during bring-up (per-head EDID requests are unconfirmed).
    pub(super) fn set_edid(&self, blob: KVec<u8>) {
        *self.head0.cached_edid.lock() = Some(blob);
        self.head0.fire_hotplug();
    }

    /// Fire a hotplug on every head's connector so the compositor re-probes [`detect`] -- used
    /// after the bring-up work item completes to expose the live-scanout outputs.
    pub(super) fn fire_hotplug(&self) {
        for h in self.heads() {
            h.fire_hotplug();
        }
    }

    /// Publish the engaged CP session so the KMS callbacks can send runtime CP messages.
    /// Called once by the bring-up work item after the dock acks (`acks > 0`).
    /// `wire_seq`/`counter` are the next free values past the bring-up CP setup.
    pub(super) fn publish_session(&self, ks: &[u8; 16], riv: &[u8; 8], wire_seq: u32, counter: u16) {
        *self.cp_link.lock() = Some(CpLink { ks: *ks, riv: *riv, wire_seq, counter });
    }

    /// Seal and send one interactive CP message on EP02 for head `head_index`, advancing the
    /// session keystream. The DL3 CP stream selects the head via the riv `byte0 ^ 0x80` (head 0
    /// base, head 1 flipped). `build(counter)` produces the inner CP message for the
    /// dock-echoed `counter` it is handed (e.g. [`super::cp::set_mode`]); `tag_reserved`
    /// trailing bytes are dropped before the live Dl3Cmac is appended (set-mode reserves a
    /// 16-byte placeholder; messages with no placeholder pass 0). Returns `Ok(())` as a
    /// **no-op when CP is not engaged**. The `cp_link` mutex serialises concurrent KMS
    /// callbacks. Runs from the atomic-commit context (same as the scanout), so the blocking
    /// `bulk_send` is fine. NOTE: head 1's `wire_seq`/counter sharing with head 0 is an
    /// assumption (both share `cp_link`); the riv differs so the keystreams differ. Unconfirmed
    /// (CP-wall-gated) -- revisit when the dock engages.
    pub(super) fn send_cp(
        &self,
        head_index: u8,
        id: u16,
        tag_reserved: usize,
        build: impl FnOnce(u16) -> Result<KVec<u8>>,
    ) -> Result {
        let mut guard = self.cp_link.lock();
        // `&mut *guard` forces the guard's `DerefMut` to `&mut Option<CpLink>` so `as_mut`
        // resolves to `Option::as_mut` (the guard has its own inherent `as_mut`).
        let Some(link) = (&mut *guard).as_mut() else {
            return Ok(()); // CP not engaged -- nothing to send
        };
        // Head select: head 1's CP stream flips the riv byte0 (see `decode_any`/CP-HANDSHAKE).
        let mut riv = link.riv;
        if head_index == 1 {
            riv[0] ^= 0x80;
        }
        let msg = build(link.counter)?;
        let content = &msg[..msg.len().saturating_sub(tag_reserved)];
        let frame = super::cp::seal_interactive(&link.ks, &riv, id, link.wire_seq, content)?;
        let dev: &super::usb::Device = self.intf.as_ref();
        dev.bulk_send(super::EP_CTRL_OUT, &frame, super::timeout())?;
        // Advance the AES-CTR block counter by the content blocks only (the appended Dl3Cmac
        // tag is sent in clear, not keystreamed) and bump the dock-echoed inner counter.
        link.wire_seq = link.wire_seq.wrapping_add(((content.len() + 15) / 16) as u32);
        link.counter = link.counter.wrapping_add(1);
        Ok(())
    }

    /// Push a DDC/CI Set-VCP write to a head's downstream monitor (brightness, contrast or
    /// DPMS power). Wraps [`super::cp::ddc_set_vcp`] (`id=0x15`); a no-op until the cipher is
    /// engaged. Used by the brightness/contrast connector properties and by DPMS in the CRTC
    /// enable/disable callbacks.
    pub(super) fn set_vcp(&self, head_index: u8, vcp: u8, value: u16) -> Result {
        self.send_cp(head_index, 0x15, 0, |ctr| super::cp::ddc_set_vcp(ctr, vcp, value))
    }
}

/// GEM object inner data. Empty: the shmem-backed `drm::gem::shmem::Object` (which
/// wires `drm_gem_shmem_dumb_create`, so userspace `DRM_IOCTL_MODE_CREATE_DUMB`
/// works) is enough until the EP08 scanout path consumes the framebuffers.
#[pin_data]
pub(super) struct VinoObject {}

impl drm::gem::DriverObject for VinoObject {
    type Driver = VinoDrmDriver;
    type Args = ();

    fn new<Ctx: drm::DeviceContext>(
        _dev: &drm::Device<VinoDrmDriver, Ctx>,
        _size: usize,
        _args: (),
    ) -> impl PinInit<Self, Error> {
        try_pin_init!(VinoObject {})
    }
}

/// Per-open DRM client state. Empty of driver data, but its lifetime is used to
/// pin the module for the duration of an open DRM file (see [`VinoDrmFile::open`]).
#[pin_data(PinnedDrop)]
pub(super) struct VinoDrmFile {}

impl drm::file::DriverFile for VinoDrmFile {
    type Driver = VinoDrmDriver;

    fn open(_dev: &drm::Device<Self::Driver>) -> Result<Pin<KBox<Self>>> {
        let file = KBox::try_pin_init(try_pin_init!(Self {}), GFP_KERNEL)?;
        // Pin this module while a DRM file is open. The Rust DRM `file_operations`
        // are built with `owner = NULL` (drm/gem/mod.rs `create_fops`), so the DRM
        // core's `try_module_get(fops->owner)` on open is a no-op: an open card fd
        // does NOT keep the driver loaded. Unloading vino (rmmod, or USB teardown at
        // shutdown) while a compositor still holds `/dev/dri/cardN` then frees the
        // module's `.rodata` -- where the fops live -- under that open fd, so the next
        // ioctl/close dereferences freed memory and oopses the kernel (observed: KWin
        // UAF in `__x64_sys_ioctl` / `put_files_struct`, "recursive fault, reboot
        // needed"). Take an explicit module reference here, released 1:1 in
        // `PinnedDrop` (run by `postclose_callback` on file close), to restore the
        // pin the NULL `fops.owner` drops. Remove once the binding sets `fops.owner`.
        // SAFETY: we are executing inside this module's own DRM `open` callback, so
        // the module is live; taking an extra reference via `__module_get` is sound.
        unsafe { bindings::__module_get(crate::THIS_MODULE.as_ptr()) };
        Ok(file)
    }
}

#[pinned_drop]
impl PinnedDrop for VinoDrmFile {
    fn drop(self: Pin<&mut Self>) {
        // Release the module reference taken in `open` (balanced one-per-open-file).
        // SAFETY: balances the `__module_get` in `open`; `THIS_MODULE` is valid for
        // the lifetime of the module.
        unsafe { bindings::module_put(crate::THIS_MODULE.as_ptr()) };
    }
}

const INFO: drm::DriverInfo = drm::DriverInfo {
    major: 0,
    minor: 1,
    patchlevel: 0,
    name: c"vino",
    desc: c"DisplayLink DL3 (Dell D6000) DRM driver",
};

#[vtable]
impl drm::Driver for VinoDrmDriver {
    type Data = VinoDrmData;
    type File = VinoDrmFile;
    type Object<Ctx: drm::DeviceContext> = drm::gem::shmem::Object<VinoObject, Ctx>;

    const INFO: drm::DriverInfo = INFO;
    // Atomic KMS driver (CRTC/plane/connector via the simple display pipe).
    // Mirrors the FEAT_RENDER idiom added by patches/drm/0001.
    const FEAT_MODESET: bool = true;
    const FEAT_ATOMIC: bool = true;

    // No driver-private ioctls (GEM/dumb + KMS handled by the DRM core).
    kernel::declare_drm_ioctls! {}
}

// ---- KMS C callbacks ------------------------------------------------------

/// Install a real EDID blob on the connector via the standard DRM EDID
/// infrastructure and return the number of modes added (0 on failure). This
/// reuses the kernel helpers -- no synthetic EDID. See CONTROL-PLANE.md.
fn install_edid(connector: *mut bindings::drm_connector, blob: &[u8]) -> i32 {
    // SAFETY: `blob` is a valid byte buffer; `drm_edid_alloc` copies it.
    let edid = unsafe { bindings::drm_edid_alloc(blob.as_ptr().cast(), blob.len()) };
    if edid.is_null() {
        return 0;
    }
    // SAFETY: `connector` is valid during probe; `edid` is freshly allocated above.
    unsafe { bindings::drm_edid_connector_update(connector, edid) };
    // SAFETY: connector valid; adds the EDID-derived modes, returns the count.
    let n = unsafe { bindings::drm_edid_connector_add_modes(connector) };
    // SAFETY: `edid` was allocated by `drm_edid_alloc` and is no longer needed.
    unsafe { bindings::drm_edid_free(edid) };
    n
}

/// Connector `.mode_valid`: reject any single mode whose pixel clock exceeds the per-head
/// ceiling ([`MAX_HEAD_CLOCK_KHZ`], ~4K@60), so the compositor never offers an over-spec mode
/// on
/// one head. The *combined* across-heads budget is enforced separately at commit by
/// [`vino_atomic_check`].
unsafe extern "C" fn mode_valid(
    _connector: *mut bindings::drm_connector,
    mode: *const bindings::drm_display_mode,
) -> bindings::drm_mode_status {
    // SAFETY: `mode` is a valid drm_display_mode for the duration of the call.
    let clock = unsafe { (*mode).clock };
    if clock > MAX_HEAD_CLOCK_KHZ {
        bindings::drm_mode_status_MODE_CLOCK_HIGH
    } else {
        bindings::drm_mode_status_MODE_OK
    }
}

/// `mode_config.funcs.atomic_check`: run the standard atomic checks, then reject the commit if
/// the **combined** pixel clock of all active heads would exceed the dock's USB/DL3 budget
/// ([`MAX_TOTAL_CLOCK_KHZ`]) -- e.g. two simultaneous 4K modes. For each head, the proposed
/// (new) CRTC state is used when the head is part of this commit, else its current committed
/// state; only `enable && active` heads count.
unsafe extern "C" fn vino_atomic_check(
    dev: *mut bindings::drm_device,
    state: *mut bindings::drm_atomic_commit,
) -> i32 {
    // SAFETY: `dev`/`state` are valid for the duration of the atomic check.
    let rc = unsafe { bindings::drm_atomic_helper_check(dev, state) };
    if rc != 0 {
        return rc;
    }
    // SAFETY: `dev` is our live, registered drm_device.
    let data: &VinoDrmData = unsafe { VinoDrmDevice::from_raw(dev) };
    let mut total_khz: i64 = 0;
    for head in data.heads() {
        let crtc = head.crtc.get();
        // SAFETY: read-only new-state accessor (a `rust_helper`, exposed without the prefix);
        // NULL when this head is not in the commit -- then fall back to its current state.
        let mut cs = unsafe { bindings::drm_atomic_get_new_crtc_state(state, crtc) };
        if cs.is_null() {
            // SAFETY: `crtc` is initialised; `.state` is its current committed state (or NULL).
            cs = unsafe { (*crtc).state };
        }
        if cs.is_null() {
            continue;
        }
        // SAFETY: `cs` is a live drm_crtc_state.
        let (enable, active, clock) = unsafe { ((*cs).enable, (*cs).active, (*cs).mode.clock) };
        if enable && active {
            total_khz += clock as i64;
        }
    }
    if total_khz > MAX_TOTAL_CLOCK_KHZ {
        pr_warn!(
            "vino: modeset rejected -- combined {total_khz} kHz pixel clock over the {} kHz dock budget\n",
            MAX_TOTAL_CLOCK_KHZ
        );
        return EINVAL.to_errno();
    }
    0
}

/// Connector `.get_modes`: install the dock's real EDID (read during probe) when
/// available; otherwise fall back to a single 1920x1080@60 CVT mode. Reading the
/// real EDID gives the true monitor name/size and its native mode list (see the
/// EDID Read path); the fallback keeps the connector usable when nothing is
/// plugged into the dock or the CP channel has not yet delivered the EDID.
unsafe extern "C" fn get_modes(connector: *mut bindings::drm_connector) -> i32 {
    // SAFETY: `connector` is a valid, initialised connector during probe.
    let dev = unsafe { (*connector).dev };
    // SAFETY: `dev` is our live, registered drm_device.
    let ddev = unsafe { VinoDrmDevice::from_raw(dev) };
    let data: &VinoDrmData = ddev;
    if let Some(head) = data.head_by_connector(connector) {
        let guard = head.cached_edid.lock();
        if let Some(blob) = guard.as_ref() {
            let n = install_edid(connector, blob);
            if n > 0 {
                return n;
            }
        }
    }
    // Fallback: single FALLBACK_W x FALLBACK_H @60 CVT mode, marked preferred.
    // SAFETY: `dev` is a valid drm_device; drm_cvt_mode allocates a mode.
    let mode = unsafe {
        bindings::drm_cvt_mode(dev, FALLBACK_W, FALLBACK_H, 60, false, false, false)
    };
    if mode.is_null() {
        return 0;
    }
    // SAFETY: `mode` is freshly allocated and owned by the connector after add.
    unsafe { bindings::drm_mode_probed_add(connector, mode) };
    // SAFETY: connector is valid; set the fallback mode as preferred.
    unsafe { bindings::drm_set_preferred_mode(connector, FALLBACK_W, FALLBACK_H) };
    1
}

/// Connector `.detect`: report **disconnected** until the dock's downstream EDID has
/// actually been read over the CP channel, then **connected**. A virtual connector
/// that always reports connected makes the compositor light up a phantom output it
/// cannot drive -- no pixels reach the dock until the CP/EP08 path is up -- which froze
/// KWin on plug (SSH stayed alive; unplug recovered it). Gating on a real EDID mirrors
/// how `gud`/`udl` report monitor presence; [`VinoDrmData::set_edid`] fires a hotplug
/// so the compositor re-probes and enables the output once the EDID arrives.
unsafe extern "C" fn detect(
    connector: *mut bindings::drm_connector,
    _force: bool,
) -> bindings::drm_connector_status {
    // SAFETY: `connector` is a valid connector embedded in our DRM device-private.
    let dev = unsafe { (*connector).dev };
    // SAFETY: `dev` is our live, registered drm_device.
    let ddev = unsafe { VinoDrmDevice::from_raw(dev) };
    let data: &VinoDrmData = ddev;
    let live_ready = super::CP_ENGAGED.load(core::sync::atomic::Ordering::SeqCst);
    let has_edid = data
        .head_by_connector(connector)
        .is_some_and(|h| h.cached_edid.lock().is_some());
    if has_edid || live_ready {
        // Force-connect (with the get_modes 1080p fallback) so a compositor drives the CRTC and
        // `primary_atomic_update` fires live frames -- but only once CP is engaged (not merely
        // after
        // bring-up): connecting the output makes the compositor push EP08 video, and doing that
        // before the dock has engaged CP makes it fault and USB-reset in a loop (the "EP08
        // write
        // wedges the hub" mode). So stay disconnected until CP is up -- or a real EDID arrived.
        bindings::drm_connector_status_connector_status_connected
    } else {
        bindings::drm_connector_status_connector_status_disconnected
    }
}

/// CRTC `.atomic_enable`: the display is turning on (scanout begins). Captures the mode the
/// compositor selected -- any entry from the connector's full EDID-derived list -- as a
/// set-mode [`super::cp::Timing`] in [`ScanoutState::active_timing`] and pushes a live
/// mode-set CP message for it (no-op until CP engages). The geometry change is also honoured
/// by the scanout path (`encode_and_send` re-inits on `dims` change).
unsafe extern "C" fn crtc_atomic_enable(
    crtc: *mut bindings::drm_crtc,
    _state: *mut bindings::drm_atomic_commit,
) {
    // SAFETY: in `.atomic_enable` the crtc and its committed `state` are valid; `state->mode`
    // is a live drm_display_mode and `timing_from_drm_mode` only reads it.
    let cs = unsafe { (*crtc).state };
    if cs.is_null() {
        return;
    }
    let timing = unsafe { super::cp::timing_from_drm_mode(&(*cs).mode) };
    pr_info!(
        "vino: KMS CRTC enable -- display ON, mode {}x{}@{} (scanout begins)\n",
        timing.hactive, timing.vactive, timing.refresh_hz
    );
    // SAFETY: `crtc` is valid; its `dev` is our live drm_device.
    let dev = unsafe { (*crtc).dev };
    if dev.is_null() {
        return;
    }
    // SAFETY: `dev` is our registered drm_device.
    let data: &VinoDrmData = unsafe { VinoDrmDevice::from_raw(dev) };
    let Some(head) = data.head_by_crtc(crtc) else {
        return;
    };
    head.scanout.lock().active_timing = Some(timing);
    // Push a live mode-set for the chosen mode (on this head's CP stream) so the dock switches
    // to it at runtime, not just the EDID-preferred mode the bring-up setup sent. `set_mode`
    // reserves a 16-byte tag placeholder. A no-op until the cipher is engaged.
    if let Err(e) = data.send_cp(head.index, 0x48, 16, |ctr| super::cp::set_mode(ctr, &timing)) {
        pr_warn!("vino: head{} runtime mode-set send failed ({e:?})\n", head.index);
    }
    // Bring the monitor out of DPMS standby (DDC/CI VCP 0xD6 = on). Inferred wire (see
    // `cp::ddc_set_vcp`); a no-op until CP engages, and re-applies the user's brightness.
    let _ = data.set_vcp(head.index, super::cp::VCP_POWER_MODE, super::cp::POWER_ON);
    let b = head.brightness.load(core::sync::atomic::Ordering::Relaxed);
    let _ = data.set_vcp(head.index, super::cp::VCP_BRIGHTNESS, b as u16);
}

/// CRTC `.atomic_disable`: the display is turning off.
/// CRTC `.atomic_disable`: the display is turning off -- DPMS-off / blank / suspend all land
/// here in atomic KMS (the compositor clears the CRTC `active` state). The compositor stops
/// page-flipping, so no new frames are pushed; this resets the head's scanout state so a later
/// re-enable (DPMS-on) re-inits the encoder and sends a **full keyframe** rather than diffing
/// against a shadow the dock may have dropped while blanked, and re-uploads the cursor sprite.
///
/// The dock holds the last frame when video stops (it has its own scanout buffer), so video
/// alone freezes the last image rather than going black. To actually blank the panel we send a
/// DDC/CI power-off (VCP 0xD6 = off) to the monitor over the same monitor-I2C bridge the EDID
/// read uses -- the standard MCCS power control the macOS/Windows agents drive (DLM's
/// `Standby`/`Suspend`/`TempPowerOff` are the host-internal names for it). Inferred wire (see
/// [`super::cp::ddc_set_vcp`]); a no-op until CP engages.
unsafe extern "C" fn crtc_atomic_disable(
    crtc: *mut bindings::drm_crtc,
    _state: *mut bindings::drm_atomic_commit,
) {
    // SAFETY: in `.atomic_disable` the crtc and its `dev` are valid.
    let dev = unsafe { (*crtc).dev };
    if dev.is_null() {
        return;
    }
    // SAFETY: `dev` is our registered drm_device.
    let data: &VinoDrmData = unsafe { VinoDrmDevice::from_raw(dev) };
    let Some(head) = data.head_by_crtc(crtc) else {
        return;
    };
    {
        let mut st = head.scanout.lock();
        st.enc = None; // force a full re-init + keyframe on the next enable
        st.dims = (0, 0);
    }
    head.cursor_primed
        .store(false, core::sync::atomic::Ordering::SeqCst);
    // DPMS-off: blank the monitor backlight via DDC/CI (VCP 0xD6 = off) rather than leaving
    // the last frame frozen on the panel. Inferred wire (see `cp::ddc_set_vcp`); no-op until
    // CP engages.
    let _ = data.set_vcp(head.index, super::cp::VCP_POWER_MODE, super::cp::POWER_OFF);
    pr_info!("vino: KMS CRTC disable -- head{} display OFF (scanout stopped)\n", head.index);
}

/// Cursor plane `.atomic_update`: the cursor sprite and/or position changed. Sends the cursor
/// CP messages (create once + image when a sprite framebuffer is present, then a move). Gated
/// on CP engagement; a no-op on current hardware (the CP wall). See [`cursor_send`].
unsafe extern "C" fn cursor_atomic_update(
    plane: *mut bindings::drm_plane,
    _state: *mut bindings::drm_atomic_commit,
) {
    if !super::CP_ENGAGED.load(core::sync::atomic::Ordering::SeqCst) {
        return;
    }
    // SAFETY: in `.atomic_update` the plane and its committed state are valid for the commit.
    let (dev_raw, fb, w, h, cx, cy) = unsafe {
        let st = (*plane).state;
        if st.is_null() {
            return;
        }
        (
            (*plane).dev,
            (*st).fb,
            (*st).crtc_w as usize,
            (*st).crtc_h as usize,
            (*st).crtc_x,
            (*st).crtc_y,
        )
    };
    // SAFETY: `dev_raw` is our live, registered drm_device.
    let data: &VinoDrmData = unsafe { VinoDrmDevice::from_raw(dev_raw) };
    let Some(head) = data.head_by_cursor(plane) else {
        return;
    };
    if let Err(e) = cursor_send(data, head, fb, w, h, cx, cy) {
        pr_warn!("vino: head{} cursor update failed ({e:?})\n", head.index);
    }
}

/// Primary plane `.atomic_update`: a new framebuffer was flipped in -- the scanout hook.
/// Maps the framebuffer, converts XRGB8888 -> RGB565, Vino-encodes the changed
/// region against the previous frame, and bulk-writes the EP08 video frame.
///
/// The EP08 write only happens once the dock has engaged CP (see `docs/BLOCKER.md`):
/// until then the dock NAKs/stalls EP08, so a normal module load must not push frames on
/// every flip and thrash the dock. With the CP-engagement wall unsolved this never fires
/// on real hardware.
unsafe extern "C" fn primary_atomic_update(
    plane: *mut bindings::drm_plane,
    _state: *mut bindings::drm_atomic_commit,
) {
    // Don't touch EP08 until the dock has engaged CP. Pushing video (and the one-shot
    // clear_halt of EPs 8/10/11/12) at a dock with a dead CP channel makes it fault and
    // USB-reset, which re-probes the driver in a ~2.7 s loop.
    if !super::CP_ENGAGED.load(core::sync::atomic::Ordering::SeqCst) {
        return;
    }
    // SAFETY: in `.atomic_update` the plane and its committed state are valid; the plane
    // state and its framebuffer are valid for the duration of the commit.
    let (dev_raw, fb, w, h, damage, rotation) = unsafe {
        let st = (*plane).state;
        if st.is_null() {
            return;
        }
        // Plane destination geometry == the negotiated mode (the compositor sizes the primary
        // plane 1:1 with a virtual output), so this drives the dynamic scanout resolution.
        let (w, h) = ((*st).crtc_w as usize, (*st).crtc_h as usize);
        ((*plane).dev, (*st).fb, w, h, damage_bbox(st), (*st).rotation)
    };
    if fb.is_null() {
        return;
    }
    // Recover our device-private data + this plane's head from the raw drm_device.
    // SAFETY: `dev_raw` is our live, registered drm_device.
    let ddev = unsafe { VinoDrmDevice::from_raw(dev_raw) };
    let data: &VinoDrmData = ddev;
    let Some(head) = data.head_by_primary(plane) else {
        return;
    };

    use core::sync::atomic::Ordering::Relaxed;
    // Throttle: while scanout is failing (dock NAKing because CP isn't engaged), skip the
    // upcoming pageflips set by the backoff below instead of converting+encoding+sending a
    // frame the dock will just drop. The backoff is shared across heads (a coarse global rate
    // limit) -- fine while it never fires on real hardware (CP wall).
    let skip = super::SCANOUT_SKIP.load(Relaxed);
    if skip > 0 {
        super::SCANOUT_SKIP.store(skip - 1, Relaxed);
        return;
    }
    // Read this head's CRTC GAMMA_LUT (if a compositor set one) and apply it host-side in the
    // conversion below -- there is no dock-side gamma message (see `read_gamma_lut`).
    let gamma = read_gamma_lut(head);
    match scanout_one(data, head, fb, w, h, damage, rotation, gamma.as_ref()) {
        Ok(()) => {
            let n = super::SCANOUT_FAILS.swap(0, Relaxed);
            super::SCANOUT_SKIP.store(0, Relaxed);
            if n > 0 {
                pr_info!("vino: scanout recovered after {n} failed frame(s)\n");
            }
        }
        Err(e) => {
            // The dock NAKs every EP08 write (EPROTO) until CP engages -- expected and not
            // actionable. Log the first failure and then at exponentially sparser points so
            // dmesg isn't flooded, and back off the scanout rate.
            let n = super::SCANOUT_FAILS.fetch_add(1, Relaxed) + 1;
            if n == 1 || n.is_power_of_two() {
                pr_err!("vino: scanout frame failed ({e:?}) [x{n}] -- throttling\n");
            }
            // Linear backoff capped at 120 frames (~2 s @ 60 Hz) between probe attempts, so
            // recovery (CP engaging) is still detected within ~2 s while idle CPU stays low.
            super::SCANOUT_SKIP.store(core::cmp::min(n, 120), Relaxed);
        }
    }
}

/// Map an output pixel `(dx, dy)` back to its source-framebuffer pixel `(sx, sy)` under a DRM
/// plane `rotation` bitmask (`DRM_MODE_ROTATE_*` | `DRM_MODE_REFLECT_*`, the values the
/// standard `drm_plane_create_rotation_property` exposes). `sw`/`sh` are the SOURCE
/// (framebuffer) dimensions; the output dimensions are `(sw, sh)` for 0 deg/180 deg and `(sh, sw)`
/// for 90 deg/270 deg (the caller swaps source vs output accordingly). Rotation is clockwise;
/// reflection is applied in source space after rotation. Pure + total (saturating), so it is
/// unit-tested directly. Used by [`encode_and_send`] to honour the connector's rotation
/// property -- DLM rotates host-side, vino rotates in the scanout encode.
pub(super) fn rot_src(
    rotation: u32,
    dx: usize,
    dy: usize,
    sw: usize,
    sh: usize,
) -> (usize, usize) {
    let xmax = sw.saturating_sub(1);
    let ymax = sh.saturating_sub(1);
    let rot = rotation & bindings::DRM_MODE_ROTATE_MASK;
    let (mut sx, mut sy) = if rot == bindings::DRM_MODE_ROTATE_90 {
        (dy, ymax.saturating_sub(dx))
    } else if rot == bindings::DRM_MODE_ROTATE_180 {
        (xmax.saturating_sub(dx), ymax.saturating_sub(dy))
    } else if rot == bindings::DRM_MODE_ROTATE_270 {
        (xmax.saturating_sub(dy), dx)
    } else {
        (dx, dy) // ROTATE_0 / unset
    };
    if rotation & bindings::DRM_MODE_REFLECT_X != 0 {
        sx = xmax.saturating_sub(sx);
    }
    if rotation & bindings::DRM_MODE_REFLECT_Y != 0 {
        sy = ymax.saturating_sub(sy);
    }
    (sx, sy)
}

/// True if `rotation` swaps width/height (90 deg or 270 deg), so the source framebuffer is
/// portrait while the scanned-out display is landscape (or vice versa).
fn rotation_swaps_dims(rotation: u32) -> bool {
    let r = rotation & bindings::DRM_MODE_ROTATE_MASK;
    r == bindings::DRM_MODE_ROTATE_90 || r == bindings::DRM_MODE_ROTATE_270
}

/// True if `rotation` is anything other than the identity (plain 0 deg), i.e. the scanout must
/// remap every pixel and cannot take the damage-clip fast path.
fn rotation_active(rotation: u32) -> bool {
    rotation != 0 && rotation != bindings::DRM_MODE_ROTATE_0
}

/// Damage bounding box (pixels, clamped to the scanout) for this atomic update, or
/// `None` meaning "convert the whole frame". Reads the standard `FB_DAMAGE_CLIPS` blob the
/// compositor attaches to the plane state and unions its rects. The Vino encoder already
/// shadow-diffs against the previous frame, so unchanged regions emit nothing regardless;
/// the win here is skipping the XRGB8888->RGB565 conversion of those regions. Returns `None`
/// when no damage is advertised, `ignore_damage_clips` is set, or the union is degenerate
/// (all treated as a full-frame update).
///
/// SAFETY: `st` must be a valid `drm_plane_state` for the duration of the call.
unsafe fn damage_bbox(
    st: *const bindings::drm_plane_state,
) -> Option<(usize, usize, usize, usize)> {
    // SAFETY: caller guarantees `st` is a live plane state.
    let (blob, ignore) = unsafe { ((*st).fb_damage_clips, (*st).ignore_damage_clips) };
    if ignore || blob.is_null() {
        return None;
    }
    // SAFETY: `blob` is non-null and lives as long as the plane state.
    let (data, len) = unsafe { ((*blob).data as *const bindings::drm_mode_rect, (*blob).length) };
    let n = len / core::mem::size_of::<bindings::drm_mode_rect>();
    if data.is_null() || n == 0 {
        return None;
    }
    let (mut x0, mut y0, mut x1, mut y1) = (i32::MAX, i32::MAX, i32::MIN, i32::MIN);
    for i in 0..n {
        // SAFETY: `i < n`, the rect array length implied by `blob.length`.
        let r = unsafe { &*data.add(i) };
        x0 = x0.min(r.x1);
        y0 = y0.min(r.y1);
        x1 = x1.max(r.x2);
        y1 = y1.max(r.y2);
    }
    // Clamp to the plane's destination geometry and reject empty/degenerate boxes (fall back to
    // a full frame). Read crtc_w/crtc_h off the plane state so the clamp tracks the live mode,
    // not a fixed 1080p.
    // SAFETY: caller guarantees `st` is a live plane state.
    let (pw, ph) = unsafe { ((*st).crtc_w as i32, (*st).crtc_h as i32) };
    let cx0 = x0.clamp(0, pw) as usize;
    let cy0 = y0.clamp(0, ph) as usize;
    let cx1 = x1.clamp(0, pw) as usize;
    let cy1 = y1.clamp(0, ph) as usize;
    if cx1 <= cx0 || cy1 <= cy0 {
        return None;
    }
    Some((cx0, cy0, cx1, cy1))
}

/// Read the CRTC's `GAMMA_LUT` and flatten it into three 256-entry 8-bit lookup tables
/// (R, G, B), or `None` when no gamma is set (the common case -- the conversion then runs at
/// full speed). DLM gamma-corrects pixels **host-side** before encoding; the DL3 dock has no
/// gamma CP message (the `NotifyGammaCurve`/`SetGammaMode` handlers are DLM-internal,
/// vtable-dispatched, and emit no wire frame -- confirmed against the decompile and every
/// capture), so vino applies the LUT in the scanout exactly like it applies `rotation`. The
/// blob holds `n` `drm_color_lut` entries (u16 per channel); 8-bit input `i` maps to entry
/// `i*(n-1)/255` and takes that entry's high 8 bits.
fn read_gamma_lut(head: &Head) -> Option<[[u8; 256]; 3]> {
    // SAFETY: `head.crtc` was initialised in `kms_init`; its committed `state` and the
    // gamma_lut blob it references are valid for the duration of the atomic commit.
    let blob = unsafe {
        let cs = (*head.crtc.get()).state;
        if cs.is_null() {
            return None;
        }
        (*cs).gamma_lut
    };
    if blob.is_null() {
        return None;
    }
    // SAFETY: `blob` is a live drm_property_blob for the commit; `data`/`length` are valid.
    let (ptr, len) =
        unsafe { ((*blob).data as *const bindings::drm_color_lut, (*blob).length) };
    let n = len / core::mem::size_of::<bindings::drm_color_lut>();
    if ptr.is_null() || n == 0 {
        return None;
    }
    let mut t = [[0u8; 256]; 3];
    for i in 0..256usize {
        let idx = if n == 1 { 0 } else { i * (n - 1) / 255 };
        // SAFETY: `idx < n`, within the blob's `n` `drm_color_lut` entries.
        let e = unsafe { &*ptr.add(idx) };
        t[0][i] = (e.red >> 8) as u8;
        t[1][i] = (e.green >> 8) as u8;
        t[2][i] = (e.blue >> 8) as u8;
    }
    Some(t)
}

/// vmap `fb`, encode it, and push one EP08 frame. Split out so `?` can be used. `damage`
/// bounds the XRGB8888->RGB565 conversion to the changed region (see [`damage_bbox`]).
fn scanout_one(
    data: &VinoDrmData,
    head: &Head,
    fb: *mut bindings::drm_framebuffer,
    w: usize,
    h: usize,
    damage: Option<(usize, usize, usize, usize)>,
    rotation: u32,
    gamma: Option<&[[u8; 256]; 3]>,
) -> Result {
    // `w`/`h` are the plane's destination (displayed) geometry (== the negotiated mode),
    // threaded in from `primary_atomic_update`, so the scanout follows the live mode (e.g. the
    // dock's
    // native 4K) instead of a hardcoded 1080p. `drm_framebuffer` is opaque in the bindings, so
    // the geometry comes from the plane state; our XRGB8888 buffers are packed. Under a
    // 90 deg/270 deg
    // `rotation` the source framebuffer is portrait relative to the display, so its row pitch
    // tracks the *source* width -- `encode_and_send` derives that from `rotation`.
    if w == 0 || h == 0 {
        return Err(EINVAL);
    }

    // Map the framebuffer's backing pages into the kernel address space.
    // SAFETY: `iosys_map` is POD (a pointer union + bool); all-zero is a valid,
    // "not mapped" value that `drm_gem_fb_vmap` overwrites for present planes.
    let mut map: [bindings::iosys_map; 4] = unsafe { core::mem::zeroed() };
    let mut dmap: [bindings::iosys_map; 4] = unsafe { core::mem::zeroed() };
    // SAFETY: `fb` is a valid framebuffer with GEM-backed storage.
    to_result(unsafe { bindings::drm_gem_fb_vmap(fb, map.as_mut_ptr(), dmap.as_mut_ptr()) })?;

    // SAFETY: plane 0's CPU virtual address, valid until `drm_gem_fb_vunmap`.
    let vaddr = unsafe { map[0].__bindgen_anon_1.vaddr } as *const u8;
    let result = if vaddr.is_null() {
        Err(EINVAL)
    } else {
        encode_and_send(data, head, vaddr, w, h, damage, rotation, gamma)
    };

    // SAFETY: balances the vmap above with the same `map`.
    unsafe { bindings::drm_gem_fb_vunmap(fb, map.as_mut_ptr()) };
    result
}

/// Convert the mapped XRGB8888 frame to RGB565, Vino-encode it against the previous
/// frame, and bulk-write the resulting EP08 frame to the dock.
fn encode_and_send(
    data: &VinoDrmData,
    head: &Head,
    vaddr: *const u8,
    w: usize,
    h: usize,
    damage: Option<(usize, usize, usize, usize)>,
    rotation: u32,
    gamma: Option<&[[u8; 256]; 3]>,
) -> Result {
    // Convert XRGB8888 (LE bytes B,G,R,X) -> RGB565 and encode, all under the scanout lock.
    // The conversion fills a PERSISTENT `cur` buffer (allocated once with the encoder) in
    // place -- no per-frame ~4 MiB kmalloc, which is what was failing with ENOMEM and flooding
    // the log. The encoder's shadow buffer is mutable state, so the lock is needed regardless.
    let frame = {
        let mut st = head.scanout.lock();
        // On the first frame `cur` is freshly zeroed, so the whole buffer must be filled
        // regardless of the advertised damage (a partial fill would scan out black around
        // the damage box). Afterwards, unchanged regions of `cur` already hold the previous
        // frame (== the shadow the encoder diffs against), so converting only the damage box
        // is correct and skips the rest of the XRGB8888->RGB565 work.
        // Re-initialise the encoder/shadow/conversion buffers on the first frame AND whenever
        // the framebuffer geometry changes (a mode switch), so they always match `cur`'s size.
        let first = st.enc.is_none() || st.dims != (w, h);
        if first {
            st.enc = Some(super::video::Encoder::new(w, h, super::video::Mode::Rle)?);
            st.cur = VVec::from_elem(0u16, w * h, GFP_KERNEL)?;
            st.dims = (w, h);
            st.hint = 0; // previous frame's size no longer applies at the new geometry
        }
        // Source framebuffer geometry: a 90 deg/270 deg rotation makes the source portrait relative
        // to the displayed `w`x`h`, so its packed row pitch tracks the *source* width.
        let (sw, sh) = if rotation_swaps_dims(rotation) { (h, w) } else { (w, h) };
        let pitch = sw * 4;
        // Damage clips are in source coordinates and don't map cleanly through a rotation, so
        // convert the whole frame on a (re)allocation OR whenever a rotation/reflection is in
        // effect; the encoder still shadow-diffs, so unchanged pixels emit nothing regardless.
        // A gamma LUT recolours every pixel, so it also forces a full convert (still no extra
        // wire traffic -- the recoloured output is identical frame-to-frame for static
        // content,
        // so the shadow-diff emits nothing for unchanged regions).
        let full = first || rotation_active(rotation) || gamma.is_some();
        let (x0, y0, x1, y1) = if full { (0, 0, w, h) } else { damage.unwrap_or((0, 0, w, h)) };
        // Split-borrow the fields so the in-place fill and the &mut encode can coexist.
        let ScanoutState { enc, cur, seq, hint, dims: _, active_timing: _ } = &mut *st;
        for dy in y0..y1 {
            for dx in x0..x1 {
                // Output pixel (dx,dy) -> source pixel under the plane rotation/reflection.
                let (sx, sy) = rot_src(rotation, dx, dy, sw, sh);
                // SAFETY: `sy*pitch + sx*4 + 3` is within the mapped source framebuffer
                // (`sw*sh*4` bytes); `rot_src` guarantees `sx < sw`, `sy < sh`.
                let px =
                    unsafe { (vaddr.add(sy * pitch + sx * 4) as *const u32).read_unaligned() };
                let (mut r, mut g, mut b) =
                    (((px >> 16) & 0xff) as usize, ((px >> 8) & 0xff) as usize, (px & 0xff) as usize);
                // Apply the CRTC gamma LUT host-side (DLM gamma-corrects pixels before
                // encoding -- there is no dock-side gamma CP message; see `read_gamma_lut`).
                if let Some(t) = gamma {
                    r = t[0][r] as usize;
                    g = t[1][g] as usize;
                    b = t[2][b] as usize;
                }
                cur[dy * w + dx] =
                    (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) as u16;
            }
        }
        let s = *seq;
        *seq = seq.wrapping_add(1);
        let enc = enc.as_mut().ok_or(ENOMEM)?;
        // Encode straight into the outgoing frame buffer: reserve the EP08 header up
        // front, append the codec stream in place, then back-patch the header now that
        // the payload length is known. This replaces a two-allocation/extra-copy path
        // (encode -> KVec, then frame_to_ep08 -> second KVec) with a single buffer,
        // and `hint` pre-sizes it from the last frame so the encode rarely reallocates.
        const HDR: usize = super::video::EP08_HDR_LEN;
        let mut frame = KVec::with_capacity((*hint).max(HDR + 64), GFP_KERNEL)?;
        frame.extend_from_slice(&[0u8; HDR], GFP_KERNEL)?; // header placeholder
        enc.encode_into(&*cur, &mut frame)?;
        let payload_len = frame.len() - HDR;
        super::video::write_ep08_header(&mut frame[..HDR], payload_len, s);
        *hint = frame.len();
        frame
    };

    // Push the frame to this head's video endpoint (lock released).
    let dev: &super::usb::Device = data.intf.as_ref();
    // First live-scanout frame: clear-halt the four iface-0 bulk-OUT video endpoints
    // (0x08 main + 0x0a/0x0b/0x0c aux, covering every head) so the first write doesn't
    // ETIMEDOUT on a stale endpoint toggle. DLM clear-halts these at engagement (the
    // "startRender" step). Once, globally.
    if !super::EP08_SCANOUT_PRIMED.swap(true, core::sync::atomic::Ordering::SeqCst) {
        for ep in [0x08u8, 0x0a, 0x0b, 0x0c] {
            let _ = dev.clear_halt(ep);
        }
        pr_info!("vino: video endpoints primed (clear-halt 8/10/11/12)\n");
    }
    // Head 0 -> EP 0x08, head 1 -> EP 0x0a (see `HEAD_EP`).
    dev.bulk_send(head.video_ep(), &frame, super::timeout())?;
    Ok(())
}

/// Send the cursor CP messages for the current sprite + position (called from
/// [`cursor_atomic_update`]). `fb` is the cursor sprite framebuffer (`None`/null = hidden),
/// `w`x`h` its size, `(cx, cy)` the on-CRTC position. Sends `create` (once, the constant
/// sprite size), `image` (the ARGB8888 sprite -- its little-endian memory bytes are already
/// the
/// `B,G,R,A` order the dock wants, copied row-by-row to honour the framebuffer pitch), then a
/// `move`. Every send routes through [`VinoDrmData::send_cp`], so all of this is a no-op until
/// the CP cipher engages (the wall). A hidden cursor currently just re-issues a move (a
/// dedicated hide message is a future refinement).
fn cursor_send(
    data: &VinoDrmData,
    head: &Head,
    fb: *mut bindings::drm_framebuffer,
    w: usize,
    h: usize,
    cx: i32,
    cy: i32,
) -> Result {
    let hid = head.index; // cursor messages carry the head id at off22; CP routes by head too
    let (mx, my) = (
        cx.clamp(0, u16::MAX as i32) as u16,
        cy.clamp(0, u16::MAX as i32) as u16,
    );
    if fb.is_null() || w == 0 || h == 0 {
        // Hidden cursor: no sprite to upload, just track the position.
        return data.send_cp(hid, 0x1a, 0, |ctr| super::cp::cursor_move(ctr, hid, mx, my));
    }
    // Declare the sprite dimensions once per head, then upload the bitmap. We don't diff sprite
    // content yet, so the image is re-sent on every sprite-present update.
    if !head.cursor_primed.swap(true, core::sync::atomic::Ordering::SeqCst) {
        data.send_cp(hid, 0x1b, 0, |ctr| super::cp::cursor_create(ctr, w as u16, h as u16))?;
    }
    // vmap the sprite; copy `w*h*4` BGRA bytes row-by-row (the source pitch is `w*4` for our
    // packed cursor buffers, but copying per-row keeps it correct if that ever changes).
    let pitch = w * 4;
    // SAFETY: `iosys_map` is POD; all-zero is a valid "not mapped" value `drm_gem_fb_vmap`
    // fills.
    let mut map: [bindings::iosys_map; 4] = unsafe { core::mem::zeroed() };
    let mut dmap: [bindings::iosys_map; 4] = unsafe { core::mem::zeroed() };
    // SAFETY: `fb` is a valid cursor framebuffer with GEM-backed storage.
    to_result(unsafe { bindings::drm_gem_fb_vmap(fb, map.as_mut_ptr(), dmap.as_mut_ptr()) })?;
    // SAFETY: plane 0's CPU virtual address, valid until `drm_gem_fb_vunmap`.
    let vaddr = unsafe { map[0].__bindgen_anon_1.vaddr } as *const u8;
    let res = if vaddr.is_null() {
        Err(EINVAL)
    } else {
        (|| -> Result {
            let mut bgra = KVec::with_capacity(w * h * 4, GFP_KERNEL)?;
            for y in 0..h {
                // SAFETY: `[y*pitch, y*pitch + w*4)` is within the mapped `h*pitch` sprite.
                let row = unsafe { core::slice::from_raw_parts(vaddr.add(y * pitch), w * 4) };
                bgra.extend_from_slice(row, GFP_KERNEL)?;
            }
            data.send_cp(hid, 0x1c, 0, |ctr| {
                super::cp::cursor_image(ctr, w as u16, h as u16, &bgra)
            })
        })()
    };
    // SAFETY: balances the vmap above with the same `map`.
    unsafe { bindings::drm_gem_fb_vunmap(fb, map.as_mut_ptr()) };
    res?;
    data.send_cp(hid, 0x1a, 0, |ctr| super::cp::cursor_move(ctr, hid, mx, my))
}

/// Wire up the atomic KMS pipeline on `ddev` (called after `drm::Device::new` and
/// before `drm_dev_register`). Sets `mode_config`, builds the virtual connector,
/// and initialises the atomic CRTC + primary/cursor planes + virtual encoder.
pub(super) fn kms_init<C: drm::DeviceContext>(
    ddev: &drm::Device<VinoDrmDriver, C>,
) -> Result {
    let raw = ddev.as_raw();
    // Deref `drm::Device<T>` -> `T::Data` to reach the embedded C objects.
    let data: &VinoDrmData = ddev;

    // SAFETY: `raw` is a valid, not-yet-registered drm_device; the funcs/objects
    // referenced below live in device-private memory (`data`) for its lifetime.
    unsafe {
        to_result(bindings::drmm_mode_config_init(raw))?;

        let mc = &mut (*raw).mode_config;
        mc.min_width = 0;
        mc.min_height = 0;
        mc.max_width = 4096;
        mc.max_height = 4096;
        // Advertise a 64x64 hardware cursor (the dock's cursor sprite size) so userspace
        // drives the cursor plane instead of compositing the pointer into the framebuffer.
        mc.cursor_width = CURSOR_SIZE;
        mc.cursor_height = CURSOR_SIZE;
        let mcf = data.mode_cfg_funcs.get();
        (*mcf).fb_create = Some(bindings::drm_gem_fb_create);
        // `vino_atomic_check` = the standard atomic check + the combined cross-head USB
        // bandwidth budget (rejects e.g. two simultaneous 4K modes).
        (*mcf).atomic_check = Some(vino_atomic_check);
        (*mcf).atomic_commit = Some(bindings::drm_atomic_helper_commit);
        mc.funcs = mcf;

        // ---- Shared vtables (one set for every head; the callbacks recover the head from
        // the C object pointer). Plane/CRTC `atomic_check` are left NULL: a virtual sink
        // accepts any configuration, and the helpers still invoke `atomic_update`/
        // `atomic_enable` because the objects are assigned to the CRTC.

        // Connector funcs + helper. We report presence from the cached EDID (see `detect`)
        // and deliver HPD ourselves (`set_edid`/`fire_hotplug`).
        let cf = data.conn_funcs.get();
        (*cf).fill_modes = Some(bindings::drm_helper_probe_single_connector_modes);
        (*cf).detect = Some(detect);
        (*cf).destroy = Some(bindings::drm_connector_cleanup);
        (*cf).reset = Some(bindings::drm_atomic_helper_connector_reset);
        (*cf).atomic_duplicate_state =
            Some(bindings::drm_atomic_helper_connector_duplicate_state);
        (*cf).atomic_destroy_state =
            Some(bindings::drm_atomic_helper_connector_destroy_state);
        // Custom DDC/CI brightness/contrast properties (see `connector_atomic_set_property`).
        (*cf).atomic_set_property = Some(connector_atomic_set_property);
        (*cf).atomic_get_property = Some(connector_atomic_get_property);
        (*data.conn_helper.get()).get_modes = Some(get_modes);
        // Prune any single mode above the per-head pixel-clock ceiling (~4K@60).
        (*data.conn_helper.get()).mode_valid = Some(mode_valid);

        // One `drm_plane_funcs` shared by both planes; per-plane helper funcs (the primary's
        // `atomic_update` scans out, the cursor's sends cursor CP).
        let plf = data.plane_funcs.get();
        (*plf).update_plane = Some(bindings::drm_atomic_helper_update_plane);
        (*plf).disable_plane = Some(bindings::drm_atomic_helper_disable_plane);
        (*plf).destroy = Some(bindings::drm_plane_cleanup);
        (*plf).reset = Some(bindings::drm_atomic_helper_plane_reset);
        (*plf).atomic_duplicate_state =
            Some(bindings::drm_atomic_helper_plane_duplicate_state);
        (*plf).atomic_destroy_state = Some(bindings::drm_atomic_helper_plane_destroy_state);
        (*data.primary_helper.get()).atomic_update = Some(primary_atomic_update);
        (*data.cursor_helper.get()).atomic_update = Some(cursor_atomic_update);

        // CRTC funcs + helper.
        let crf = data.crtc_funcs.get();
        (*crf).set_config = Some(bindings::drm_atomic_helper_set_config);
        (*crf).page_flip = Some(bindings::drm_atomic_helper_page_flip);
        (*crf).destroy = Some(bindings::drm_crtc_cleanup);
        (*crf).reset = Some(bindings::drm_atomic_helper_crtc_reset);
        (*crf).atomic_duplicate_state =
            Some(bindings::drm_atomic_helper_crtc_duplicate_state);
        (*crf).atomic_destroy_state = Some(bindings::drm_atomic_helper_crtc_destroy_state);
        let crh = data.crtc_helper.get();
        (*crh).atomic_enable = Some(crtc_atomic_enable);
        (*crh).atomic_disable = Some(crtc_atomic_disable);

        // Encoder funcs.
        (*data.encoder_funcs.get()).destroy = Some(bindings::drm_encoder_cleanup);

        // Build each head's objects (connector + primary/cursor planes + CRTC + encoder).
        // DDC/CI brightness/contrast: one 0..=100 range property each, created on the device
        // and attached to every connector in `build_head`. Non-fatal: a NULL property just
        // means the knob is absent (kept in the AtomicPtr as NULL, ignored by the callbacks).
        let bp = bindings::drm_property_create_range(
            raw,
            0,
            c"brightness".as_ptr().cast(),
            0,
            100,
        );
        data.brightness_prop.store(bp, core::sync::atomic::Ordering::Relaxed);
        let cp = bindings::drm_property_create_range(
            raw,
            0,
            c"contrast".as_ptr().cast(),
            0,
            100,
        );
        data.contrast_prop.store(cp, core::sync::atomic::Ordering::Relaxed);

        for head in data.heads() {
            build_head(raw, data, head)?;
        }

        drm_mode_config_reset(raw);
    }
    Ok(())
}

/// Build one head's KMS objects -- connector + primary plane (scanout) + cursor plane + CRTC +
/// virtual encoder -- using the shared vtables already filled in `data`. Each is a complete
/// independent output (its own CRTC), so the compositor sees [`NHEADS`] monitors and routes
/// each to its own video EP / CP stream (see [`Head`]).
///
/// SAFETY: `raw` is a valid, not-yet-registered drm_device; the `data`/`head` C objects live
/// in device-private memory for its lifetime.
unsafe fn build_head(raw: *mut bindings::drm_device, data: &VinoDrmData, head: &Head) -> Result {
    // SAFETY: see the function contract; every object/vtable below is device-private memory.
    unsafe {
        // Connector.
        let conn = head.connector.get();
        to_result(bindings::drm_connector_init(
            raw,
            conn,
            data.conn_funcs.get(),
            bindings::DRM_MODE_CONNECTOR_VIRTUAL as i32,
        ))?;
        (*conn).helper_private = data.conn_helper.get();
        (*conn).polled = bindings::DRM_CONNECTOR_POLL_HPD as u8;

        // Primary plane (XRGB8888 scanout). `possible_crtcs` is fixed up once the CRTC exists.
        let primary = head.primary.get();
        to_result(bindings::drm_universal_plane_init(
            raw,
            primary,
            0,
            data.plane_funcs.get(),
            PRIMARY_FORMATS.as_ptr(),
            PRIMARY_FORMATS.len() as u32,
            ptr::null(),
            bindings::drm_plane_type_DRM_PLANE_TYPE_PRIMARY,
            ptr::null(),
        ))?;
        (*primary).helper_private = data.primary_helper.get();

        // Cursor plane (ARGB8888 sprite).
        let cursor = head.cursor.get();
        to_result(bindings::drm_universal_plane_init(
            raw,
            cursor,
            0,
            data.plane_funcs.get(),
            CURSOR_FORMATS.as_ptr(),
            CURSOR_FORMATS.len() as u32,
            ptr::null(),
            bindings::drm_plane_type_DRM_PLANE_TYPE_CURSOR,
            ptr::null(),
        ))?;
        (*cursor).helper_private = data.cursor_helper.get();

        // CRTC with both planes, plus a GAMMA_LUT (applied host-side in `read_gamma_lut`).
        let crtc = head.crtc.get();
        to_result(bindings::drm_crtc_init_with_planes(
            raw,
            crtc,
            primary,
            cursor,
            data.crtc_funcs.get(),
            ptr::null(),
        ))?;
        (*crtc).helper_private = data.crtc_helper.get();
        bindings::drm_crtc_enable_color_mgmt(crtc, 0, false, GAMMA_SIZE);

        // The CRTC now has an index: bind both planes and the encoder to it.
        let crtc_mask = 1u32 << (*crtc).index;
        (*primary).possible_crtcs = crtc_mask;
        (*cursor).possible_crtcs = crtc_mask;

        // Virtual encoder bound to this head's connector.
        let encoder = head.encoder.get();
        to_result(bindings::drm_encoder_init(
            raw,
            encoder,
            data.encoder_funcs.get(),
            bindings::DRM_MODE_ENCODER_VIRTUAL as i32,
            ptr::null(),
        ))?;
        (*encoder).possible_crtcs = crtc_mask;
        to_result(bindings::drm_connector_attach_encoder(conn, encoder))?;

        // Rotation property on the primary plane (DLM rotates host-side; vino remaps in the
        // scanout encode -- see `rot_src`). Canonical helper; non-fatal on failure.
        let supported = bindings::DRM_MODE_ROTATE_0
            | bindings::DRM_MODE_ROTATE_90
            | bindings::DRM_MODE_ROTATE_180
            | bindings::DRM_MODE_ROTATE_270
            | bindings::DRM_MODE_REFLECT_X
            | bindings::DRM_MODE_REFLECT_Y;
        let rc = bindings::drm_plane_create_rotation_property(
            primary,
            bindings::DRM_MODE_ROTATE_0,
            supported,
        );
        if rc != 0 {
            pr_warn!("vino: head{} rotation property unavailable ({rc})\n", head.index);
        }

        // Attach the shared DDC/CI brightness/contrast properties to this connector, each at
        // its default of 100 (= no attenuation). The callbacks store the value per head and
        // fire the DDC/CI write (see `connector_atomic_set_property`).
        let bp = data.brightness_prop.load(core::sync::atomic::Ordering::Relaxed);
        if !bp.is_null() {
            bindings::drm_object_attach_property(&mut (*conn).base, bp, 100);
        }
        let cp = data.contrast_prop.load(core::sync::atomic::Ordering::Relaxed);
        if !cp.is_null() {
            bindings::drm_object_attach_property(&mut (*conn).base, cp, 100);
        }
    }
    Ok(())
}

/// Connector `.atomic_set_property`: handle the custom DDC/CI brightness/contrast properties
/// (the standard properties are handled by the DRM core, which never calls us for them). On a
/// value change we store it on the head and immediately push a DDC/CI Set-VCP write to the
/// monitor -- DDC/CI is a side-band action on the physical panel, not part of the atomic
/// scanout state, so it is applied here rather than threaded through connector state. Returns
/// `-EINVAL` for any other property so the core reports it as unknown.
unsafe extern "C" fn connector_atomic_set_property(
    connector: *mut bindings::drm_connector,
    _state: *mut bindings::drm_connector_state,
    property: *mut bindings::drm_property,
    val: u64,
) -> i32 {
    // SAFETY: `connector` is a valid connector embedded in our DRM device-private.
    let dev = unsafe { (*connector).dev };
    // SAFETY: `dev` is our live, registered drm_device.
    let data: &VinoDrmData = unsafe { VinoDrmDevice::from_raw(dev) };
    let Some(head) = data.head_by_connector(connector) else {
        return EINVAL.to_errno();
    };
    let bp = data.brightness_prop.load(core::sync::atomic::Ordering::Relaxed);
    let cp = data.contrast_prop.load(core::sync::atomic::Ordering::Relaxed);
    let v = val.min(100) as u32;
    let (slot, vcp) = if property == bp && !bp.is_null() {
        (&head.brightness, super::cp::VCP_BRIGHTNESS)
    } else if property == cp && !cp.is_null() {
        (&head.contrast, super::cp::VCP_CONTRAST)
    } else {
        return EINVAL.to_errno();
    };
    if slot.swap(v, core::sync::atomic::Ordering::Relaxed) != v {
        let _ = data.set_vcp(head.index, vcp, v as u16);
    }
    0
}

/// Connector `.atomic_get_property`: read back the stored DDC/CI brightness/contrast (the
/// values round-trip through the head atomics set by [`connector_atomic_set_property`]).
unsafe extern "C" fn connector_atomic_get_property(
    connector: *mut bindings::drm_connector,
    _state: *const bindings::drm_connector_state,
    property: *mut bindings::drm_property,
    val: *mut u64,
) -> i32 {
    // SAFETY: `connector` is a valid connector embedded in our DRM device-private.
    let dev = unsafe { (*connector).dev };
    // SAFETY: `dev` is our live, registered drm_device.
    let data: &VinoDrmData = unsafe { VinoDrmDevice::from_raw(dev) };
    let Some(head) = data.head_by_connector(connector) else {
        return EINVAL.to_errno();
    };
    let bp = data.brightness_prop.load(core::sync::atomic::Ordering::Relaxed);
    let cp = data.contrast_prop.load(core::sync::atomic::Ordering::Relaxed);
    let slot = if property == bp && !bp.is_null() {
        &head.brightness
    } else if property == cp && !cp.is_null() {
        &head.contrast
    } else {
        return EINVAL.to_errno();
    };
    // SAFETY: the DRM core passes a valid `*mut u64` output pointer.
    unsafe { *val = slot.load(core::sync::atomic::Ordering::Relaxed) as u64 };
    0
}

/// Thin wrapper so the `unsafe` block above reads cleanly.
unsafe fn drm_mode_config_reset(raw: *mut bindings::drm_device) {
    // SAFETY: `raw` is a valid drm_device with mode_config initialised.
    unsafe { bindings::drm_mode_config_reset(raw) };
}
