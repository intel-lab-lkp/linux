/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Reset a DECstation machine.
 *
 * Copyright (C) 2025  WangYuli
 */

#ifndef __ASM_DEC_RESET_H

#include <linux/interrupt.h>

extern void __noreturn dec_machine_restart(char *command);
extern void __noreturn dec_machine_halt(void);
extern void __noreturn dec_machine_power_off(void);
extern irqreturn_t dec_intr_halt(int irq, void *dev_id);

#endif /* __ASM_DEC_RESET_H */
