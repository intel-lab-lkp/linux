/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#ifndef __NET_PON_PLOAM_MSG_H
#define __NET_PON_PLOAM_MSG_H

#include <linux/types.h>
#include <net/pon/ploam.h>

/**
 * DOC: The PLOAM message codec
 *
 * Lays out and reads the standard message body. It is a pure function of an
 * ITU-T wire format: no state, no timer, no register and no policy. A driver
 * decides what a message means and when to send one. This decides where the
 * bytes go.
 *
 * The body is what the standard defines and nothing else: the ONU-ID, the
 * message identifier, the sequence number and 36 bytes of content. Whatever a
 * MAC wraps around that, a prefix, a trailer, a FIFO word order or a message
 * integrity check, stays in that MAC's driver.
 */

/* The generic PLOAM message structure, G.9807.1 Table C.11.1. */
#define PON_PLOAM_CONTENT_LEN	36
#define PON_PLOAM_BODY_LEN	(2 + 1 + 1 + PON_PLOAM_CONTENT_LEN)

/**
 * struct pon_ploam_sn - Serial_Number_ONU
 * @sn: the eight byte serial number
 * @random_delay: the response delay the ONU applies, in bit periods at
 *	2.48832 Gbit/s whatever the upstream rate of the ONU
 *
 * ITU-T G.9807.1 Table C.11.24.
 */
struct pon_ploam_sn {
	u8 sn[PON_PLOAM_SN_LEN];
	u32 random_delay;
};

/**
 * struct pon_ploam_registration - Registration
 * @reg_id: the registration id
 *
 * ITU-T G.9807.1 Table C.11.25.
 */
struct pon_ploam_registration {
	u8 reg_id[PON_PLOAM_REG_ID_LEN];
};

/**
 * struct pon_ploam_key_report - Key_Report
 * @key: the key fragment
 * @len: its length, at most PON_PLOAM_KEY_FRAGMENT_LEN
 * @type: new key or existing key, PON_PLOAM_KEY_REPORT_TYPE_*
 * @index: the reported key index
 * @num: the fragment number
 *
 * ITU-T G.9807.1 Table C.11.26.
 */
struct pon_ploam_key_report {
	u8 key[PON_PLOAM_KEY_FRAGMENT_LEN];
	u8 len;
	u8 type;
	u8 index;
	u8 num;
};

/**
 * struct pon_ploam_ack_msg - Acknowledgment
 * @code: the completion code, enum pon_ploam_ack
 *
 * ITU-T G.9807.1 Table C.11.27.
 */
struct pon_ploam_ack_msg {
	u8 code;
};

/**
 * struct pon_ploam_up - one upstream message to lay out
 * @onu_id: the ONU-ID to send it from, at most PON_PLOAM_ONU_ID_MASK
 * @msg_id: which message, enum pon_ploam_up_id
 * @seq_no: the downstream sequence number to echo, or 0
 * @sn: Serial_Number_ONU content
 * @registration: Registration content
 * @key_report: Key_Report content
 * @ack: Acknowledgment content
 *
 * ITU-T G.9807.1 Table C.11.3, the upstream message summary.
 */
struct pon_ploam_up {
	u16 onu_id;
	u8 msg_id;
	u8 seq_no;
	union {
		struct pon_ploam_sn sn;
		struct pon_ploam_registration registration;
		struct pon_ploam_key_report key_report;
		struct pon_ploam_ack_msg ack;
	};
};

int pon_ploam_up_build(void *buf, size_t len, const struct pon_ploam_up *msg);

/* The Burst_Profile field sizes, G.9807.1 Table C.11.4. */
#define PON_PLOAM_BURST_PATTERN_LEN	8
#define PON_PLOAM_PON_TAG_LEN		8

/**
 * struct pon_ploam_burst_profile - Burst_Profile
 * @delimiter: the burst delimiter pattern
 * @preamble: the burst preamble pattern
 * @pon_tag: the PON tag the OLT assigns
 * @index: which of the profiles this one is
 * @version: the profile version
 * @line_rate: the upstream line rate, PON_PLOAM_LINE_RATE_*
 * @fec: upstream FEC is on
 * @delimiter_len: significant bytes of @delimiter
 * @preamble_len: significant bytes of @preamble
 * @preamble_repeat: how many times the preamble repeats, masked to the width
 *	that @line_rate gives the field: five bits for
 *	PON_PLOAM_LINE_RATE_XGPON, eight for PON_PLOAM_LINE_RATE_XGSPON
 *
 * ITU-T G.9807.1 Table C.11.4.
 */
struct pon_ploam_burst_profile {
	u8 delimiter[PON_PLOAM_BURST_PATTERN_LEN];
	u8 preamble[PON_PLOAM_BURST_PATTERN_LEN];
	u8 pon_tag[PON_PLOAM_PON_TAG_LEN];
	u8 index;
	u8 version;
	u8 line_rate;
	u8 fec;
	u8 delimiter_len;
	u8 preamble_len;
	u8 preamble_repeat;
};

/**
 * struct pon_ploam_assign_onu_id - Assign_ONU-ID
 * @sn: the serial number the assignment is for
 * @onu_id: the ONU-ID being assigned
 * @line_rate: the upstream nominal line rate the OLT selects,
 *	PON_PLOAM_LINE_RATE_*. It applies only to an ONU that supports both
 *	upstream rates
 *
 * ITU-T G.9807.1 Table C.11.6.
 */
struct pon_ploam_assign_onu_id {
	u8 sn[PON_PLOAM_SN_LEN];
	u16 onu_id;
	u8 line_rate;
};

/**
 * struct pon_ploam_ranging_time - Ranging_Time
 * @eqd: the equalization delay, in bit periods at 2.48832 Gbit/s whatever
 *	the upstream rate of the ONU
 * @absolute: @eqd replaces the current value rather than adjusting it
 * @positive: an adjustment adds rather than subtracts
 *
 * ITU-T G.9807.1 Table C.11.7.
 */
struct pon_ploam_ranging_time {
	u32 eqd;
	bool absolute;
	bool positive;
};

/**
 * struct pon_ploam_disable_sn - Disable_Serial_Number
 * @sn: the serial number the mode applies to
 * @mode: enum pon_ploam_disable_mode
 *
 * ITU-T G.9807.1 Table C.11.9.
 */
struct pon_ploam_disable_sn {
	u8 sn[PON_PLOAM_SN_LEN];
	u8 mode;
};

/**
 * struct pon_ploam_assign_alloc_id - Assign_Alloc-ID
 * @alloc_id: the alloc-id
 * @type: assign or deallocate, PON_PLOAM_ALLOC_*
 *
 * ITU-T G.9807.1 Table C.11.11.
 */
struct pon_ploam_assign_alloc_id {
	u16 alloc_id;
	u8 type;
};

/**
 * struct pon_ploam_key_control - Key_Control
 * @key_index: the key index to report
 * @control: the Control flag, generate a new key or confirm a key,
 *	PON_PLOAM_KEY_CONTROL_*
 * @key_length: the key length the OLT asks for, in bytes, where 0 means 256
 *
 * ITU-T G.9807.1 Table C.11.12.
 */
struct pon_ploam_key_control {
	u8 key_index;
	u8 control;
	u8 key_length;
};

/**
 * struct pon_ploam_reboot - Reboot_ONU
 * @sn: the serial number a broadcast message names, all zero for every ONU
 * @depth: the reboot depth, where 0 is an OMCI MIB reset
 * @image: which software image to load
 * @state: the activation states the reboot applies in
 * @flags: the call-in-progress conditions
 *
 * ITU-T G.9807.1 Table C.11.23A.
 */
struct pon_ploam_reboot {
	u8 sn[PON_PLOAM_SN_LEN];
	u8 depth;
	u8 image;
	u8 state;
	u8 flags;
};

/**
 * struct pon_ploam_down - one downstream message, parsed
 * @onu_id: the destination, the ten bits the standard defines
 * @msg_id: which message, enum pon_ploam_down_id
 * @seq_no: the sequence number to echo in an acknowledgment
 * @burst_profile: Burst_Profile content
 * @assign_onu_id: Assign_ONU-ID content
 * @ranging_time: Ranging_Time content
 * @disable_sn: Disable_Serial_Number content
 * @assign_alloc_id: Assign_Alloc-ID content
 * @key_control: Key_Control content
 * @reboot: Reboot_ONU content
 *
 * ITU-T G.9807.1 Table C.11.2, the downstream message summary.
 */
struct pon_ploam_down {
	u16 onu_id;
	u8 msg_id;
	u8 seq_no;
	union {
		struct pon_ploam_burst_profile burst_profile;
		struct pon_ploam_assign_onu_id assign_onu_id;
		struct pon_ploam_ranging_time ranging_time;
		struct pon_ploam_disable_sn disable_sn;
		struct pon_ploam_assign_alloc_id assign_alloc_id;
		struct pon_ploam_key_control key_control;
		struct pon_ploam_reboot reboot;
	};
};

int pon_ploam_down_parse(const void *buf, size_t len,
			 struct pon_ploam_down *msg);

/**
 * pon_ploam_is_broadcast() - whether an ONU-ID is the broadcast destination
 * @onu_id: the ten bit ONU-ID of a downstream message
 *
 * Only 0x3ff counts. 0x3fe, the broadcast of a Burst_Profile for the
 * 9.95328 Gbit/s upstream rate, does not. ITU-T G.9807.1 clause C.11.2.1.
 * A Burst_Profile tests its destination with
 * pon_ploam_profile_is_broadcast().
 *
 * Return: true for PON_PLOAM_ONU_ID_BROADCAST.
 */
static inline bool pon_ploam_is_broadcast(u16 onu_id)
{
	return onu_id == PON_PLOAM_ONU_ID_BROADCAST;
}

/**
 * pon_ploam_profile_is_broadcast() - whether a Burst_Profile is broadcast
 * @onu_id: the ten bit ONU-ID of a downstream Burst_Profile
 *
 * A Burst_Profile goes to 0x3ff for every ONU or to 0x3fe for the ONUs that
 * support the 9.95328 Gbit/s upstream rate. An XGS-PON ONU takes both alike.
 * ITU-T G.9807.1 Table C.11.4 and Appendix II.
 *
 * Return: true for PON_PLOAM_ONU_ID_BROADCAST and
 * PON_PLOAM_ONU_ID_PROFILE_BCAST_10G.
 */
static inline bool pon_ploam_profile_is_broadcast(u16 onu_id)
{
	return pon_ploam_is_broadcast(onu_id) ||
	       onu_id == PON_PLOAM_ONU_ID_PROFILE_BCAST_10G;
}

/**
 * pon_ploam_is_assignable() - whether the OLT can assign an ONU-ID
 * @onu_id: the ten bit ONU-ID
 *
 * ITU-T G.9807.1 Table C.6.4.
 *
 * Return: true for 0 to PON_PLOAM_ONU_ID_MAX.
 */
static inline bool pon_ploam_is_assignable(u16 onu_id)
{
	return onu_id <= PON_PLOAM_ONU_ID_MAX;
}

#endif /* __NET_PON_PLOAM_MSG_H */
