/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 Herbert Xu <herbert@gondor.apana.org.au>
 */
#include <asm/ppc_asm.h>
#include <linux/export.h>

_GLOBAL(__lshrti3)
	cmplwi	r5,63
	ble	1f
	addi	r5,r5,-64
	srd	r3,r4,r5
	li	r4,0
	blr
1:
	subfic	r7,r5,64
	srd	r3,r3,r5
	sld	r6,r4,r7
	srd	r4,r4,r5
	or	r3,r3,r6
	blr
EXPORT_SYMBOL(__lshrti3)

_GLOBAL(__ashrti3)
	cmplwi	r5,63
	ble	1f
	addi	r5,r5,-64
	srad	r3,r4,r5
	sradi	r4,r4,63
	blr
1:
	subfic	r7,r5,64
	srd	r3,r3,r5
	sld	r6,r4,r7
	srad	r4,r4,r5
	or	r3,r3,r6
	blr
EXPORT_SYMBOL(__ashrti3)

_GLOBAL(__ashlti3)
	cmplwi	r5,63
	ble	1f
	addi	r5,r5,-64
	sld	r4,r3,r5
	li	r3,0
	blr
1:
	subfic	r7,r5,64
	sld	r4,r4,r5
	srd	r6,r3,r7
	sld	r3,r3,r5
	or	r4,r4,r6
	blr
EXPORT_SYMBOL(__ashlti3)
