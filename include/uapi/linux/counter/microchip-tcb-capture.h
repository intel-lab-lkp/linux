/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Channel numbers used by the microchip-tcb-capture driver
 * Copyright (C) 2025 Bence Csókás
 */
#ifndef _UAPI_COUNTER_MCHP_TCB_H_
#define _UAPI_COUNTER_MCHP_TCB_H_

/*
 * The driver defines the following components:
 *
 * Count 0
 * \__  Synapse 0 -- Signal 0 (Channel A, i.e. TIOA)
 * \__  Synapse 1 -- Signal 1 (Channel B, i.e. TIOB)
 */

enum counter_mchp_signals {
	COUNTER_MCHP_SIG_TIOA,
	COUNTER_MCHP_SIG_TIOB,
};

#endif /* _UAPI_COUNTER_MCHP_TCB_H_ */
