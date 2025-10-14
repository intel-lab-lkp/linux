/* SPDX-License-Identifier: GPL-2.0-only OR BSD 2-Clause */
/*
 * This header provides macros for MAXIM MAX77675 device bindings.
 *
 * Copyright (c) 2025, Analog Device inc.
 * Author: Joan Na <joan.na@analog.com>
 */

#ifndef _DT_BINDINGS_REGULATOR_MAX77675_
#define _DT_BINDINGS_REGULATOR_MAX77675_

/* FPS source */
#define MAX77675_FPS_SLOT_0       0x0
#define MAX77675_FPS_SLOT_1       0x1
#define MAX77675_FPS_SLOT_2       0x2
#define MAX77675_FPS_SLOT_3       0x3
#define MAX77675_FPS_DEF          0x4

/* nEN Manual Reset Time Configuration (MRT) */
#define MAX77675_MRT_4S           0x0
#define MAX77675_MRT_8S           0x1
#define MAX77675_MRT_12S          0x2
#define MAX77675_MRT_16S          0x3

/* nEN Mode Configuration */
#define MAX77675_EN_PUSH_BUTTON   0x0
#define MAX77675_EN_SLIDE_SWITCH  0x1
#define MAX77675_EN_LOGIC         0x2

/* Debounce Timer Enable (DBEN_nEN) */
#define MAX77675_DBEN_100US       0x0
#define MAX77675_DBEN_30000US     0x1

/* Rising slew rate control for SBB0 when ramping up */
#define MAX77675_SR_2MV_PER_US    0x0  // 2 mV/us
#define MAX77675_SR_USE_DVS       0x1  // Use DVS slew rate setting (maxim,dvs-slew-rate)

/* Dynamic Voltage Scaling (DVS) Slew Rate */
#define MAX77675_DVS_SLEW_5MV_PER_US    0x0  // 5 mV/us
#define MAX77675_DVS_SLEW_10MV_PER_US   0x1  // 10 mV/us

/* Latency Mode */
#define MAX77675_HIGH_LATENCY_MODE  0x0   // High latency, low quiescent current (~100us)
#define MAX77675_LOW_LATENCY_MODE   0x1   // Low latency, high quiescent current (~10us)

/* SIMO Buck-Boost Drive Strength (All Channels) */
#define MAX77675_DRV_SBB_STRENGTH_MAX  0x0  // Maximum drive strength (~0.6 ns transition time)
#define MAX77675_DRV_SBB_STRENGTH_HIGH 0x1  // High drive strength (~1.2 ns transition time)
#define MAX77675_DRV_SBB_STRENGTH_LOW  0x2  // Low drive strength (~1.8 ns transition time)
#define MAX77675_DRV_SBB_STRENGTH_MIN  0x3  // Minimum drive strength (~8 ns transition time)

#endif
