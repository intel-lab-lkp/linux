/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_HYGON_FAMILY_H
#define _ASM_X86_HYGON_FAMILY_H

/*
 * The helpers to support Hygon CPU specific code path.
 */

#include <asm/processor.h>

#define HFM(_family, _model)	VFM_MAKE(X86_VENDOR_HYGON, _family, _model)

#define HYGON_F18_M04		HFM(0x18, 4)
#define HYGON_F18_M06		HFM(0x18, 6)
#define HYGON_F18_M07		HFM(0x18, 7)

#endif /* _ASM_X86_HYGON_FAMILY_H */
