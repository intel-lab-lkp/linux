/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_GENERIC_DYNDBG_LDS_H
#define __ASM_GENERIC_DYNDBG_LDS_H

#include <asm-generic/bounded_sections.lds.h>
#define DYNDBG_SECTIONS()					\
	BOUNDED_SECTION_BY(__dyndbg, ___dyndbg)			\
	BOUNDED_SECTION_BY(__dyndbg_classes, ___dyndbg_classes)

#define MOD_DYNDBG_SECTIONS()						\
	__dyndbg 0 : ALIGN(8) {						\
		KEEP(*(__dyndbg))					\
	}								\
	__dyndbg_classes 0 : ALIGN(8) {					\
		KEEP(*(__dyndbg_classes))				\
	}

#endif /* __ASM_GENERIC_DYNDBG_LDS_H */
