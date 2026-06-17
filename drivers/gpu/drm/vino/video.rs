// SPDX-License-Identifier: GPL-2.0

//! RawRl (Raw/RLX) **mode-2 video encoder** -- clean-room from the AArch64 DLM
//! decompile (sec 8.4 + `docs/decompile/arm64-blockencoder`/`-frame-markers`).
//! Emits packed-RGB565 frames the dock decodes WITHOUT the impractical Vino
//! Walsh-Hadamard entropy codec (sec 7.11). This is a **verbatim port** of the
//! `vino-codec::rawrl` oracle, whose encode/decode round-trip is unit-tested
//! (keyframe, differential, >256-pixel multi-block and >255 RLE run-splits all
//! reconstruct byte-exact); keep the two in lockstep. No real mode-2 capture
//! exists to diff against (sec 7.4), so that round-trip is the correctness anchor.
//! NOT yet wired into `probe()`: sending a frame the dock rejects USB-resets the
//! dock, so EP08 streaming is a supervised bring-up step.
#![allow(dead_code)] // Encoder/Mode variants validated by KUnit; live scanout uses the RLE path

use super::*;

pub(super) const MAGIC_RAW16: u16 = 0x68af;
pub(super) const MAGIC_RLE16: u16 = 0x69af;
/// Frame-init `0x40af` (`FUN_003330fc`: u32 `0xaf0440af` + u16 `0x0840`).
pub(super) const FRAME_INIT: [u8; 6] = [0xaf, 0x40, 0x04, 0xaf, 0x40, 0x08];
/// Bare `0xa0af` sync (`FUN_00332a38`).
pub(super) const SYNC: [u8; 2] = [0xaf, 0xa0];
/// Frame-end section->code table `DAT_005b7860`, indexed by `mode - 1`.
pub(super) const SECTION_CODE: [u8; 7] = [0x01, 0x00, 0x03, 0x00, 0x05, 0x07, 0x07];
pub(super) const MAX_BLOCK_PIXELS: usize = 256;

/// Per-run strategy: mode 0 raw-only, 1 RLE-only, 2 adaptive (sec 8.4).
#[derive(Clone, Copy)]
pub(super) enum Mode {
    Raw = 0,
    Rle = 1,
    Adaptive = 2,
}

/// Pack 8-bit RGB into RGB565 (the XRGB framebuffer reduced for the
/// `0x68af`/`0x69af` path).
pub(super) fn rgb565(r: u8, g: u8, b: u8) -> u16 {
    ((r as u16 >> 3) << 11) | ((g as u16 >> 2) << 5) | (b as u16 >> 3)
}

/// 6-byte block header: magic LE, 24-bit coord BE, count u8 (256 -> 0).
fn block_header(out: &mut KVec<u8>, magic: u16, coord: u32, count: usize) -> Result {
    out.extend_from_slice(&magic.to_le_bytes(), GFP_KERNEL)?;
    out.push(((coord >> 16) & 0xff) as u8, GFP_KERNEL)?;
    out.push(((coord >> 8) & 0xff) as u8, GFP_KERNEL)?;
    out.push((coord & 0xff) as u8, GFP_KERNEL)?;
    out.push((count & 0xff) as u8, GFP_KERNEL)?;
    Ok(())
}

fn encode_raw_into(out: &mut KVec<u8>, coord: u32, pix: &[u16]) -> Result {
    block_header(out, MAGIC_RAW16, coord, pix.len())?;
    for &p in pix {
        out.extend_from_slice(&p.to_be_bytes(), GFP_KERNEL)?;
    }
    Ok(())
}

fn encode_rle_into(out: &mut KVec<u8>, coord: u32, pix: &[u16]) -> Result {
    block_header(out, MAGIC_RLE16, coord, pix.len())?;
    let mut i = 0;
    while i < pix.len() {
        let v = pix[i];
        let mut run = 1;
        while i + run < pix.len() && pix[i + run] == v && run < 255 {
            run += 1;
        }
        out.push(run as u8, GFP_KERNEL)?;
        out.extend_from_slice(&v.to_be_bytes(), GFP_KERNEL)?;
        i += run;
    }
    Ok(())
}

fn run_count(pix: &[u16]) -> usize {
    let mut c = 0;
    let mut i = 0;
    while i < pix.len() {
        let v = pix[i];
        let mut j = i + 1;
        while j < pix.len() && pix[j] == v {
            j += 1;
        }
        c += 1;
        i = j;
    }
    c
}

fn encode_run_into(out: &mut KVec<u8>, mode: Mode, coord: u32, pix: &[u16]) -> Result {
    match mode {
        Mode::Raw => encode_raw_into(out, coord, pix),
        Mode::Rle => encode_rle_into(out, coord, pix),
        Mode::Adaptive => {
            let l = pix.len();
            let c = run_count(pix);
            if 2 * l < 3 * c + 1 {
                encode_raw_into(out, coord, pix)
            } else {
                encode_rle_into(out, coord, pix)
            }
        }
    }
}

/// Mode-2 frame encoder holding the shadow (previous-frame) buffer.
pub(super) struct Encoder {
    width: usize,
    height: usize,
    mode: Mode,
    // vmalloc-backed: a `width*height` u16 buffer is ~4 MiB at 1080p, far above the
    // contiguous-kmalloc order limit (the page allocator WARNs and fails on it).
    shadow: VVec<u16>,
}

impl Encoder {
    pub(super) fn new(width: usize, height: usize, mode: Mode) -> Result<Self> {
        let shadow = VVec::from_elem(0u16, width * height, GFP_KERNEL)?;
        Ok(Self { width, height, mode, shadow })
    }

    /// Encode `cur` (RGB565) into a mode-2 marker stream; updates the shadow.
    /// Change-detection is per row; changed runs chunk into <=256-px blocks.
    pub(super) fn encode(&mut self, cur: &[u16]) -> Result<KVec<u8>> {
        let mut s = KVec::new();
        self.encode_into(cur, &mut s)?;
        Ok(s)
    }

    /// Like [`encode`](Self::encode) but appends the marker stream to a caller-owned
    /// `out` instead of allocating a fresh `KVec`. The hot scanout path
    /// ([`encode_and_send`](super::drm_sink::encode_and_send)) uses this to encode
    /// straight into a buffer that already reserves the EP08 transport header, so a
    /// frame costs one allocation with no separate framing copy.
    pub(super) fn encode_into(&mut self, cur: &[u16], s: &mut KVec<u8>) -> Result {
        s.extend_from_slice(&FRAME_INIT, GFP_KERNEL)?;
        for y in 0..self.height {
            let row = y * self.width;
            let mut x = 0;
            while x < self.width {
                while x < self.width && cur[row + x] == self.shadow[row + x] {
                    x += 1;
                }
                if x >= self.width {
                    break;
                }
                let run_start = x;
                while x < self.width && cur[row + x] != self.shadow[row + x] {
                    x += 1;
                }
                let run_end = x;
                let mut p = run_start;
                while p < run_end {
                    let n = (run_end - p).min(MAX_BLOCK_PIXELS);
                    let coord = (((row + p) * 2) & 0xff_ffff) as u32;
                    encode_run_into(s, self.mode, coord, &cur[row + p..row + p + n])?;
                    p += n;
                }
                for k in run_start..run_end {
                    self.shadow[row + k] = cur[row + k];
                }
            }
        }
        let code = SECTION_CODE[(self.mode as usize).saturating_sub(1).min(6)];
        s.extend_from_slice(&SYNC, GFP_KERNEL)?;
        s.extend_from_slice(&[0xaf, 0x20, 0x1f, code], GFP_KERNEL)?;
        s.extend_from_slice(&[0xaf, 0x20, 0xff, 0x00], GFP_KERNEL)?;
        s.extend_from_slice(&SYNC, GFP_KERNEL)?;
        Ok(())
    }
}

/// Vino (`0x2801`) Walsh-Hadamard codec -- the bandwidth-constrained / 4K path (the RLE path
/// above is what the dock currently runs; this is the lossy transform codec DLM uses when raw/
/// RLE won't fit the USB budget). See `docs/WHT-CODEC.md` + `docs/VIDEO.md`.
///
/// **Scope.** The colour transform, the quantizer, and the 2-level Walsh-Hadamard transform
/// are reverse-engineered and **validated offline** (`white -> Y_DC=16320 -> quantized 1020`;
/// achromatic -> `Cb=Cr=0`; uniform block -> DC=mean, AC=0). The token *bit format* (5-bit
/// short
/// 0..=30 / 17-bit long, MSB-first) and the **token-value mapping** are confirmed against DLM's
/// own frida token trace (`captures/02-solid-white/tokens.jsonl`): the **token value is the
/// quantized coefficient, directly** -- pure-white strips emit `L,1020` exactly where
/// `quantize(16320, DC) = 1020`, so the rumoured "entropy codebook" is just this direct value
/// encoding, not a lookup table (the 1641-byte expression-tree coder is the bit-packer). **What
/// is still NOT generated here:** the per-strip *framing* -- a uniform strip wraps the DC in a
/// constant prefix/suffix of framing tokens (`L,2048 L,3072 ... L,3 ... S,19 S,16 ...`) plus
/// zero-run
/// AC coding, and the dock's exact sequency ordering -- so a complete `Mode::Wht` would replay
/// the recovered uniform-strip template with the DC substituted (the `docs/WHT-CODEC.md`
/// structural model, ~90% desktop coverage). Until that framing is generalized + wired, the
/// scanout path keeps using RLE.
// Not yet wired into the scanout path (the per-strip framing template is recovered for white
// but not yet generalized to arbitrary uniform colour / non-uniform content) -- RLE stays the
// active codec; this module is validated by its KUnit tests + the frida-trace value mapping.
#[allow(dead_code)] // Walsh-Hadamard codec: KUnit-validated, not yet on the live scanout path
pub(super) mod wht {
    use super::*;

    /// 4x8 transform block geometry (`docs/VIDEO.md`): 4 rows x 8 columns = 32 samples.
    pub(super) const ROWS: usize = 4;
    pub(super) const COLS: usize = 8;
    pub(super) const BLOCK: usize = ROWS * COLS;

    /// Vino colour transform (`docs/VIDEO.md`, exact integer form, no rounding):
    /// `Y = 16R + 32G + 16B`, `Cb = 64(R-G)`, `Cr = 64(B-G)`. Achromatic (R=G=B) ->
    /// Cb=Cr=0.
    pub(super) fn colour(r: u8, g: u8, b: u8) -> (i32, i32, i32) {
        let (r, g, b) = (r as i32, g as i32, b as i32);
        (16 * r + 32 * g + 16 * b, 64 * (r - g), 64 * (b - g))
    }

    /// Per-coefficient `(bias, step)` quantization table (`docs/VIDEO.md` `FUN_0077b140`),
    /// keyed by coefficient position `0..64`.
    fn bias_step(i: usize) -> (i32, i32) {
        match i {
            0..=2 => (8, 16),
            3 => (16, 32),
            4..=11 => (2, 4),
            12..=15 => (4, 8),
            16..=47 => (1, 2),
            _ => (2, 4), // 48..=63
        }
    }

    /// Quantize coefficient `coeff` at position `i`: `(coeff + bias) * (65536/step) >> 16`,
    /// the fixed-point form of `(coeff + bias) / step` (`docs/VIDEO.md`). Clamped to the
    /// 12-bit signed long-token range (the DC is wider than the +/-127 AC clip -- the
    /// documented
    /// `white -> 1020` vector is a 12-bit long token, not a +/-127 value).
    pub(super) fn quantize(coeff: i32, i: usize) -> i32 {
        let (bias, step) = bias_step(i);
        let scale = 65536 / step;
        (((coeff + bias) * scale) >> 16).clamp(-2048, 2047)
    }

    /// In-place 1-D Walsh-Hadamard (natural/Hadamard order) on a power-of-two slice,
    /// unnormalized (pairwise sums/differences); the 2-D transform normalizes afterwards.
    fn hadamard_1d(v: &mut [i32]) {
        let n = v.len();
        let mut h = 1;
        while h < n {
            let mut i = 0;
            while i < n {
                for j in i..i + h {
                    let (a, b) = (v[j], v[j + h]);
                    v[j] = a + b;
                    v[j + h] = a - b;
                }
                i += 2 * h;
            }
            h *= 2;
        }
    }

    /// 2-level separable Walsh-Hadamard transform of a 4x8 `block` (row-major), normalized so
    /// the DC coefficient equals the block **mean** -- i.e. a uniform block yields `DC = the
    /// per-pixel value` and all AC = 0 (`docs/VIDEO.md`). Returns 32 coefficients row-major.
    /// (Natural Hadamard order; the dock's sequency reorder is not bit-matched -- see the
    /// module note.)
    pub(super) fn transform(block: &[i32; BLOCK]) -> [i32; BLOCK] {
        let mut m = *block;
        for r in 0..ROWS {
            hadamard_1d(&mut m[r * COLS..r * COLS + COLS]);
        }
        let mut col = [0i32; ROWS];
        for c in 0..COLS {
            for r in 0..ROWS {
                col[r] = m[r * COLS + c];
            }
            hadamard_1d(&mut col);
            for r in 0..ROWS {
                m[r * COLS + c] = col[r];
            }
        }
        // Normalize by the block size (/32 = >>5) so DC = mean (uniform block -> DC = value).
        for x in m.iter_mut() {
            *x >>= 5;
        }
        m
    }

    /// MSB-first bit packer for the Vino token stream (`docs/VIDEO.md`): a 16-bit zero pad at
    /// the start, then codewords packed most-significant-bit first across byte boundaries.
    pub(super) struct TokenWriter {
        out: KVec<u8>,
        acc: u32,
        nbits: u32,
    }

    impl TokenWriter {
        pub(super) fn new() -> Result<Self> {
            let mut w = Self { out: KVec::new(), acc: 0, nbits: 0 };
            w.put(0, 16)?; // 16-bit zero pad at stream start
            Ok(w)
        }

        /// Append the low `n` bits of `val` (n <= 24), MSB-first.
        fn put(&mut self, val: u32, n: u32) -> Result {
            self.acc = (self.acc << n) | (val & ((1u32 << n) - 1));
            self.nbits += n;
            while self.nbits >= 8 {
                self.nbits -= 8;
                self.out.push(((self.acc >> self.nbits) & 0xff) as u8, GFP_KERNEL)?;
            }
            Ok(())
        }

        /// Write one token *value* in the Vino short/long encoding: a 5-bit short token for
        /// `0..=30`, else the 17-bit long token `0b11111` escape + 12-bit value. (The mapping
        /// from a quantized coefficient to this `value` is the un-RE'd entropy codebook -- see
        /// the module note -- so callers can only pack values they already know.)
        pub(super) fn token(&mut self, value: u16) -> Result {
            if value <= 30 {
                self.put(value as u32, 5)
            } else {
                self.put(0b11111, 5)?;
                self.put((value & 0x0fff) as u32, 12)
            }
        }

        /// Flush any partial byte (zero-padded) and return the packed stream.
        pub(super) fn finish(mut self) -> Result<KVec<u8>> {
            if self.nbits > 0 {
                let pad = 8 - self.nbits;
                self.put(0, pad)?;
            }
            Ok(self.out)
        }
    }
}

/// Length of the EP08 transport header ([`write_ep08_header`]).
pub(super) const EP08_HDR_LEN: usize = 16;

/// Write the 16-byte EP08 transport header into `hdr` for a `payload_len`-byte codec
/// stream: `type=4 sub=0x30 sub_len_dw=0` sec 3 framing (matches the live capture).
/// `size = payload_len + 12`. Used by the in-place scanout path. `hdr` must be at
/// least 16 bytes.
pub(super) fn write_ep08_header(hdr: &mut [u8], payload_len: usize, seq: u32) {
    hdr[0] = 0;
    hdr[1] = 0;
    hdr[2..4].copy_from_slice(&((payload_len + 12) as u16).to_le_bytes());
    hdr[4..8].copy_from_slice(&4u32.to_le_bytes());
    hdr[8..10].copy_from_slice(&0x30u16.to_le_bytes());
    hdr[10..12].copy_from_slice(&0u16.to_le_bytes());
    hdr[12..16].copy_from_slice(&seq.to_le_bytes());
}
