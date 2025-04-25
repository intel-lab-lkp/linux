/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ZL3073X_REGS_H
#define __ZL3073X_REGS_H

#include <linux/mfd/zl3073x-regs.h>

/**************************
 * Register Page 0, General
 **************************/

#define ZL_REG_ID				ZL_REG(0, 0x01, 2)
#define ZL_REG_REVISION				ZL_REG(0, 0x03, 2)
#define ZL_REG_FW_VER				ZL_REG(0, 0x05, 2)
#define ZL_REG_CUSTOM_CONFIG_VER		ZL_REG(0, 0x07, 4)

#endif /* __ZL3073X_REGS_H */
