/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef _DT_BINDINGS_REGULATOR_MEDIATEK_MT6392_H_
#define _DT_BINDINGS_REGULATOR_MEDIATEK_MT6392_H_

/*
 * Buck mode constants which may be used in devicetree properties (eg.
 * regulator-initial-mode, regulator-allowed-modes).
 * See the manufacturer's datasheet for more information on these modes.
 */

#define MT6392_BUCK_MODE_AUTO		0
#define MT6392_BUCK_MODE_FORCE_PWM	1

/*
 * LDO mode constants which may be used in devicetree properties (eg.
 * regulator-initial-mode, regulator-allowed-modes).
 * See the manufacturer's datasheet for more information on these modes.
 */

#define MT6392_LDO_MODE_NORMAL		0
#define MT6392_LDO_MODE_LP		1

#endif
