.. SPDX-License-Identifier: GPL-2.0

===================================
Linux Motion Control Subsystem UAPI
===================================

The User-space API for the Linux Motion Control (LMC) subsystem consists of
a device node ioctl() interface and driver-specific sysfs attributes. The
devnodes use dynamically allocated MAJOR numbers and are named /dev/motionX
by default, where X is the ordinal of the device in probe order starting at
zero. The entries in /sys/class/motion/... can be used to further identify
each device.

If CONFIG_IIO is defined, then also a IIO trigger device is created, that
can be accessed via /sys/bus/iio/devices/iiotriggerX and the usual IIO API.

Motion Control Background
=========================

A motion controller device can control one or more actuators (motors), that
can move individually or synchronously. To accommodate for that, the API
has two distinct ways of specifying the actuator channel a specific function
applies to. Some structs take a single channel number as an unsigned integer,
while others apply to a set of channels represented in the form of a bitmask.
A "motion" in general can be any type of mechanical movement or displacement.
This can be carried out by any sort of motor or linear actuator. Types of
motors can be (but not limited to) brushed- or brushless DC motors, induction
motors, switched-reluctance motors, stepper motors, etc...
The mechanical movement can be coupled directly to a position sensor, such as
a linear encoder for example. Some actuators (such as stepper motors) can be
absolutely positioned without the need of a sensor.
A mechanical movement (motion) can be described by a (multi-dimensional-)
position and its two common time-derivatives, speed and acceleration.
Depending on the motion controller's capabilities, these can be specified in
more or less detail. For example a motor connected to a simple on-off switch
can only be conrolled at zero or maximum speed. If an encoder is present,
position control becomes possible. A stepper motor controller OTOH, can
often precisely control all three, position, speed and acceleration, and
sometimes even higher order time-derivatives, such as jerk.

.. _motion-profiles:

Motion profiles
===============

Some motion controllers have means of specifying a motion (or acceleration)
profile. This is generally represented by a curve of speed versus time. The
most common types of such a curve are triangular, trapezoidal, dual-slope and
S-curve (or an 8-point approximation thereof).
A triangular profile is in essence a special case of the trapezoidal profile,
where constant maximum speed is never reached. LMC supports the following 3
types of motion profiles:

1. Trapezoidal
--------------

A trapezoidal profile is comprised of 3 speed values: Start-, maximum- and
stop speed. 2 acceleration values determine the start and ending slopes of
the trapezoid. Start- and stop speed need not be zero, nor equal. Specific
constraints may depend on the underlying conroller, but often speed below
a certain value is hard to attain for example with a stepper motor controller
if the interval time between steps is limited. In addition to the 3 speed
and 2 acceleration values, a value for minimum constant speed time can also
be specified. This time value determines the minimum time the movement needs
to have a constant speed after acceleration, before deceleration begins.

2. Dual-slope
-------------

A dual-slope profile, is similar to a trapezoidal profile, but both the
acceleration and deceleration phase is split in two sections. The transition
point is determined by a 4th speed value which lies between max(Vs, Ve)
and Vmax, where Vs, Vmax and Ve are the trapezoidal profile values for
Start-, maximum- and stop speed respectively. Since there are two segments
to each acceleration phase, this profile has 4 acceleration values.

3. S-curve (8-point) profile
----------------------------

An 8-point profile (approximation of an S-curve) takes the dual-slope profile
one step further by adding a second intermediate speed value, which results
in acceleration and deceleration phases split into 3 segments each. This
profile type thus has 5 speed values and 6 acceleration values.

Units
=====

The LMC subsystem measures all time values in nanoseconds, in the format
mot_time_t (signed 64-bit integer). This is similar to the current
imoplementation of ktime_t, so conversion to/from mot_time_t is simple.
Internally ktime_t is used, but since ktime_t cannot be interfaced to
user-space directly, the mot_time_t type is defined for use in the user-
space API, but it currently is equivalent to ktime_t.

All position, speed and acceleration values are assumed in machine-units by
the kernel.
Machine-units are defined by the respective motion controller hardware.
For example for a stepper motor controller with 1/256 microstepping,
one unit of distance is equivalent to one microstep. Each driver can specify
scaling factors to convert units in user-space if needed. Since speed and
acceleration values have an implied time component, these conversion factors
also have an implied time conversion from machine-time units to nanoseconds.
These conversion factors are each specified by a rational number represented
by a numerator and a denominator. Both are unsigned 32bit integers.

Machine units for position, speed and acceleration are signed 32-bit integers,
but have a type definition as pos_raw_t, speed_raw_t and accel_raw_t
respectively.

Convention of signed position, speed and acceleration:
Movement of one channel is unidimensional, meaning position can be above or
below the origin (positive or negative respecively). Consequently, given
a positive position, a positive speed represents a movement further away
from the origin (position 0), while a negative speed value represents a
movement towards the origin. The opposite is valid when starting from a
negative position value.
Analogous to what speed does to position, is what acceletation does to speed:
Given positive speed, positive acceleration increments the speed, and given
"negative" speed, negative acceleration decrements the speed (increments its
absolute value).
For movement profiles, the convention is that profile (acceleration-, speed-)
values are strictly positive. The direction of movement is solely determined
by the relative position (i.e. "positive" or "negative" displacement).


IOCTL based interface description
=================================

API capabilities interrogation
------------------------------

``ioctl(fd, MOT_IOCTL_APIVER, null)``
 Function return value is the LMC API version number, or -1 on error.

``ioctl(fd, MOT_IOCTL_GET_CAPA, struct mot_capabilities *capa)``
 Returns a struct mot_capabilities filled in.

Basic motion control
--------------------

``ioctl(fd, MOT_IOCTL_GET_STATUS, struct mot_status *st)``
 Takes a struct mot_status with the channel parameter filled in and
 specifying the channel number for which to retrieve the status.
 Returns the current speed and position (if supported) in the same
 struct.

``ioctl(fd, MOT_IOCTL_BASIC_RUN, struct mot_speed_duration *s)``
 Takes a struct mot_speed_duration with the channel number, the speed and
 optionally either a non-zero value of duration or a non-zero value of
 distance. Note that one of these two values *must* be zero, otherwise
 an error is returned. If both are zero, then a movement is started
 with speed abs(speed) and direction the sign of speed for an indefinite
 amount of time. If duration is non-zero, then the movement is stopped
 at most after duration (in nanoseconds).

``ioctl(fd, MOT_IOCTL_BASIC_STOP, u32 chmsk)``
 Takes an unsigned 32-bit integer argument chmsk, which represents a bit-mask
 of channels. All current movements of all specified channels are stopped at
 once.

Feedback control
----------------

``ioctl(fd, MOT_IOCTL_CONFIG_INPUT, struct mot_input *inp)``
 Takes a struct mot_input with an index number, a function value and a channel
 bit-mask among other flags. This configures one of the internal or external
 feedback inputs of the motion controller for a certain function to act on a
 set of channels as specified by the chmask bit-mask. A common type of feedback
 input for example are end-stop switches on a 3D printer. If the function
 parameter is set to MOT_INP_FUNC_NONE (0) then the specified input is
 deconfigured.

Profile-based control
---------------------

``ioctl(fd, MOT_IOCTL_SET_PROFILE, struct mot_profile *p)``
 Adjust a motion profile for the specified slot index to the values supplied
 by this struct. (MOT_MAX_PROFILES-1) is the highest accepted value for index.
 The only values allowed for na (number of acceleration values) are 2, 4 or 6.
 The only values allowed for nv (number of speed values) are 3, 4 or 5. A
 specific driver can restrict these parameters even further. This is
 communicated in the struct mot_capabilities data.

``ioctl(fd, MOT_IOCTL_GET_PROFILE, struct mot_profile *p)``
 Takes the index from the provided struct mot_profile and returns a the same
 struct with the profile values filled in for that index. Returns an erro if
 the index does not point to a valid profile.

``ioctl(fd, MOT_IOCTL_START, struct mot_start *s)``
 Takes a struct mot_start, that programs a motion to start immediately or
 triggered by an event. The motion can be time-based if the duration parameter
 is non-zero - in this case also the direction parameter is taken into
 account - or distance-base. In the latter case the duration value must be
 zero and in the former case, the distance value must be zero. The parameter
 index specifies a motion profile to use for this movement.

``ioctl(fd, MOT_IOCTL_START, struct mot_start *s)``
 Takes a struct mot_stop. This struct contains a channel mask parameter chmask,
 which contains a bit-mask of all channels this command applis to. All motions
 of selected channels are stopped or prepared to be stopped by an event,
 following the deceleration path of the selected motion profile that started
 each respective motion or immediately, depending on the function of the
 trigger input that has been armed for each respective channel (if applicable).

Unimplemented future functionality
----------------------------------

``ioctl(fd, MOT_IOCTL_TORQUE_LIMITS, struct mot_torque *t)``
 Analogous to motion profiles, torque limit curves can augment a motion profile
 with a time-sectioned or position-sectioned profile of torque limit values.
 Background for torque limit curves: In some cases, it is desired to limit the
 torque (or force) of a movement depending on the position. For example when
 hitting an end-stop. In the case of a sliding door for example, one might
 require higher torque initially, while limiting torque during the reminder of
 the movement, in order to detect stalls due to a person standing in the way
 of the door without causing injuries. Torque control isn't always possible,
 and sometimes only in a limited fashion. For example, on AC motors, the
 controller could vary the gain of the V/f curve to reduce or increase the
 stall or slip limit. Stepper- or BLDC motor could vary the run current limit,
 or stall detect threshold.

``ioctl(fd, MOT_IOCTL_VECTOR_START, struct mot_vector *v)``
 Takes a struct mot_vector, that defines a multi-channel coordinated multi-
 dimensional linear movement along an n-dimensional space vector following a
 motion profile and optional torque curve. The chmask bit-mask specifies the
 channels that participate in this movement. The vector dist[] has the same
 size as the number of bits set in chmask and specifies the nth coordinate
 of the vector. This would be the prime application for controlling 3D
 printers, where tight coordination of several axes is required.

Constants used in the ioctl API
-------------------------------

The following constants are used in different field of the structs provided
to or returned by the ioctl API:

.. code-block:: C

   /* Trigger inputs and End Stop functions */
   enum {
           MOT_INP_FUNC_NONE = 0,
           MOT_INP_FUNC_STOP,
           MOT_INP_FUNC_STOP_POS,
           MOT_INP_FUNC_STOP_NEG,
           MOT_INP_FUNC_DECEL,
           MOT_INP_FUNC_DECEL_POS,
           MOT_INP_FUNC_DECEL_NEG,
           MOT_INP_FUNC_START,
           MOT_INP_FUNC_SIGNAL,
           MOT_INP_FUNC_LAST
   };

These constants are used in for the field function of struct mot_input.
This field specicies the function that a specified trigger input or end-stop
should have on the selected channels. FUNC_STOP means immediately set the
speed to zero, not following an acceleration curve, whereas FUNC_DECEL means
stopping by folowwing the deceleration slope(s) specified in the motion
profile that started the motion. _NEG and _POS restrict the action of the
trigger to act on backwards (_NEG, into negative distance) or forwards (_POS,
into positive distance) motion. If this is specified and the controller
supports this, this will avoid a situation in which an actuator that has
two end-stops for example at each extreme of the actuation range will stop
a movement to "the left" when the "right" end-stop is accidentally triggered.
MOT_INP_FUNC_SIGNAL will only generate an event to user-space or the IIO
trigger and not affect the motion directly.

.. code-block:: C

   /* Config trigger input edge */
   #define MOT_EDGE_RISING 0
   #define MOT_EDGE_FALLING 1

These constants are used for the edge parameter in ioctl structs. It specifies
the trigger input to be high-active (MOT_EDGE_RISING) or low-active
(MOT_EDGE_FALLING).

.. code-block:: C

   /* Start/Stop conditions */
   enum {
           MOT_WHEN_IMMEDIATE = 0,
           MOT_WHEN_INT_TRIGGER,
           MOT_WHEN_EXT_TRIGGER,
           MOT_WHEN_NEXT,
           MOT_WHEN_LAST
   };

Each parameter called "when" in the ioctl structs can potentially take one
of the MOT_WHEN_xxx values. INT_TRIGGER will execute the corresponding action
on an internal trigger signal, while EXT_TRIGGER will execute the
corresponding action on an external trigger signal (GPIO specified in fwnode).
MOT_WHEN_NEXT makes it possible to prepare a new motion that will start when
the currently active motion ends. By listening to poll() events of type
MOT_EVENT_TARGET and then sending the next motion start command with
MOT_WHEN_NEXT, it is possible to produce an uninterrupted stream of
consecutive movements. In that case MOT_EVENT_TARGET correspondes to the
end of the preceding motion that ended when the current "next" motion is
started, freeing the slot for the new "next" motion after that.

.. code-block:: C

   /* Event types */
   enum {
           MOT_EVENT_NONE = 0,
           MOT_EVENT_TARGET,
           MOT_EVENT_STOP,
           MOT_EVENT_INPUT,
           MOT_EVENT_STALL,
           MOT_EVENT_ERROR,
           MOT_EVENT_LAST
   };

These are constants for the event field of struct mot_event. See below for
a description of events ingeneral. The type of events reported to user space
are "target reached" (MOT_EVENT_TARGET), "stopped by internal trigger"
(MOT_EVENT_STOP), "external trigger" (MOT_EVENT_INPUT) or different fault
conditions (stall or generic error event). MOT_EVENT_STALL can be produced by
some motion controllers that can react to motor stalls in a natural way, for
example in the case of force-based obstacle- or end stop detection.

.. code-block:: C

   #define MOT_DIRECTION_LEFT 0
   #define MOT_DIRECTION_RIGHT 1

These constants are used for specifying direction of movement. LEFT in this
case means decreasing position value, while RIGHT is increasing position
value.

Structs used in the ioctl API
-----------------------------

.. code-block:: C

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
           MOT_TYPE_LAST
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
           __u32 subdiv;
           __u32 speed_conv_mul;
           __u32 speed_conv_div;
           __u32 accel_conv_mul;
           __u32 accel_conv_div;
           __u32 reserved2;
   };

The field ``features`` is a bit-mask of MOT_FEATURE_XX flags. The feature
SPEED means that the motion controller supports adjustable speed. All but the
most simple (on/off switch) controllers will have this bit set.
ACCEL means that the motion controller supports specifying acceleration
values. Not that this is not sufficient to indicate that motion profiles can
be used.
ENCODER meaans that the motion controller has a built-in or fixed connected
position encoder. If this bit is set, the position values returned by
MOT_IOCTL_GET_STATUS can be assumed to be accurate. I.e. slip and/or skipped
steps are properly taken into account.
PROFILE means that the motion controller supports setting motion profiles.
How many profiles, and how many speed and/or acceleration values are supported
is inticated by the fields ``max_profiles``, ``max_vpoints`` and
``max_apoints``.
The field ``type`` can take any of the values MOT_TYPE_XX and is only
informative. The fields ``num_channels``, ``num_int_triggers`` and
``num_ext_triggers`` specify the supported number of channels, internal
triggers and configured external triggers respectively.
The field ``subdiv`` can have a different meaning, depending on the type of
motion controller. For example for stepper motors, this typically indicates
the microstepping divider. If this number is not 1, this means that the value
of distance divided by ``subdiv`` will give the amount of machine-natural
mechanical units of distance (whole steps in case of a stepper motor).
The 4 different ``_conv_`` fields specify two rational conversion factors for
speed and acceleration respectively. All unit conversions of speed and
acceleration are done in user-space. The kernel only provides these numbers
to user-space as part of physical characteristics of the motion controller.
If the driver does not specify these values, or they lack a defined meaning,
all four of these fields will have a value of 1, so no zero-division can
happen and the conversion is just 1:1.

.. code-block:: C

   struct mot_speed_duration {
           __u32 channel;
           speed_raw_t speed;
           mot_time_t duration;
           pos_raw_t distance;
           __u32 reserved[3];
   };

This struct is used int the ioctl MOT_IOCTL_BASIC_RUN to start a new motion.
There are different ways of using BASIC_RUN:

 1. Specify only ``channel`` and ``speed``. All other fields are zero. This
    starts a new motion for indefinte amount of time.

 2. Additionally to 1. specify non-zero ``duration``. Starts a timed motion
    that will stop after the specified time.

 3. Additionally to 1. specify non-zero ``distance``. Starts a motion until
    the position specified by distance from starting point is reached.

Non zero values for both ``duration`` and ``distance`` will result in an
error (-EINVAL).

.. code-block:: C

   struct mot_status {
           __u32 channel;
           pos_raw_t position;
           speed_raw_t speed;
           __u32 local_inputs;
   };

This struct is used for the ioctl MOT_IOCTL_GET_STATUS. The caller needs to
set the field ``channel`` to the channel number it wants to request status
information from. After a successful call, the field ``speed`` will contain
the current speed of movement of the channel, and ``position`` will contain
the actual position if this is supported by the hardware. The field
``local_inputs`` contains a bit-mask of the state of all internal trigger
inputs.

.. code-block:: C

   struct mot_input {
           __u32 index;
           __u8 external;
           __u8 edge;
           __u8 reserved[2];
           __u32 function;
           channel_mask_t chmask;
   };

This struct is used for configuring internal or external trigger inputs. The
input ``index`` is specific to the flag ``external``. This means that internal
and external trigger inputs can have the same ``index`` value. The exact
``index`` value for internal inputs is determined by the driver and should be
enumerated from 0 onwards. For external GPIO intputs the index number is the
order in which they appear in the fwnode (DT property "motion,gpios").
``edge`` indicates the polarity of the input electrical signal,
MOT_EDGE_FALLING or MOT_EDGE_RISING. Please note, that any GPIO polarity is
also applied. So if a GPIO input is GPIO_ACTIVE_LOW and ``edge`` is
MOT_EDGE_FALLING, the resulting trigger will occur on the rising edge of the
electrical input signal.
``function`` takes any of the MOT_INP_FUNC_XX values mentioned above.
``chmask`` is a bit-mask of all motion channels this input will affect.

.. code-block:: C

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

Used to define a motion profile. See :ref:`motion-profiles` for a general
explanation of motion profiles. ``nv`` and ``na`` are the number of valid
entries in the ``velp[]`` and ``acc[]`` arrays respectively. ``tvmax`` is the
minimum amount of time the constant (maximum) speed needs to be maintained
after ending the acceleration phase, and before beginning the deceleration
phase. ``tvzero`` is the minimum amount of time the speed needs to be 0,
before a new motion is started in the same direction as the preceding motion.
In other words, analogous to ``tvmax``, a new acceleration with opposite sign
of the preceding deceleration. This time value is not applied if the next
motion is into the opposite direction, since there will be no change in the
sign of the resulting acceleration.

.. code-block:: C

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

Used for the MOT_IOCTL_START ioctl. The ``index`` parameter is the index
number of the motion profile that will be used for the motion. ``when``
determines when (under which condition) the motion is started, and takes
the value of any of the MOT_WHEN_XX constants. Like in the case of struct
mot_speed_duration, here also ``duration`` and ``distance`` are mutually
exclusive, meaning that one of both must always be zero.

.. code-block:: C

   struct mot_stop {
           channel_mask_t chmask;
           __u8 when;
           __u8 reserved[3];
   };

This struct is used to schedule a deceleration of a running motion of all
channels specified by ``chmask``, by following the deceleration part of their
respecive motion profiles. ``when`` takes any of the MOT_WHEN_XX constants and
determines the condition that triggers the deceleration.

Event handling with (e)poll/select
----------------------------------

When user-space opens the devnode for reading, (e)poll() can be used to wait
for motion events, using the (E)POLLIN flag. These events can then be read by
calling read() on the file-descriptor with a buffer size equal to the size of
struct mot_event. Make sure the file is opened in non-blocking mode for
for reliable event processing. read() will return -EAGAIN in this case.

.. code-block:: C

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

``event`` can take any of the MOT_EVENT_XX constants, and is used to determine
the type of event. The values of ``position``, ``speed`` and ``timestamp`` are
the corresponding motion position, speed and time at which the event ocurred.
Not all drivers support reporting a position value. If they don't that field
will always be zero. ``input_index`` is the index number of the external input
(in the case of a MOT_EVENT_INPUT event), or the index number of the internal
input (in the case of a MOT_EVENT_STOP event), that caused the event. For
other event types than MOT_EVENT_INPUT or MOT_EVENT_STOP, this field has no
meaning and will be zero.
