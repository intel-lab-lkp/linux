// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *   Ploytec PCM encoding/decoding functions for the S24_3LE format (3 bytes per sample)
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#include <linux/unaligned.h>
#include <linux/string.h>
#include "ploytec_codec.h"

/**
 * DOC: Why the wire format looks like this
 *
 * The "bit-plane" layout implemented below is not an arbitrary scramble; it is
 * a direct image of how the hardware moves audio. On the Jockey 3 the USB
 * device controller (NXP ISP1583) runs in split-bus mode, and the serial
 * data lines of the capture ADCs are wired straight onto the low bits of its
 * 16-bit DMA bus - one converter per bus bit. The controller samples the whole
 * bus once per bit clock and streams the result to the host, so every byte on
 * the wire is a snapshot of all the channel data lines at one instant.
 *
 * Decoding a frame is therefore a transpose:
 *
 * - The byte index is one bit-clock tick, most significant bit first: wire
 *   byte 0 carries bit 23 of the current sample, wire byte 23 carries bit 0.
 * - The bit position within each byte is one physical converter line, that is
 *   one channel (bit 0, bit 1, bit 2, ... for successive converters).
 * - The frame is two 24-byte halves, the left and right sub-frames of the
 *   underlying I2S frame. On capture an 8-byte gap sits between and after the
 *   halves: it is the unused tail of each 32-bit I2S slot (64-byte frame).
 *   Playback is generated on the byte-wide microprocessor bus instead, so its
 *   slots are packed to 24 bits with no gap (48-byte frame).
 *
 * Because the geometry follows from the I2S timing and the DMA bus width, it
 * is fixed regardless of how many converters are populated. That is why the
 * codec always works in 8-channel-wide slices and simply ignores the bit
 * positions that carry no converter (the Jockey 3 populates three capture
 * lines and two playback lines).
 *
 * This is a model inferred from the board schematic and the observed stream
 * rather than from vendor documentation, but it accounts for every bit of the
 * format the functions below implement.
 */

#if IS_ENABLED(CONFIG_SND_USB_JOCKEY3_REFERENCE_CODEC)

/*
 * The reference implementation of the Ploytec codec performs a bit
 * gather/scatter operation with minimal optimization. It mainly serves as a
 * readable definition of the wire format, and as the oracle the optimized
 * variants below are validated against.
 *
 * Selected by CONFIG_SND_USB_JOCKEY3_REFERENCE_CODEC.
 */

/**
 * ploytec_encode_s24_3le() - Encode 4-channel S24_3LE to 48-byte Ploytec frame
 * @dest: 48-byte destination buffer
 * @src: 12-byte source buffer (4 channels * 3 bytes)
 *
 * Ploytec Bit-Plane Interleaving (Playback):
 * The firmware uses a non-standard "bit-plane" format where bits from different
 * channels are interleaved into the same byte.
 * - Each 48-byte frame carries one PCM frame: a single sample for each of
 *   the 4 channels, spread one bit per wire byte.
 * - Bytes 0-23: ALSA Channels 1 & 3
 * - Bytes 24-47: ALSA Channels 2 & 4
 * - Within each 24-byte block, bits are grouped by significance:
 *   - [0-7]: Most significant bits
 *   - [8-15]: Middle bits
 *   - [16-23]: Least significant bits
 * - bit 0 of each byte corresponds to the first channel in the pair.
 * - bit 1 of each byte corresponds to the second channel in the pair.
 */
static inline void ploytec_encode_s24_3le(u8 *dest, const u8 *src)
{
	int i;

	for (i = 0; i < 8; i++) {
		/* First 24 bytes: odd channels (ALSA Ch 1 & 3) */
		dest[i]      = (((src[2] >> (7 - i)) & 1) << 0) | (((src[8] >> (7 - i)) & 1) << 1);
		dest[8 + i]  = (((src[1] >> (7 - i)) & 1) << 0) | (((src[7] >> (7 - i)) & 1) << 1);
		dest[16 + i] = (((src[0] >> (7 - i)) & 1) << 0) | (((src[6] >> (7 - i)) & 1) << 1);

		/* Second 24 bytes: even channels (ALSA Ch 2 & 4) */
		dest[24 + i] = (((src[5] >> (7 - i)) & 1) << 0) | (((src[11] >> (7 - i)) & 1) << 1);
		dest[32 + i] = (((src[4] >> (7 - i)) & 1) << 0) | (((src[10] >> (7 - i)) & 1) << 1);
		dest[40 + i] = (((src[3] >> (7 - i)) & 1) << 0) | (((src[9]  >> (7 - i)) & 1) << 1);
	}
}

/**
 * ploytec_decode_s24_3le() - Decode 64-byte Ploytec frame to 6-channel S24_3LE
 * @dest: 18-byte destination buffer (6 channels * 3 bytes)
 * @src: 64-byte source buffer
 *
 * Ploytec Bit-Plane Interleaving (Capture):
 * Similar to encoding, the capture path interleaves 3 pairs of channels
 * into bit-planes (bit 0, 1, and 2 of each byte).
 * - Bytes 0x00-0x17: Pair 1 (bits 0,1,2)
 * - Bytes 0x20-0x37: Pair 2 (bits 0,1,2)
 */
static inline void ploytec_decode_s24_3le(u8 *dest, const u8 *src)
{
	int i;

	memset(dest, 0, 18);
	for (i = 0; i < 8; i++) {
		/* Channel 1: odd channel 1 (bit 0 of bytes 0x00-0x17) */
		dest[0x00] |= ((src[0x10 + i] & 0x01) << (7 - i));	// Ch1 LSB byte
		dest[0x01] |= ((src[0x08 + i] & 0x01) << (7 - i));
		dest[0x02] |= ((src[0x00 + i] & 0x01) << (7 - i));	// Ch1 MSB byte

		/* Channel 2: even channel 2 (bit 0 of bytes 0x20-0x37) */
		dest[0x03] |= ((src[0x30 + i] & 0x01) << (7 - i));
		dest[0x04] |= ((src[0x28 + i] & 0x01) << (7 - i));
		dest[0x05] |= ((src[0x20 + i] & 0x01) << (7 - i));

		/* Channel 3: odd channel 3 (bit 1 of bytes 0x00-0x17) */
		dest[0x06] |= (((src[0x10 + i] & 0x02) >> 1) << (7 - i));
		dest[0x07] |= (((src[0x08 + i] & 0x02) >> 1) << (7 - i));
		dest[0x08] |= (((src[0x00 + i] & 0x02) >> 1) << (7 - i));

		/* Channel 4: even channel 4 (bit 1 of bytes 0x20-0x37) */
		dest[0x09] |= (((src[0x30 + i] & 0x02) >> 1) << (7 - i));
		dest[0x0A] |= (((src[0x28 + i] & 0x02) >> 1) << (7 - i));
		dest[0x0B] |= (((src[0x20 + i] & 0x02) >> 1) << (7 - i));

		/* Channel 5: odd channel 5 (bit 2 of bytes 0x00-0x17) */
		dest[0x0C] |= (((src[0x10 + i] & 0x04) >> 2) << (7 - i));
		dest[0x0D] |= (((src[0x08 + i] & 0x04) >> 2) << (7 - i));
		dest[0x0E] |= (((src[0x00 + i] & 0x04) >> 2) << (7 - i));

		/* Channel 6: even channel 6 (bit 2 of bytes 0x20-0x37) */
		dest[0x0F] |= (((src[0x30 + i] & 0x04) >> 2) << (7 - i));
		dest[0x10] |= (((src[0x28 + i] & 0x04) >> 2) << (7 - i));
		dest[0x11] |= (((src[0x20 + i] & 0x04) >> 2) << (7 - i));
	}
}
#elif defined(CONFIG_64BIT)
/**
 * DOC: Ploytec bit-plane interleaving optimization
 *
 * CORE PERFORMANCE PROBLEM:
 * The Ploytec firmware uses a non-standard "bit-plane" format where bits
 * from different channels are interleaved into the same byte. The naive
 * implementation processes data bit-by-bit using heavily nested loops,
 * causing massive CPU overhead due to serial bit-shifting operations,
 * branch mispredictions, and an inability to utilize hardware pipelines.
 *
 * THE SOLUTION: SWAR (SIMD Within A Register) & INT-MULTIPLIERS
 * Instead of bit-by-bit iteration, we treat entire 32-bit or 64-bit blocks
 * of memory as parallel vector lanes inside standard CPU registers. This
 * completely eliminates loops and branches, reducing operations to a small,
 * deterministic chain of native processor instructions.
 *
 * -------------------------------------------------------------------------
 * 1. ENCODING MATH: BIT-SPREAD LOOKUP TABLES (LUT)
 * -------------------------------------------------------------------------
 * To encode, we must take a single 8-bit channel byte (e.g., b7 b6 b5 ... b0)
 * and "spread" its bits across an 8-byte destination block so that each bit
 * lands on bit 0 of 8 separate bytes.
 *
 * 64-Bit Implementation:
 * We use a precomputed 256-entry 64-bit LUT (ploytec_bit_spread). For any
 * byte value, the table returns a u64 where the bits are perfectly spaced out:
 * Input Byte:  0b11000000
 * u64 Return:  0x0000000000000101 (Bit 7 and 6 mapped to byte 0 and 1)
 *
 * By loading the spread values of two channels simultaneously, we shift the
 * second channel by 1 (<< 1) to align its bits to bit position 1, and bitwise
 * OR them together. This encodes two full channels into an 8-byte frame using
 * just one lookup and a single 64-bit store.
 *
 * 32-Bit Implementation:
 * A 32-bit register cannot hold 8 spread bytes at once. Thus, the 32-bit
 * variant splits the input byte into two 4-bit nibbles using two distinct
 * 32-bit LUTs (high nibble and low nibble). Each lookup fills 4 bytes of
 * destination memory natively using 32-bit registers, maintaining low
 * register pressure and avoiding 64-bit arithmetic emulation penalties on
 * 32-bit architectures (like ARM32/armhf).
 *
 * -------------------------------------------------------------------------
 * 2. DECODING MATH: PARALLEL BIT GATHER VIA MAGIC MULTIPLIERS
 * -------------------------------------------------------------------------
 * To decode, we must perform the inverse: extract a specific bit position
 * from 8 consecutive bytes in memory and pack them back into a single byte.
 *
 * 64-Bit Implementation (`pack_bit_plane64`):
 * 1. Load 8 bytes into a u64 register.
 * 2. Shift right by the desired bit index and mask with 0x0101010101010101ULL.
 * This leaves the target bit isolated as bit 0 of every single byte.
 * 3. Multiply the masked u64 by the magic constant: 0x8040201008040201ULL.
 *
 * This multiplication acts as a parallel shift-and-add engine:
 * Byte 7 is multiplied by 0x01 (shifted << 0)
 * Byte 6 is multiplied by 0x02 (shifted << 1)
 * ...
 * Byte 0 is multiplied by 0x80 (shifted << 7)
 *
 * The mathematical properties of this multiplication collapse all 8 isolated
 * bits perfectly into the most significant byte (the highest 8 bits) of the
 * u64 register. A final logical shift right (>> 56) extracts the completed
 * audio byte in a single operation.
 *
 * 32-Bit Implementation (`pack_bit_plane32`):
 * To avoid emulating 64-bit multiplication on a 32-bit CPU, we read two
 * separate 4-byte halves into native u32 registers. We apply the 32-bit
 * magic multiplier (0x08040201U) to both halves independently. This collapses
 * each half into a 4-bit nibble at the top of each register. We then combine
 * the two resulting nibbles `(high << 4) | low` to form the final byte.
 *
 * -------------------------------------------------------------------------
 * 3. ENDIANNESS & ALIGNMENT MITIGATION
 * -------------------------------------------------------------------------
 * All bitwise shifting (`<<`, `>>`) and multiplier math inside CPU registers
 * is inherently endian-agnostic. However, reading and writing these multi-byte
 * integers (u32/u64) directly to physical RAM introduces two system hazards:
 * - Byte Inversion on Big Endian architectures (e.g., MIPS, PowerPC).
 * - Kernel Alignment Faults (SIGBUS) on alignment-strict processors.
 *
 * To ensure safe, architecture-independent execution, all memory interfaces
 * utilize the kernel's `get_unaligned_le32/64` and `put_unaligned_le32/64`
 * macros (or user-space memcpy/bswap equivalents).
 *
 * On little-endian architectures with unaligned support (x86_64, arm64, armhf),
 * the compiler optimizes these macros completely out, compiling down to standard,
 * zero-overhead native instruction loads and stores. On big-endian or strict-
 * alignment architectures, the macros safely handle arbitrary memory addresses
 * and emit hardware byte-swaps (`rev`) automatically.
 *
 * -------------------------------------------------------------------------
 * 4. EMPIRICAL PERFORMANCE TESTING
 * -------------------------------------------------------------------------
 * Compared to the reference implementation, testing of these optimized
 * versions showed a significant improvement, justifying the added complexity
 * from these optimizations. Table shows the relative speed-up and the time
 * for encoding (4ch) or decoding (6ch) a sample frame on the validation
 * hardware, measured through the batch API at the batch sizes the driver
 * actually uses (10 frames per encode call, 8 per decode):
 *
 * Architecture	Bit	Encode	Decode		Encode Time	Decode Time
 *   i386	32	 6.2x	 5.4x		  8.3 ns	 20.7 ns
 *   x86_64	64	11.0x	 7.8x		  3.7 ns	 11.9 ns
 *   armhf	32	 3.3x	 4.8x		358.2 ns	762.7 ns
 *   arm64	64	 6.9x	 6.1x		 13.6 ns	 35.5 ns
 *
 * i386 and x86_64 on a Core i5-6500, arm64 on a Raspberry Pi 4B, armhf on a
 * Raspberry Pi 1B+ (ARMv6). Speed-up is against this same build's portable
 * reference codec on the same machine, so it is comparable across rows; the
 * absolute times are not.
 *
 * The 64-bit variants gain more than the 32-bit ones because the transpose
 * works in register-width chunks: a 64-bit word carries twice the bit-plane
 * of a 32-bit one, so the same frame needs half the operations.
 */

static u64 ploytec_bit_spread_64[256];

/**
 * init_ploytec_bit_spread_lut64() - Build the 64-bit bit-spread lookup table
 *
 * Precomputes ploytec_bit_spread_64[], mapping each possible input byte to
 * the 8-byte bit-plane pattern used by ploytec_encode_s24_3le_lut64(). See
 * the optimization note above for details of the algorithm.
 */
static void init_ploytec_bit_spread_lut64(void)
{
	for (int b = 0; b < 256; b++) {
		u64 spread = 0;

		// Extract bit (7 - i) and place it into bit 0 of byte i
		for (int i = 0; i < 8; i++)
			spread |= (u64)((b >> (7 - i)) & 1) << (i * 8);

		ploytec_bit_spread_64[b] = spread;
	}
}

/**
 * ploytec_encode_s24_3le_lut64() - Encode 4-channel S24_3LE using the 64-bit LUT bit-spread method
 * @dest: 48-byte destination buffer
 * @src: 12-byte source buffer (4 channels * 3 bytes)
 *
 * Optimized, CONFIG_64BIT equivalent of ploytec_encode_s24_3le(); produces
 * identical output. See the bit-plane layout on ploytec_encode_s24_3le()
 * and the optimization note above for the LUT technique used here.
 */
static inline void ploytec_encode_s24_3le_lut64(u8 *dest, const u8 *src)
{
	/*
	 * put_unaligned_le64 safely writes a 64-bit value in Little Endian
	 * order, even if the target CPU is Big Endian or alignment-strict.
	 */

	// First 24 bytes: odd channels (ALSA Ch 1 & 3)
	put_unaligned_le64(ploytec_bit_spread_64[src[2]] |
			   (ploytec_bit_spread_64[src[8]] << 1), dest + 0);
	put_unaligned_le64(ploytec_bit_spread_64[src[1]] |
			   (ploytec_bit_spread_64[src[7]] << 1), dest + 8);
	put_unaligned_le64(ploytec_bit_spread_64[src[0]] |
			   (ploytec_bit_spread_64[src[6]] << 1), dest + 16);

	// Second 24 bytes: even channels (ALSA Ch 2 & 4)
	put_unaligned_le64(ploytec_bit_spread_64[src[5]] |
			   (ploytec_bit_spread_64[src[11]] << 1), dest + 24);
	put_unaligned_le64(ploytec_bit_spread_64[src[4]] |
			   (ploytec_bit_spread_64[src[10]] << 1), dest + 32);
	put_unaligned_le64(ploytec_bit_spread_64[src[3]] |
			   (ploytec_bit_spread_64[src[9]] << 1), dest + 40);
}

/**
 * pack_bit_plane64() - Gather bit @bit_index of 8 consecutive bytes into one byte
 * @val: 8 source bytes loaded as a little-endian u64
 * @bit_index: bit position (0-7) to extract from each source byte
 *
 * Return: the gathered byte, using the magic-multiplier technique described
 * in the optimization note above.
 */
static inline u8 pack_bit_plane64(u64 val, int bit_index)
{
	// Shift target bit to bit 0 of each byte, then mask it
	u64 masked = (val >> bit_index) & 0x0101010101010101ULL;

	/*
	 * Multiplier acts as a parallel shift-and-add, collecting the bits
	 * into the highest byte of the u64.
	 */
	return (u8)((masked * 0x8040201008040201ULL) >> 56);
}

/**
 * ploytec_decode_s24_3le_pack64() - Decode a Ploytec frame using the 64-bit magic-multiplier method
 * @dest: 18-byte destination buffer (6 channels * 3 bytes)
 * @src: 64-byte source buffer
 *
 * Optimized, CONFIG_64BIT equivalent of ploytec_decode_s24_3le(); produces
 * identical output. See the bit-plane layout on ploytec_decode_s24_3le()
 * and the optimization note above for the gather technique used here.
 */
static inline void ploytec_decode_s24_3le_pack64(u8 *dest, const u8 *src)
{
	u64 blocks_0_17_low  = get_unaligned_le64(src + 0x00);
	u64 blocks_0_17_mid  = get_unaligned_le64(src + 0x08);
	u64 blocks_0_17_high = get_unaligned_le64(src + 0x10);

	u64 blocks_20_37_low  = get_unaligned_le64(src + 0x20);
	u64 blocks_20_37_mid  = get_unaligned_le64(src + 0x28);
	u64 blocks_20_37_high = get_unaligned_le64(src + 0x30);

	// Channel 1 (Bit 0 of bytes 0x00-0x17)
	dest[0x00] = pack_bit_plane64(blocks_0_17_high, 0);
	dest[0x01] = pack_bit_plane64(blocks_0_17_mid,  0);
	dest[0x02] = pack_bit_plane64(blocks_0_17_low,  0);

	// Channel 2 (Bit 0 of bytes 0x20-0x37)
	dest[0x03] = pack_bit_plane64(blocks_20_37_high, 0);
	dest[0x04] = pack_bit_plane64(blocks_20_37_mid,  0);
	dest[0x05] = pack_bit_plane64(blocks_20_37_low,  0);

	// Channel 3 (Bit 1 of bytes 0x00-0x17)
	dest[0x06] = pack_bit_plane64(blocks_0_17_high, 1);
	dest[0x07] = pack_bit_plane64(blocks_0_17_mid,  1);
	dest[0x08] = pack_bit_plane64(blocks_0_17_low,  1);

	// Channel 4 (Bit 1 of bytes 0x20-0x37)
	dest[0x09] = pack_bit_plane64(blocks_20_37_high, 1);
	dest[0x0A] = pack_bit_plane64(blocks_20_37_mid,  1);
	dest[0x0B] = pack_bit_plane64(blocks_20_37_low,  1);

	// Channel 5 (Bit 2 of bytes 0x00-0x17)
	dest[0x0C] = pack_bit_plane64(blocks_0_17_high, 2);
	dest[0x0D] = pack_bit_plane64(blocks_0_17_mid,  2);
	dest[0x0E] = pack_bit_plane64(blocks_0_17_low,  2);

	// Channel 6 (Bit 2 of bytes 0x20-0x37)
	dest[0x0F] = pack_bit_plane64(blocks_20_37_high, 2);
	dest[0x10] = pack_bit_plane64(blocks_20_37_mid,  2);
	dest[0x11] = pack_bit_plane64(blocks_20_37_low,  2);
}

#else /* optimized codec, 32-bit build */
static u32 ploytec_bit_spread_32_high[256];
static u32 ploytec_bit_spread_32_low[256];

/**
 * init_ploytec_bit_spread_lut32() - Build the 32-bit bit-spread lookup tables
 *
 * Precomputes ploytec_bit_spread_32_high[] and ploytec_bit_spread_32_low[],
 * the 32-bit nibble-split equivalent of init_ploytec_bit_spread_lut64() for
 * architectures without efficient native 64-bit arithmetic. See the
 * optimization note above for details.
 */
static void init_ploytec_bit_spread_lut32(void)
{
	for (int b = 0; b < 256; b++) {
		u32 high_spread = 0;
		u32 low_spread  = 0;

		// Process high nibble (bits 7 down to 4) -> maps to bytes 0 to 3
		for (int i = 0; i < 4; i++)
			high_spread |= (u32)((b >> (7 - i)) & 1) << (i * 8);

		// Process low nibble (bits 3 down to 0) -> maps to bytes 0 to 3
		for (int i = 0; i < 4; i++)
			low_spread  |= (u32)((b >> (3 - i)) & 1) << (i * 8);

		ploytec_bit_spread_32_high[b] = high_spread;
		ploytec_bit_spread_32_low[b]  = low_spread;
	}
}

/**
 * ploytec_encode_s24_3le_lut32() - Encode 4-channel S24_3LE using the 32-bit LUT bit-spread method
 * @dest: 48-byte destination buffer
 * @src: 12-byte source buffer (4 channels * 3 bytes)
 *
 * 32-bit equivalent of ploytec_encode_s24_3le_lut64() for architectures
 * without efficient native 64-bit arithmetic; produces identical output.
 */
static inline void ploytec_encode_s24_3le_lut32(u8 *dest, const u8 *src)
{
	/*
	 * put_unaligned_le32 safely writes a 32-bit value in Little Endian
	 * order, even if the target CPU is Big Endian or alignment-strict.
	 */

	// First 24 bytes: Odd channels (ALSA Ch 1 [src 0,1,2] & Ch 3 [src 6,7,8])
	// Block 1: Most Significant Bytes
	put_unaligned_le32(ploytec_bit_spread_32_high[src[2]] |
			   (ploytec_bit_spread_32_high[src[8]] << 1), dest + 0);
	put_unaligned_le32(ploytec_bit_spread_32_low[src[2]]  |
			   (ploytec_bit_spread_32_low[src[8]]  << 1), dest + 4);

	// Block 2: Middle Bytes
	put_unaligned_le32(ploytec_bit_spread_32_high[src[1]] |
			   (ploytec_bit_spread_32_high[src[7]] << 1), dest + 8);
	put_unaligned_le32(ploytec_bit_spread_32_low[src[1]]  |
			   (ploytec_bit_spread_32_low[src[7]]  << 1), dest + 12);

	// Block 3: Least Significant Bytes
	put_unaligned_le32(ploytec_bit_spread_32_high[src[0]] |
			   (ploytec_bit_spread_32_high[src[6]] << 1), dest + 16);
	put_unaligned_le32(ploytec_bit_spread_32_low[src[0]]  |
			   (ploytec_bit_spread_32_low[src[6]]  << 1), dest + 20);

	// Second 24 bytes: Even channels (ALSA Ch 2 [src 3,4,5] & Ch 4 [src 9,10,11])
	// Block 1: Most Significant Bytes
	put_unaligned_le32(ploytec_bit_spread_32_high[src[5]] |
			   (ploytec_bit_spread_32_high[src[11]] << 1), dest + 24);
	put_unaligned_le32(ploytec_bit_spread_32_low[src[5]]  |
			   (ploytec_bit_spread_32_low[src[11]]  << 1), dest + 28);

	// Block 2: Middle Bytes
	put_unaligned_le32(ploytec_bit_spread_32_high[src[4]] |
			   (ploytec_bit_spread_32_high[src[10]] << 1), dest + 32);
	put_unaligned_le32(ploytec_bit_spread_32_low[src[4]]  |
			   (ploytec_bit_spread_32_low[src[10]]  << 1), dest + 36);

	// Block 3: Least Significant Bytes
	put_unaligned_le32(ploytec_bit_spread_32_high[src[3]] |
			   (ploytec_bit_spread_32_high[src[9]] << 1), dest + 40);
	put_unaligned_le32(ploytec_bit_spread_32_low[src[3]]  |
			   (ploytec_bit_spread_32_low[src[9]]  << 1), dest + 44);
}

/**
 * pack_bit_plane32() - Gather bit @bit_index of 8 consecutive bytes into one byte
 * @high_4_bytes: first 4 source bytes of the block, holding the high nibble (bits 7-4)
 * @low_4_bytes: last 4 source bytes of the block, holding the low nibble (bits 3-0)
 * @bit_index: bit position (0-7) to extract from each source byte
 *
 * Return: the gathered byte, using the magic-multiplier technique described
 * in the optimization note above.
 */
static inline u8 pack_bit_plane32(u32 high_4_bytes, u32 low_4_bytes, int bit_index)
{
	// Isolate the targeted bit index across all 4 bytes simultaneously
	u32 high_masked = (high_4_bytes >> bit_index) & 0x01010101U;
	u32 low_masked  = (low_4_bytes  >> bit_index) & 0x01010101U;

	// Use 32-bit multiplier to gather bits 0,1,2,3 into the highest byte
	u8 high_bits = (u8)((high_masked * 0x08040201U) >> 24);
	u8 low_bits  = (u8)((low_masked  * 0x08040201U) >> 24);

	// Combine the upper 4 bits and lower 4 bits into the single output audio byte
	return (high_bits << 4) | low_bits;
}

/**
 * ploytec_decode_s24_3le_pack32() - Decode a Ploytec frame using the 32-bit magic-multiplier method
 * @dest: 18-byte destination buffer (6 channels * 3 bytes)
 * @src: 64-byte source buffer
 *
 * 32-bit equivalent of ploytec_decode_s24_3le_pack64() for architectures
 * without efficient native 64-bit arithmetic; produces identical output.
 */
static inline void ploytec_decode_s24_3le_pack32(u8 *dest, const u8 *src)
{
	// Pre-load all required 4-byte chunks into native 32-bit registers
	u32 src_00_high = get_unaligned_le32(src + 0x00);
	u32 src_04_low  = get_unaligned_le32(src + 0x04);
	u32 src_08_high = get_unaligned_le32(src + 0x08);
	u32 src_0C_low  = get_unaligned_le32(src + 0x0C);
	u32 src_10_high = get_unaligned_le32(src + 0x10);
	u32 src_14_low  = get_unaligned_le32(src + 0x14);

	u32 src_20_high = get_unaligned_le32(src + 0x20);
	u32 src_24_low  = get_unaligned_le32(src + 0x24);
	u32 src_28_high = get_unaligned_le32(src + 0x28);
	u32 src_2C_low  = get_unaligned_le32(src + 0x2C);
	u32 src_30_high = get_unaligned_le32(src + 0x30);
	u32 src_34_low  = get_unaligned_le32(src + 0x34);

	// --- Channel 1: Bit 0 of bytes 0x00-0x17 ---
	dest[0x00] = pack_bit_plane32(src_10_high, src_14_low, 0);
	dest[0x01] = pack_bit_plane32(src_08_high, src_0C_low, 0);
	dest[0x02] = pack_bit_plane32(src_00_high, src_04_low, 0);

	// --- Channel 2: Bit 0 of bytes 0x20-0x37 ---
	dest[0x03] = pack_bit_plane32(src_30_high, src_34_low, 0);
	dest[0x04] = pack_bit_plane32(src_28_high, src_2C_low, 0);
	dest[0x05] = pack_bit_plane32(src_20_high, src_24_low, 0);

	// --- Channel 3: Bit 1 of bytes 0x00-0x17 ---
	dest[0x06] = pack_bit_plane32(src_10_high, src_14_low, 1);
	dest[0x07] = pack_bit_plane32(src_08_high, src_0C_low, 1);
	dest[0x08] = pack_bit_plane32(src_00_high, src_04_low, 1);

	// --- Channel 4: Bit 1 of bytes 0x20-0x37 ---
	dest[0x09] = pack_bit_plane32(src_30_high, src_34_low, 1);
	dest[0x0A] = pack_bit_plane32(src_28_high, src_2C_low, 1);
	dest[0x0B] = pack_bit_plane32(src_20_high, src_24_low, 1);

	// --- Channel 5: Bit 2 of bytes 0x00-0x17 ---
	dest[0x0C] = pack_bit_plane32(src_10_high, src_14_low, 2);
	dest[0x0D] = pack_bit_plane32(src_08_high, src_0C_low, 2);
	dest[0x0E] = pack_bit_plane32(src_00_high, src_04_low, 2);

	// --- Channel 6: Bit 2 of bytes 0x20-0x37 ---
	dest[0x0F] = pack_bit_plane32(src_30_high, src_34_low, 2);
	dest[0x10] = pack_bit_plane32(src_28_high, src_2C_low, 2);
	dest[0x11] = pack_bit_plane32(src_20_high, src_24_low, 2);
}
#endif

/*
 * Bind the build's chosen implementation to a single pair of names, so that
 * everything below is free of conditional compilation.
 */
#if IS_ENABLED(CONFIG_SND_USB_JOCKEY3_REFERENCE_CODEC)

static const enum ploytec_codec_variant ploytec_codec_selected =
	PLOYTEC_CODEC_PORTABLE;

static void ploytec_codec_init_tables(void) { }

static inline void ploytec_encode_frame(u8 *dest, const u8 *src)
{
	ploytec_encode_s24_3le(dest, src);
}

static inline void ploytec_decode_frame(u8 *dest, const u8 *src)
{
	ploytec_decode_s24_3le(dest, src);
}

#elif defined(CONFIG_64BIT)

static const enum ploytec_codec_variant ploytec_codec_selected =
	PLOYTEC_CODEC_OPTIMIZED_64BIT;

static void ploytec_codec_init_tables(void)
{
	init_ploytec_bit_spread_lut64();
}

static inline void ploytec_encode_frame(u8 *dest, const u8 *src)
{
	ploytec_encode_s24_3le_lut64(dest, src);
}

static inline void ploytec_decode_frame(u8 *dest, const u8 *src)
{
	ploytec_decode_s24_3le_pack64(dest, src);
}

#else

static const enum ploytec_codec_variant ploytec_codec_selected =
	PLOYTEC_CODEC_OPTIMIZED_32BIT;

static void ploytec_codec_init_tables(void)
{
	init_ploytec_bit_spread_lut32();
}

static inline void ploytec_encode_frame(u8 *dest, const u8 *src)
{
	ploytec_encode_s24_3le_lut32(dest, src);
}

static inline void ploytec_decode_frame(u8 *dest, const u8 *src)
{
	ploytec_decode_s24_3le_pack32(dest, src);
}

#endif

/**
 * ploytec_initialize_codec() - Initialize the codec's pre-computed lookup tables
 *
 * Builds the bit-spread lookup table(s) used by the optimized encoder/decoder.
 * Must be called once (e.g. at driver probe) before any call to
 * ploytec_encode_batch() or ploytec_decode_batch().
 *
 * Return: which codec variant this build selected, for the caller to log.
 */
enum ploytec_codec_variant ploytec_initialize_codec(void)
{
	ploytec_codec_init_tables();
	return ploytec_codec_selected;
}

/**
 * ploytec_encode_batch() - Encode a number of 4-channel S24_3LE to 48-byte Ploytec frames
 * @dest: n_frames * 48-byte destination buffer (Ploytec playback frame)
 * @src: n_frames * 12-byte source buffer (4 channels * 3 bytes)
 * @n_frames: number of frames to process in this batch
 */
void ploytec_encode_batch(u8 *dest, const u8 *src, const int n_frames)
{
	for (int f = 0; f < n_frames; f++) {
		ploytec_encode_frame(dest, src);
		dest += PLOYTEC_PLAYBACK_FRAME_SIZE;
		src += PLOYTEC_PLAYBACK_PCM_FRAME_SIZE;
	}
}

/**
 * ploytec_decode_batch() - Decode a number of 64-byte Ploytec frames to 6-channel S24_3LE
 * @dest: n_frames * 18-byte destination buffer (6 channels * 3 bytes)
 * @src: n_frames * 64-byte source buffer
 * @n_frames: number of frames to process in this batch
 */
void ploytec_decode_batch(u8 *dest, const u8 *src, const int n_frames)
{
	for (int f = 0; f < n_frames; f++) {
		ploytec_decode_frame(dest, src);
		dest += PLOYTEC_CAPTURE_PCM_FRAME_SIZE;
		src += PLOYTEC_CAPTURE_FRAME_SIZE;
	}
}
