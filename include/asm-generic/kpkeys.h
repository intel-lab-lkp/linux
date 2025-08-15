/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_GENERIC_KPKEYS_H
#define __ASM_GENERIC_KPKEYS_H

#ifndef KPKEYS_PKEY_DEFAULT
#define KPKEYS_PKEY_DEFAULT	0
#endif

/*
 * Represents a pkey register value that cannot be used, typically disabling
 * access to all keys.
 */
#ifndef KPKEYS_PKEY_REG_INVAL
#define KPKEYS_PKEY_REG_INVAL	0
#endif

#endif	/* __ASM_GENERIC_KPKEYS_H */
