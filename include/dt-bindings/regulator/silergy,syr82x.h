/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2026 Arturia - All rights reserved.
 *
 * Device Tree binding constants for the Sylergy SYR827 and SYR828 PMIC regulators
 */

#ifndef _DT_BINDINGS_REGULATOR_SYR82X_H
#define _DT_BINDINGS_REGULATOR_SYR82X_H

/*
 * Constants to specify regulator modes in device tree for SYR82X regulators
 * SYR82X_REGULATOR_MODE_FORCE_PWM:	Force fixed PWM mode
 * SYR82X_REGULATOR_MODE_AUTO:		Allow auto-PFM mode during light load
 */

#define SYR82X_REGULATOR_MODE_FORCE_PWM	1
#define SYR82X_REGULATOR_MODE_AUTO	2

#endif
