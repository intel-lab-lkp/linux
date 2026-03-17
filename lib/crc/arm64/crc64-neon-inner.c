// SPDX-License-Identifier: GPL-2.0-only
/*
 * Accelerated CRC64 (NVMe) using ARM NEON C intrinsics
 */

#include <linux/types.h>
#include <linux/crc64.h>
#ifdef CONFIG_ARM64
#include <asm/neon-intrinsics.h>
#else
#include <arm_neon.h>
#endif

#define GET_P64_0(v) ((poly64_t)vgetq_lane_u64(vreinterpretq_u64_p64(v), 0))
#define GET_P64_1(v) ((poly64_t)vgetq_lane_u64(vreinterpretq_u64_p64(v), 1))

static const u64 fold_consts_val[2] = {0xeadc41fd2ba3d420ULL, 0x21e9761e252621acULL};
static const u64 bconsts_val[2] = {0x27ecfa329aef9f77ULL, 0x34d926535897936aULL};

u64 crc64_nvme_arm64_c(u64 crc, const u8 *p, size_t len)
{
	if (len == 0)
		return crc;

	uint64x2_t v0_u64 = {crc, 0};
	poly64x2_t v0 = vreinterpretq_p64_u64(v0_u64);

	poly64x2_t fold_consts = vreinterpretq_p64_u64(vld1q_u64(fold_consts_val));

	if (len >= 16) {
		poly64x2_t v1 = vreinterpretq_p64_u8(vld1q_u8(p));

		v0 = vreinterpretq_p64_u8(veorq_u8(vreinterpretq_u8_p64(v0),
						   vreinterpretq_u8_p64(v1)));
		p += 16;
		len -= 16;

		while (len >= 16) {
			v1 = vreinterpretq_p64_u8(vld1q_u8(p));

			poly128_t v2 = vmull_high_p64(fold_consts, v0);
			poly128_t v0_128 = vmull_p64(GET_P64_0(fold_consts), GET_P64_0(v0));

			uint8x16_t x0 = veorq_u8(vreinterpretq_u8_p128(v0_128),
						 vreinterpretq_u8_p128(v2));

			x0 = veorq_u8(x0, vreinterpretq_u8_p64(v1));
			v0 = vreinterpretq_p64_u8(x0);

			p += 16;
			len -= 16;
		}

		poly64x2_t v7 = vreinterpretq_p64_u64((uint64x2_t){0, 0});

		poly128_t v1_128 = vmull_p64(GET_P64_1(fold_consts), GET_P64_0(v0));

		uint8x16_t ext_v0 = vextq_u8(vreinterpretq_u8_p64(v0), vreinterpretq_u8_p64(v7), 8);
		uint8x16_t x0 = veorq_u8(ext_v0, vreinterpretq_u8_p128(v1_128));

		v0 = vreinterpretq_p64_u8(x0);

		poly64x2_t bconsts = vreinterpretq_p64_u64(vld1q_u64(bconsts_val));

		v1_128 = vmull_p64(GET_P64_0(bconsts), GET_P64_0(v0));

		poly64x2_t v1_64 = vreinterpretq_p64_u8(vreinterpretq_u8_p128(v1_128));
		poly128_t v3_128 = vmull_p64(GET_P64_1(bconsts), GET_P64_0(v1_64));

		x0 = veorq_u8(vreinterpretq_u8_p64(v0), vreinterpretq_u8_p128(v3_128));

		uint8x16_t ext_v2 = vextq_u8(vreinterpretq_u8_p64(v7),
					     vreinterpretq_u8_p128(v1_128), 8);

		x0 = veorq_u8(x0, ext_v2);

		v0 = vreinterpretq_p64_u8(x0);
		crc = vgetq_lane_u64(vreinterpretq_u64_p64(v0), 1);
	}

	return crc;
}
