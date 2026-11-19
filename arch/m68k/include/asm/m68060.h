/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_M68K_M68060_H
#define _ASM_M68K_M68060_H

/* From MC68060 User's Manual (section 3.2.2.5) */
#define M68060_PCR_ID		0xffff0000
#define M68060_PCR_ID_68060	0x0430
#define M68060_PCR_ID_68LC060	0x0431		/* LC or EC */
#define M68060_PCR_REVISION	0x0000ff00
#define M68060_PCR_EDEBUG	0x00000080	/* internal state on idle bus cycles */
#define M68060_PCR_I14		0x00000020	/* Reserved: errata I14 workaround */
#define M68060_PCR_DFP		0x00000002	/* disable the FPU */
#define M68060_PCR_ESS		0x00000001	/* enable superscalar dispatch */

#endif /* _ASM_M68K_M68060_H */
