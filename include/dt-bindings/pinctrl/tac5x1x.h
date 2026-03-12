/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Device Tree binding constants for TAC5x1x pinctrl
 *
 * Copyright (C) 2025 Texas Instruments Incorporated
 * Author: Niranjan H Y <niranjan.hy@ti.com>
 */

#ifndef _DT_BINDINGS_PINCTRL_TAC5X1X_H
#define _DT_BINDINGS_PINCTRL_TAC5X1X_H

/* Pin IDs */
#define TAC5X1X_PIN_GPIO1 0
#define TAC5X1X_PIN_GPIO2 1
#define TAC5X1X_PIN_GPO1 2
#define TAC5X1X_PIN_GPI1 3
#define TAC5X1X_PIN_GPI2A 4

/* Pin functions */
#define TAC5X1X_FUNC_GPIO 0
#define TAC5X1X_FUNC_PDM 1
#define TAC5X1X_FUNC_IRQ 2

/* Pin drive modes */
#define TAC5X1X_DRIVE_HIZ 0
#define TAC5X1X_DRIVE_PUSH_PULL 1
#define TAC5X1X_DRIVE_PULL_DOWN 2
#define TAC5X1X_DRIVE_OPEN_DRAIN 3
#define TAC5X1X_DRIVE_PULL_UP 4
#define TAC5X1X_DRIVE_OPEN_SOURCE 5

/* PDM configurations */
#define TAC5X1X_PDM_GPIO1_GPIO2 0
#define TAC5X1X_PDM_GPIO1_GPI1 1
#define TAC5X1X_PDM_GPIO1_GPI2A 2
#define TAC5X1X_PDM_GPIO2_GPIO1 3
#define TAC5X1X_PDM_GPIO2_GPI1 4
#define TAC5X1X_PDM_GPIO2_GPI2A 5
#define TAC5X1X_PDM_GPO1_GPIO1 6
#define TAC5X1X_PDM_GPO1_GPIO2 7
#define TAC5X1X_PDM_GPO1_GPI1 8
#define TAC5X1X_PDM_GPO1_GPI2A 9

#endif /* _DT_BINDINGS_PINCTRL_TAC5X1X_H */
