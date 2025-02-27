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
 * \__  Extension capture0    (RA register)
 * \__  Extension capture1    (RB register)
 * \__  Extension capture2    (RC register)
 *
 * It also supports the following events:
 *
 * Channel 0:
 * - CV register changed
 * - CV overflowed
 * - RA captured
 * Channel 1:
 * - RB captured
 * Channel 2:
 * - RC compare triggered
 */

enum counter_mchp_signals {
	COUNTER_MCHP_SIG_TIOA,
	COUNTER_MCHP_SIG_TIOB,
};

enum counter_mchp_capture_extensions {
	COUNTER_MCHP_EXCAP_RA,
	COUNTER_MCHP_EXCAP_RB,
	COUNTER_MCHP_EXCAP_RC,
};

enum counter_mchp_event_channels {
	COUNTER_MCHP_EVCHN_CV = 0,
	COUNTER_MCHP_EVCHN_RA = 0,
	COUNTER_MCHP_EVCHN_RB,
	COUNTER_MCHP_EVCHN_RC,
};

#endif /* _UAPI_COUNTER_MCHP_TCB_H_ */
