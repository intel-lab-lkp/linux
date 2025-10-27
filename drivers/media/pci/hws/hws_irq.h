/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HWS_INTERRUPT_H
#define HWS_INTERRUPT_H

#include <linux/pci.h>
#include "hws.h"

irqreturn_t hws_irq_handler(int irq, void *info);
void hws_dpc_audio(unsigned long data);
void hws_bh_video(struct tasklet_struct *t);

#endif /* HWS_INTERRUPT_H */
