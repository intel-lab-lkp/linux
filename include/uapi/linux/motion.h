/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MOTION_H
#define _UAPI_LINUX_MOTION_H

#include <linux/const.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/* NOTE: Proposing to use IOC 'M' seq 0x80-0xc0 */

#define MOT_MAX_PROFILES 32
#define MOT_MAX_SPEEDPTS 5
#define MOT_MAX_ACCELPTS 6
#define MOT_MAX_INPUTS 32
#define MOT_UAPI_VERSION 1

/* Trigger inputs and End Stop functions */
enum {
	MOT_INP_FUNC_NONE = 0,	/* No-op, used to clear previous functions */
	MOT_INP_FUNC_STOP,	/* Stop immediately */
	MOT_INP_FUNC_STOP_POS,	/* Stop immediately if moving forward */
	MOT_INP_FUNC_STOP_NEG,	/* Stop immediately if moving backward */
	MOT_INP_FUNC_DECEL,	/* Stop by deceleration curve */
	MOT_INP_FUNC_DECEL_POS,
	MOT_INP_FUNC_DECEL_NEG,
	MOT_INP_FUNC_START,	/* Start motion with preset profile */
	MOT_INP_FUNC_SIGNAL,	/* Only produce a signal (EPOLLIN) */
	MOT_INP_FUNC_NUM_FUNCS
};

/* Config trigger input edge */
#define MOT_EDGE_RISING 0
#define MOT_EDGE_FALLING 1

/* Start/Stop conditions */
enum {
	MOT_WHEN_IMMEDIATE = 0,
	MOT_WHEN_INT_TRIGGER,	/* On internal trigger input */
	MOT_WHEN_EXT_TRIGGER,	/* On external trigger input */
	MOT_WHEN_NEXT,		/* After preceding (current) motion ends */
	MOT_WHEN_NUM_WHENS
};

/* Event types */
enum {
	MOT_EVENT_NONE = 0,
	MOT_EVENT_TARGET,	/* Target position reached */
	MOT_EVENT_STOP,		/* Endstop triggered */
	MOT_EVENT_INPUT,	/* (Virtual-) input event */
	MOT_EVENT_STALL,	/* Motor stalled */
	MOT_EVENT_ERROR,	/* Other motor drive error */
	MOT_EVENT_NUM_EVENTS
};

#define MOT_DIRECTION_LEFT 0
#define MOT_DIRECTION_RIGHT 1

/* Convention of signed position, speed and acceleration:
 * movement of one channel is unidimensional, meaning position can be above or
 * below the origin (positive or negative respecively). Consequently, given
 * a positive position, a positive speed represents a movement further away
 * from the origin (position 0), while a negative speed value represents a
 * movement towards the origin. The opposite is valid when starting from a
 * negative position value.
 * Analogous to what speed does to position, is what acceletation does to speed:
 * Given positive speed, positive acceleration increments the speed, and given
 * "negative" speed, negative acceleration decrements the speed (increments its
 * absolute value).
 * For movement profiles, the convention is that profile (acceleration-, speed-)
 * values are strictly positive. The direction of movement is solely determined
 * by the relative position (i.e. "positive" or "negative" displacement).
 */
typedef __u32 channel_mask_t;
typedef __s32 pos_raw_t;
typedef __s32 speed_raw_t;
typedef __s32 accel_raw_t;
typedef __u32 torque_raw_t;
typedef __s64 mot_time_t; /* Try to mimic ktime_t, unit is nanoseconds. */

#define MOT_FEATURE_SPEED	BIT(0)
#define MOT_FEATURE_ACCEL	BIT(1)
#define MOT_FEATURE_ENCODER	BIT(2)
#define MOT_FEATURE_PROFILE	BIT(3)
#define MOT_FEATURE_VECTOR	BIT(4)

enum motion_device_type {
	MOT_TYPE_DC_MOTOR,
	MOT_TYPE_AC_MOTOR,
	MOT_TYPE_STEPPER,
	MOT_TYPE_BLDC,
	MOT_TYPE_SRM,
	MOT_TYPE_LINEAR,
	MOT_TYPE_NUM_TYPES
};

struct mot_capabilities {
	__u32 features;
	__u8 type;
	__u8 num_channels;
	__u8 num_int_triggers;
	__u8 num_ext_triggers;
	__u8 max_profiles;
	__u8 max_vpoints;
	__u8 max_apoints;
	__u8 reserved1;
	__u32 subdiv; /* Position unit sub-divisions, microsteps, etc... */
	/*
	 * Coefficients for converting to/from controller time <--> seconds.
	 * Speed[1/s] = Speed[controller_units] * conv_mul / conv_div
	 * Accel[1/s^2] = Accel[controller_units] * conv_mul / conv_div
	 */
	__u32 speed_conv_mul;
	__u32 speed_conv_div;
	__u32 accel_conv_mul;
	__u32 accel_conv_div;
	__u32 reserved2;
};

struct mot_speed_duration {
	__u32 channel;
	speed_raw_t speed;
	mot_time_t duration;
	pos_raw_t distance;
	__u32 reserved[3];
};

struct mot_status {
	__u32 channel;
	pos_raw_t position;
	speed_raw_t speed;
	__u32 local_inputs;
};

struct mot_input {
	__u32 index;
	__u8 external;
	__u8 edge;
	__u8 reserved[2];
	__u32 function;
	channel_mask_t chmask;
};

/**
 * struct mot_profile - Describe an acceleration profile
 * @index: The index into the table of profiles to change
 * @tvmax: Minimum time to stay at maximum velocity
 * @tvzero: Minimum time to stay at zero velocity
 * @na: Number of acceleration values
 * @nv: Number of velocity values
 * @acc: List of acceleration values. All values are absolute machine units.
 * @vel: List of velocity values. All values are absolure machine units.
 *
 * 3 different types of acceleration curves are supported:
 * 1. Trapezoidal - comprised of 3 velocity values and 2 acceleration values.
 *    Motion starts at start velocity (vel[0]) and accelerates with acc[0]
 *    linearly up to maximum velocity vel[1]. Maximum velocity is maintained
 *    for at least tvmax, before decelerating with acc[1] down to stop
 *    velocity vel[2]. After that velocity drops to zero and stays there for
 *    at least tvzero.
 *
 * 2. Dual slope - comprised of 4 velocity values and 4 acceleration values.
 *    Similar to trapezoidal profile above, but adding an intermediate
 *    velocity vel[1]. acc[0] is the first acceleration slope between
 *    vel[0] and vel[1]. acc[1] is the second acceleration slope between
 *    vel[1] and vel[2] (maximum velocity). acc[2] is the first deceleration
 *    slope between vel[2] and vel[1], and acc[3] is the final deceleration
 *    slope between vel[1] and vel[3].
 *
 * 3. S-curve profile - Most advanced profile, often also called 8-point
 *    profile, comprised of 5 velocity values and 6 acceleration values.
 */
struct mot_profile {
	__u32 index;
	mot_time_t tvmax;
	mot_time_t tvzero;
	__u8 na;
	__u8 nv;
	__u8 reserved[2];
	accel_raw_t acc[MOT_MAX_ACCELPTS];
	speed_raw_t vel[MOT_MAX_SPEEDPTS];
};

struct mot_start {
	__u32 channel;
	__u8 direction;
	__u8 index;
	__u8 when;
	__u8 reserved1;
	mot_time_t duration;
	pos_raw_t distance;
	__u32 reserved2;
};

struct mot_stop {
	channel_mask_t chmask;
	__u8 when;
	__u8 reserved[3];
};

struct mot_event {
	__u32 channel;
	__u8 event;
	__u8 reserved1[3];
	pos_raw_t position;
	speed_raw_t speed;
	mot_time_t timestamp;
	__u32 input_index;
	__u32 reserved2;
};

/* API capabilities interrogation ioctls */
#define MOT_IOCTL_APIVER		_IO('M', 0x80)
#define MOT_IOCTL_GET_CAPA		_IOR('M', 0x81, struct mot_capabilities)

/* Basic motion control */
#define MOT_IOCTL_GET_STATUS		_IOWR('M', 0x82, struct mot_status)
#define MOT_IOCTL_BASIC_RUN		_IOW('M', 0x83, struct mot_speed_duration)
#define MOT_IOCTL_BASIC_STOP		_IOW('M', 0x84, __u32)

/* Feedback control */
#define MOT_IOCTL_CONFIG_INPUT		_IOW('W', 0x85, struct mot_input)

/* Profile control */
#define MOT_IOCTL_SET_PROFILE		_IOW('M', 0x86, struct mot_profile)
#define MOT_IOCTL_GET_PROFILE		_IOWR('M', 0x87, struct mot_profile)
#define MOT_IOCTL_START			_IOW('M', 0x88, struct mot_start)
#define MOT_IOCTL_STOP			_IOW('M', 0x89, struct mot_stop)

#endif /* _UAPI_LINUX_MOTION_H */
