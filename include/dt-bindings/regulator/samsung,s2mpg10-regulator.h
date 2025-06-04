/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright 2021 Google LLC
 * Copyright 2025 Linaro Ltd.
 *
 * Device Tree binding constants for the Samsung S2MPG1x PMIC regulators
 */

#ifndef _DT_BINDINGS_REGULATOR_SAMSUNG_S2MPG10_H
#define _DT_BINDINGS_REGULATOR_SAMSUNG_S2MPG10_H

/*
 * Several regulators may be controlled via external signals instead of via
 * software. These constants describe the possible signals for such regulators
 * and generally correspond to the respecitve on-chip pins. The constants
 * suffixed with _TRG enable control using the respective bits in the
 * MIMICKING_CTRL register instead.
 *
 * S2MPG10 regulators supporting these are:
 * - buck1m .. buck7m buck10m
 * - ldo3m .. ldo19m
 *
 * ldo20m supports external control, but using a different set of control
 * signals.
 *
 * S2MPG11 regulators supporting these are:
 * - buck1s .. buck3s buck5s buck8s buck9s bucka buckd
 * - ldo1s ldo2s ldo8s ldo13s
 */
#define S2MPG10_PCTRLSEL_ON               0x0 /* always on */
#define S2MPG10_PCTRLSEL_PWREN            0x1 /* PWREN pin */
#define S2MPG10_PCTRLSEL_PWREN_TRG        0x2 /* PWREN_TRG bit in MIMICKING_CTRL */
#define S2MPG10_PCTRLSEL_PWREN_MIF        0x3 /* PWREN_MIF pin */
#define S2MPG10_PCTRLSEL_PWREN_MIF_TRG    0x4 /* PWREN_MIF_TRG bit in MIMICKING_CTRL */
#define S2MPG10_PCTRLSEL_AP_ACTIVE_N      0x5 /* ~AP_ACTIVE_N pin */
#define S2MPG10_PCTRLSEL_AP_ACTIVE_N_TRG  0x6 /* ~AP_ACTIVE_N_TRG bit in MIMICKING_CTRL */
#define S2MPG10_PCTRLSEL_CPUCL1_EN        0x7 /* CPUCL1_EN pin */
#define S2MPG10_PCTRLSEL_CPUCL1_EN2       0x8 /* CPUCL1_EN & PWREN pins */
#define S2MPG10_PCTRLSEL_CPUCL2_EN        0x9 /* CPUCL2_EN pin */
#define S2MPG10_PCTRLSEL_CPUCL2_EN2       0xa /* CPUCL2_E2 & PWREN pins */
#define S2MPG10_PCTRLSEL_TPU_EN           0xb /* TPU_EN pin */
#define S2MPG10_PCTRLSEL_TPU_EN2          0xc /* TPU_EN & ~AP_ACTIVE_N pins */
#define S2MPG10_PCTRLSEL_TCXO_ON          0xd /* TCXO_ON pin */
#define S2MPG10_PCTRLSEL_TCXO_ON2         0xe /* TCXO_ON & ~AP_ACTIVE_N pins */

#define S2MPG10_PCTRLSEL_LDO20M_ON        0x0 /* always on */
#define S2MPG10_PCTRLSEL_LDO20M_EN_SFR    0x1 /* LDO20M_EN & LDO20M_SFR */
#define S2MPG10_PCTRLSEL_LDO20M_EN        0x2 /* VLDO20M_EN pin */
#define S2MPG10_PCTRLSEL_LDO20M_SFR       0x3 /* LDO20M_SFR bit in LDO_CTRL1 register */
#define S2MPG10_PCTRLSEL_LDO20M_OFF       0x4 /* disable */

#define S2MPG11_PCTRLSEL_ON               0x0 /* always on */
#define S2MPG11_PCTRLSEL_PWREN            0x1 /* PWREN pin */
#define S2MPG11_PCTRLSEL_PWREN_TRG        0x2 /* PWREN_TRG bit in MIMICKING_CTRL */
#define S2MPG11_PCTRLSEL_PWREN_MIF        0x3 /* PWREN_MIF pin */
#define S2MPG11_PCTRLSEL_PWREN_MIF_TRG    0x4 /* PWREN_MIF_TRG bit in MIMICKING_CTRL */
#define S2MPG11_PCTRLSEL_AP_ACTIVE_N      0x5 /* ~AP_ACTIVE_N pin */
#define S2MPG11_PCTRLSEL_AP_ACTIVE_N_TRG  0x6 /* ~AP_ACTIVE_N_TRG bit in MIMICKING_CTRL */
#define S2MPG11_PCTRLSEL_G3D_EN           0x7 /* G3D_EN pin */
#define S2MPG11_PCTRLSEL_G3D_EN2          0x8 /* G3D_EN & ~AP_ACTIVE_N pins */
#define S2MPG11_PCTRLSEL_AOC_VDD          0x9 /* AOC_VDD pin */
#define S2MPG11_PCTRLSEL_AOC_RET          0xa /* AOC_RET pin */
#define S2MPG11_PCTRLSEL_UFS_EN           0xb /* UFS_EN pin */
#define S2MPG11_PCTRLSEL_LDO13S_EN        0xc /* VLDO13S_EN pin */

#endif /* _DT_BINDINGS_REGULATOR_SAMSUNG_S2MPG10_H */
