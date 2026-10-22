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

	while (length >= 16) {
		u32 w0 = readl(card + 0), w1 = readl(card + 1);
		u32 w2 = readl(card + 2), w3 = readl(card + 3);
		u32 w4 = readl(card + 4), w5 = readl(card + 5);
		u32 w6 = readl(card + 6), w7 = readl(card + 7);

		card += 8;

		((u32 *)buf)[0] = (w0 & 0xffff) | (w1 << 16);
		((u32 *)buf)[1] = (w2 & 0xffff) | (w3 << 16);
		((u32 *)buf)[2] = (w4 & 0xffff) | (w5 << 16);
		((u32 *)buf)[3] = (w6 & 0xffff) | (w7 << 16);

		buf += 16;
		length -= 16;
	}

	while (length >= 8) {
		u32 w0 = readl(card + 0), w1 = readl(card + 1);
		u32 w2 = readl(card + 2), w3 = readl(card + 3);

		card += 4;

		((u32 *)buf)[0] = (w0 & 0xffff) | (w1 << 16);
		((u32 *)buf)[1] = (w2 & 0xffff) | (w3 << 16);

		buf += 8;
		length -= 8;
	}

	while (length >= 4) {
		u32 w0 = readl(card + 0), w1 = readl(card + 1);

		card += 2;

		((u32 *)buf)[0] = (w0 & 0xffff) | (w1 << 16);

		buf += 4;
		length -= 4;
	}

	while (length >= 2) {
		u32 w0 = readl(card);

		card++;

		buf[0] = w0 & 0xff;
		buf[1] = (w0 >> 8) & 0xff;

		buf += 2;
		length -= 2;
	}

	if (length >= 1) {
		u32 w0 = readl(card);

		buf[0] = w0 & 0xff;
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

	while (length >= 16) {
		u32 v0 = ((const u32 *)buf)[0];
		u32 v1 = ((const u32 *)buf)[1];
		u32 v2 = ((const u32 *)buf)[2];
		u32 v3 = ((const u32 *)buf)[3];

		writel((v0 & 0xffff) | ((v0 & 0xffff) << 16), card + 0);
		writel((v0 >> 16)   | ((v0 >> 16)   << 16), card + 1);
		writel((v1 & 0xffff) | ((v1 & 0xffff) << 16), card + 2);
		writel((v1 >> 16)   | ((v1 >> 16)   << 16), card + 3);
		writel((v2 & 0xffff) | ((v2 & 0xffff) << 16), card + 4);
		writel((v2 >> 16)   | ((v2 >> 16)   << 16), card + 5);
		writel((v3 & 0xffff) | ((v3 & 0xffff) << 16), card + 6);
		writel((v3 >> 16)   | ((v3 >> 16)   << 16), card + 7);

		card += 8;
		buf  += 16;
		length -= 16;
	}

	while (length >= 8) {
		u32 v0 = ((const u32 *)buf)[0];
		u32 v1 = ((const u32 *)buf)[1];

		writel((v0 & 0xffff) | ((v0 & 0xffff) << 16), card + 0);
		writel((v0 >> 16)   | ((v0 >> 16)   << 16), card + 1);
		writel((v1 & 0xffff) | ((v1 & 0xffff) << 16), card + 2);
		writel((v1 >> 16)   | ((v1 >> 16)   << 16), card + 3);

		card += 4;
		buf  += 8;
		length -= 8;
	}

	while (length >= 4) {
		u32 v0 = ((const u32 *)buf)[0];

		writel((v0 & 0xffff) | ((v0 & 0xffff) << 16), card + 0);
		writel((v0 >> 16)   | ((v0 >> 16)   << 16), card + 1);

		card += 2;
		buf  += 4;
		length -= 4;
	}

	while (length >= 2) {
		u16 v = *(const u16 *)buf;
		void __iomem *cp = (void __iomem *)card;

		writeb(v & 0xff, cp);
		writeb(v >> 8, cp + 1);

		card++;
		buf += 2;
		length -= 2;
	}

	if (length >= 1)
		writeb(*buf, (void __iomem *)card);
}
