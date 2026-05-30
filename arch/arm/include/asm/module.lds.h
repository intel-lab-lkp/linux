/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_ARM_UNWIND
SECTIONS {
	.ARM.extab		0 : {
		*(.ARM.extab .ARM.extab.text .ARM.extab.text.[0-9a-zA-Z_]*)
	}
	.ARM.exidx		0 : {
		*(.ARM.exidx .ARM.exidx.text .ARM.exidx.text.[0-9a-zA-Z_]*)
	}
}
#endif

#ifdef CONFIG_ARM_MODULE_PLTS
SECTIONS {
	.plt : { BYTE(0) }
	.init.plt : { BYTE(0) }
}
#endif
