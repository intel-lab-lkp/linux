/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Acorn SCSI card I/O
 */

#ifndef _ACORNSCSI_IO_H
#define _ACORNSCSI_IO_H

#include <linux/io.h>

void __acornscsi_in(void __iomem *addr, char *buffer, int length);
void __acornscsi_out(void __iomem *addr, char *buffer, int length);

#endif /* _ACORNSCSI_IO_H */
