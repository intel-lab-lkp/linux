/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_STATE_H__
#define __MALI_VPU_STATE_H__

/*
 * Decoder session state machine
 *
 *            open
 *              |
 *              |
 *              v
 *      +----------------+   ->  ->   ->   StreamOff(Output)    ->   ->   ->      +------------+
 *      | INITIALIZATION |<------------------------------------------------------>| RESET/SEEK |
 *      +----------------+  <-  <-   StreamOff(Capture)/REQBUF(OUTPUT, 0) <-  <-  +------------+
 *              ^                                                                          ^
 *              | capture format                                                           |
 *              |  established                                            StreamOff(Output)|
 *              |                                                                          |
 *              |                      +---------------+                                   |
 *              +--------------------->|               |-----------------------------------+
 * +---------------------------------->| CAPTURE_SETUP/|                                   |
 * |                                   |    STOPPED    |                                   |
 * |            +--------------------->|               |                                   |
 * |            |capture format        +---------------+                                   |
 * |            | established            |           ^                                     |
 * |  +-------------------+              |           |                                     |
 * |  | RESOLUTION_CHANGE |              |           |                                     |
 * |  +-------------------+              |           |                                     |
 * |            ^                        |           |   StreamOff(Capture)  +-------+     |
 * |            |                        |           +-----------------------| DRAIN |     |
 * |            |                        |                                   +-------+     |
 * |            |                        |                                      ^          |
 * |            |                        |START_CMD/                            |          |
 * |            |                        |StreamOn                              |          |
 * |            |                        |(Capture)                             |          |
 * |            |  Stream                |                                      |          |
 * |            |Resolution              |                                      |          |
 * |            |  Change                |                                      |          |
 * |            |                        |                                      |          |
 * |            |                        |                                      |          |
 * |            |                        |                                      |STOP_CMD/ |
 * |            |                        |                                      |EOS Mark  |
 * |            |                        |                                      |          |
 * |            |                        v                                      |          |
 * |            |                      +---------------+                        |          |
 * |            +----------------------|   STREAMING   |------------------------+          |
 * |                                   +---------------+                                   |
 * |                                      ^     ^                                          |
 * |                                      |     |  ->  ->    StreamOff(Output)   ->   ->   |
 * +--------------------------------------+     +------------------------------------------+
 *                                                  <-  <-   StreamOn(Output)   <-  <-
 */

/*
 * INITIALIZATION -> STOPPED			OUTPUT STREAMON happens first. Firmware may parse
 *						source headers while CAPTURE is not configured.
 *
 * STOPPED -> INITIALIZATION			OUTPUT STREAMOFF while CAPTURE is stopped.
 *						Now neither queue is streaming.
 *
 * INITIALIZATION -> RESET			CAPTURE STREAMON happens first, as exercised by the
 *						compliance blocking test. Firmware should not start
 *						decoding yet.
 *
 * RESET -> INITIALIZATION			CAPTURE STREAMOFF while OUTPUT is already stopped.
 *						Now neither queue is streaming.
 *
 * RESET -> STREAMING				OUTPUT STREAMON after CAPTURE was started first.
 *						Both queues are now streaming.
 *
 * STREAMING -> RESET				OUTPUT STREAMOFF first; CAPTURE remains streaming.
 *
 * STREAMING -> DYNAMIC_RESOLUTION_CHANGE	Firmware reports changed stream parameters while
 *						actively decoding. The driver flushes CAPTURE and
 *						sends V4L2_EVENT_SOURCE_CHANGE.
 *
 * DYNAMIC_RESOLUTION_CHANGE -> STOPPED		Userspace performs CAPTURE STREAMOFF following the
 *						source-change event. OUTPUT remains streaming.
 *
 * STREAMING -> DRAIN				V4L2_DEC_CMD_STOP is received or implicit EOS is
 *						detected on OUTPUT/source.
 *
 * DRAIN -> STOPPED				Firmware returns the final CAPTURE buffer with
 *						LAST/EOS, completing the drain.
 *
 * DRAIN -> RESET				OUTPUT STREAMOFF occurs during draining;
 *						CAPTURE remains streaming.
 *
 * STOPPED -> STREAMING				Userspace sends V4L2_DEC_CMD_START to continue
 *						decoding while both queues remain streaming./
 *						CAPTURE STREAMON after OUTPUT was started first.
 *						Both queues are now streaming.
 *
 * STOPPED -> FIRST_SOURCE_CHANGE		Firmware reports first RESOLUTION CHANGE in sequence
 *						occurs during before CAPTURE is configured; it is
 *						not runtime DRC. The driver flushes CAPTURE and
 *						sends V4L2_EVENT_SOURCE_CHANGE.
 *
 * FIRST_SOURCE_CHANGE -> STREAMING		Userspace on receiving V4L2_EVENT_SOURCE_CHANGE for
 *						FIRST_SOURCE_CHANGE will allocate CAPTURE BUFFERS
 *						and call streamOn which will move the state to
 *						STREAMING as both queues are now streaming.
 *
 * STOPPED -> RESET				OUTPUT STREAMOFF after drain completion; CAPTURE
 *						remains streaming. TO BE VERIFIED
 */

/**
 * enum mali_vpu_sess_state - VPU session streaming states
 *
 * @MALI_VPU_STATE_INITIALIZATION: Session is initialized with no queues streaming.
 * @MALI_VPU_STATE_STREAMING: Session both queues streaming.
 * @MALI_VPU_STATE_STOPPED: For decoder session CAPTURE is not streaming.
 * @MALI_VPU_STATE_DRAIN: For decoder session all CAPTURE buffers returned
 * @MALI_VPU_STATE_RESET: For decoder session OUTPUT is not streaming.
 * @MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE: Decoder only for mid stream resolution change
 * @MALI_VPU_DEC_STATE_FIRST_SOURCE_CHANGE: Decoder only for first source change when CAPTURE
 * not streaming.
 */
enum mali_vpu_sess_state {
	/* Common states for encoder and decoder */
	MALI_VPU_STATE_INITIALIZATION = 0,
	MALI_VPU_STATE_STREAMING,
	MALI_VPU_STATE_STOPPED,
	MALI_VPU_STATE_DRAIN,
	MALI_VPU_STATE_RESET,
	/* Decoder only states */
	MALI_VPU_DEC_STATE_DYNAMIC_RESOLUTION_CHANGE,
	MALI_VPU_DEC_STATE_FIRST_SOURCE_CHANGE,
};

#endif /* __MALI_VPU_STATE_H__ */
