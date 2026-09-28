// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#![cfg_attr(not(CONFIG_KUNIT), expect(dead_code))]

use kernel::prelude::*;

use super::{
    EncodedStream,
    Index,
    KeyId,
    Op,
    Opcode, //
};

/// An encoder for an NVKV stream.
pub(crate) struct Encoder {
    stream: EncodedStream,
}

impl Encoder {
    /// Creates an empty encoder.
    pub(crate) fn new() -> Self {
        Self {
            stream: EncodedStream::new(),
        }
    }

    /// Returns the encoded data.
    #[must_use = "encoded stream must be consumed"]
    pub(crate) fn finish(self) -> EncodedStream {
        self.stream
    }

    #[inline]
    fn encode_op(&mut self, op: Op) -> Result {
        self.stream.push_u64(op.into_raw())
    }

    /// Encodes a 32-bit value as an IMM32 pair, with the value in the op word.
    #[inline]
    pub(crate) fn encode_u32(&mut self, key: KeyId, index: Index, value: u32) -> Result {
        // TODO: Consider automatically merging sequential keys.
        self.encode_op(
            Op::zeroed()
                .with_key(key)
                .with_index(index)
                .with_opcode(Opcode::Imm32)
                .with_value(value),
        )
    }

    /// Encodes a 64-bit value as a single-element SEQ64 pair.
    #[inline]
    pub(crate) fn encode_u64(&mut self, key: KeyId, index: Index, value: u64) -> Result {
        // TODO: Consider automatically merging sequential keys.
        const KEY_COUNT: u32 = 1;
        self.encode_op(
            Op::zeroed()
                .with_key(key)
                .with_index(index)
                .with_opcode(Opcode::Seq64)
                .with_value(KEY_COUNT),
        )?;
        self.stream.push_u64(value)
    }

    /// Encodes a byte array as an ARRAY8 pair, zero-padded to a multiple of 8 bytes.
    #[inline]
    pub(crate) fn encode_array8(&mut self, key: KeyId, index: Index, array: &[u8]) -> Result {
        let value_count = u32::try_from(array.len()).map_err(|_| EMSGSIZE)?;
        self.encode_op(
            Op::zeroed()
                .with_key(key)
                .with_index(index)
                .with_opcode(Opcode::Array8)
                .with_value(value_count),
        )?;
        self.stream.extend_with_padding(array)
    }

    /// Encodes a 32-bit array as an ARRAY32 pair, zero-padded to a multiple of 8 bytes.
    #[inline]
    pub(crate) fn encode_array32(&mut self, key: KeyId, index: Index, array: &[u32]) -> Result {
        let value_count = u32::try_from(array.len()).map_err(|_| EMSGSIZE)?;
        self.encode_op(
            Op::zeroed()
                .with_key(key)
                .with_index(index)
                .with_opcode(Opcode::Array32)
                .with_value(value_count),
        )?;
        self.stream.extend_with_padding(array)
    }

    /// Encodes a 64-bit array as an ARRAY64 pair.
    #[inline]
    pub(crate) fn encode_array64(&mut self, key: KeyId, index: Index, array: &[u64]) -> Result {
        let value_count = u32::try_from(array.len()).map_err(|_| EMSGSIZE)?;
        self.encode_op(
            Op::zeroed()
                .with_key(key)
                .with_index(index)
                .with_opcode(Opcode::Array64)
                .with_value(value_count),
        )?;
        self.stream.extend_with_padding(array)
    }
}

#[kunit_tests(nova_core_nvkv_encode)]
mod tests {
    use super::*;

    // Tests that each kind of value is encoded to NVKV wire format properly.
    #[test]
    fn encode_all_value_kinds() -> Result {
        // All keys, indexes, and values are distinct but arbitrary values to make it easier for the
        // test to catch bugs in the encoded output.
        const U32_KEY: KeyId = 0x1001;
        const U64_KEY: KeyId = 0x1002;
        const ARRAY8_KEY: KeyId = 0x1003;
        const ARRAY32_KEY: KeyId = 0x1004;
        const ARRAY64_KEY: KeyId = 0x1005;

        const U32_VALUE: u32 = 0x1111_2222;
        const U64_VALUE: u64 = 0x3333_4444_5555_6666;
        const ARRAY8_VALUE: &[u8] = &[0xaa, 0xbb, 0xcc];
        const ARRAY32_VALUE: &[u32] = &[0xbbbb_cccc, 0xdddd_eeee];
        const ARRAY64_VALUE: &[u64] = &[0x0123_4567_89ab_cdef, 0xfedc_ba98_7654_3210];

        let mut encoder = Encoder::new();
        encoder.encode_u32(U32_KEY, Index::new::<0>(), U32_VALUE)?;
        encoder.encode_u64(U64_KEY, Index::new::<1>(), U64_VALUE)?;
        encoder.encode_array8(ARRAY8_KEY, Index::new::<2>(), ARRAY8_VALUE)?;
        encoder.encode_array32(ARRAY32_KEY, Index::new::<3>(), ARRAY32_VALUE)?;
        encoder.encode_array64(ARRAY64_KEY, Index::new::<4>(), ARRAY64_VALUE)?;

        let encoded = encoder.finish();
        assert_eq!(encoded.len(), 10);

        // IMM32 has its value in the op word.
        assert_eq!(
            encoded[0],
            Op::zeroed()
                .with_key(U32_KEY)
                .with_index(Index::new::<0>())
                .with_opcode(Opcode::Imm32)
                .with_value(U32_VALUE)
                .into_raw()
        );

        // The SEQ64 op word followed by the value.
        assert_eq!(
            encoded[1],
            Op::zeroed()
                .with_key(U64_KEY)
                .with_index(Index::new::<1>())
                .with_opcode(Opcode::Seq64)
                .with_value(1u32)
                .into_raw()
        );
        assert_eq!(encoded[2], U64_VALUE);

        // The ARRAY8 op word has the byte count. The bytes follow, padded out to a whole word.
        assert_eq!(
            encoded[3],
            Op::zeroed()
                .with_key(ARRAY8_KEY)
                .with_index(Index::new::<2>())
                .with_opcode(Opcode::Array8)
                .with_value(3u32)
                .into_raw()
        );
        assert_eq!(
            encoded[4],
            u64::from_le_bytes([0xaa, 0xbb, 0xcc, 0, 0, 0, 0, 0])
        );

        // The ARRAY32 op word has the element count. The two elements follow in little endian.
        assert_eq!(
            encoded[5],
            Op::zeroed()
                .with_key(ARRAY32_KEY)
                .with_index(Index::new::<3>())
                .with_opcode(Opcode::Array32)
                .with_value(2u32)
                .into_raw()
        );
        assert_eq!(
            encoded[6],
            u64::from(ARRAY32_VALUE[1]) << 32 | u64::from(ARRAY32_VALUE[0])
        );

        // The ARRAY64 op word has the element count with the two elements after.
        assert_eq!(
            encoded[7],
            Op::zeroed()
                .with_key(ARRAY64_KEY)
                .with_index(Index::new::<4>())
                .with_opcode(Opcode::Array64)
                .with_value(2u32)
                .into_raw()
        );
        assert_eq!(encoded[8], ARRAY64_VALUE[0]);
        assert_eq!(encoded[9], ARRAY64_VALUE[1]);

        Ok(())
    }
}
