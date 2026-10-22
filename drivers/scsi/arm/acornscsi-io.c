// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Acorn SCSI card I/O
 *
 *  The Acorn SCSI expansion card exposes each 16-bit SCSI data
 *  halfword on the low half of a 32-bit card access:
 *
 *   - on reads, one 16-bit value is returned per 32-bit card load;
 *     consecutive card reads are packed into 32-bit memory words
 *
 *   - on writes, a 16-bit memory halfword is expanded into a 32-bit
 *     card word.  The original assembly duplicates the halfword into
 *     both halves of the card word for the bulk of the transfer and
 *     writes only the low 16 bits of the trailing partial word,
 *     since the hardware samples only the low half.
 */

#include <linux/align.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "acornscsi-io.h"

/*
 * Transfer a block of data from the Acorn SCSI card into memory.
 *
 * The card address is rounded down to a 32-bit boundary.  Each
 * 32-bit read from the card yields one valid 16-bit halfword in
 * the low half; pairs of card reads are packed into single 32-bit
 * memory words.
 */
void __acornscsi_in(void __iomem *addr, char *buffer, int length)
{
	const u32 __iomem *card = PTR_ALIGN_DOWN((const u32 __iomem *)addr, 4);
	u8 *buf = (u8 *)buffer;

	while (length >= 4) {
		u32 w0 = readl(card + 0);
		u32 w1 = readl(card + 1);

		put_unaligned_le32((w0 & 0xffff) | (w1 << 16), buf);

		card += 2;
		buf  += 4;
		length -= 4;
	}

	if (length >= 2) {
		u32 w0 = readl(card);

		put_unaligned_le16(w0 & 0xffff, buf);

		card++;
		buf += 2;
		length -= 2;
	}

	if (length >= 1) {
		u32 w0 = readl(card);

		*buf = w0 & 0xff;
	}
}

/*
 * Transfer a block of data from memory to the Acorn SCSI card.
 *
 * Each 16-bit value read from memory is expanded into a 32-bit card
 * word, with the halfword duplicated in both halves (matching the
 * original assembly).  For the trailing partial halfword the card
 * only samples the low 16 bits, so two byte stores are sufficient.
 */
void __acornscsi_out(void __iomem *addr, char *buffer, int length)
{
	u32 __iomem *card = PTR_ALIGN_DOWN((u32 __iomem *)addr, 4);
	const u8 *buf = (const u8 *)buffer;

	while (length >= 4) {
		u32 v  = get_unaligned_le32(buf);
		u32 lo = v & 0xffff;
		u32 hi = v >> 16;

		writel(lo | (lo << 16), card + 0);
		writel(hi | (hi << 16), card + 1);

		card += 2;
		buf  += 4;
		length -= 4;
	}

	if (length >= 2) {
		void __iomem *cp = (void __iomem *)card;

		writeb(buf[0], cp);
		writeb(buf[1], cp + 1);

		card++;
		buf += 2;
		length -= 2;
	}

	if (length >= 1)
		writeb(*buf, card);
}
