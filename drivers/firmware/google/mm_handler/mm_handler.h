/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Derived from arch/x86/include/realmode.h
 */

#ifndef _MM_HANDLER_H
#define _MM_HANDLER_H

#define REALMODE_END_SIGNATURE	0x65a22c82

/*
 * These macros correspond to the arguments
 * passed by coreboot's SMI handler. Depending
 * on which one is passed in rdi or esp + x, handler
 * will jump to the appropriate section.
 */
#define MM_ACPI_ENABLE		1
#define MM_ACPI_DISABLE		0
#define MM_STORE		2

#endif /* _MM_HANDLER_H */
