// SPDX-License-Identifier: GPL-2.0
/*
 * Generic Reed Solomon encoder / decoder library
 *
 * Copyright 2002, Phil Karn, KA9Q
 * May be used under the terms of the GNU General Public License (GPL)
 *
 * Adaption to the kernel by Thomas Gleixner (tglx@linutronix.de)
 *
 * Generic data width independent code which is included by the wrappers.
 */
{
	struct rs_codec *rs = rsc->codec;
	int i, j, pad;
	int nn = rs->nn;
	int nroots = rs->nroots;
	uint16_t *alpha_to = rs->alpha_to;
	uint16_t *index_of = rs->index_of;
	uint16_t *genpoly = rs->genpoly;
	uint16_t fb;
	uint16_t msk = (uint16_t) rs->nn;

	/* Check length parameter for validity */
	pad = nn - nroots - len;
	if (pad < 0 || pad >= nn)
		return -ERANGE;

	for (i = 0; i < len; i++) {
		fb = index_of[((((uint16_t) data[i])^invmsk) & msk) ^ par[0]];
		if (fb != nn) {
			/* feedback term is non-zero */
			for (j = 1; j < nroots; j++)
				par[j - 1] = par[j] ^ alpha_to[rs_modnn_fast(rs,
						      fb +
						      genpoly[nroots - j])];
			par[nroots - 1] = alpha_to[rs_modnn_fast(rs,
					  fb +
					  genpoly[0])];
		} else {
			for (j = 1; j < nroots; j++)
				par[j - 1] = par[j];
			par[nroots - 1] = 0;
		}
	}
	return 0;
}
