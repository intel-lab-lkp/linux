.. SPDX-License-Identifier: GPL-2.0

.. _media_using_camera_sensor_drivers:

Using camera sensor drivers
===========================

This section describes common practices for how the V4L2 sub-device interface is
used to control the camera sensor drivers.

You may also find :ref:`media_writing_camera_sensor_drivers` useful.

Sensor internal pipeline configuration
--------------------------------------

Camera sensors have an internal processing pipeline including cropping and
binning functionality. The sensor drivers belong to two distinct classes, freely
configurable and register list-based drivers, depending on how the driver
configures this functionality.

Freely configurable camera sensor drivers
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Freely configurable camera sensor drivers expose the device's internal
processing pipeline as one or more sub-devices with different cropping and
scaling configurations. The output size of the device is the result of a series
of cropping and scaling operations from the device's pixel array's size.

An example of such a driver is the CCS driver.

Register list-based drivers
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Register list-based drivers generally, instead of able to configure the device
they control based on user requests, are limited to a number of preset
configurations that combine a number of different parameters that on hardware
level are independent. How a driver picks such configuration is based on the
format set on a source pad at the end of the device's internal pipeline.

Most sensor drivers are implemented this way.

Frame interval configuration
----------------------------

There are two different methods for obtaining possibilities for different frame
intervals as well as configuring the frame interval. Which one to implement
depends on the type of the device.

Raw camera sensors
~~~~~~~~~~~~~~~~~~

Instead of a high level parameter such as frame interval, the frame interval on
a raw camera sensor is determined by a number of sensor-specific parameters.
These parameters tend to be common across most modern raw camera sensors.

The pixel array is the full grid of photosensitive elements on the camera
sensor. A subregion of it is selected by the analogue crop. The cropped image
may then be subject to binning (averaging of an NxN block) and subsampling
which further reduce the image dimensions. The resulting image is then read out
by the ADC (analogue-to-digital converter) line by line. After ADC readout,
optional digital crop or scaling may further reduce the image dimensions, see
:ref:`VIDIOC_SUBDEV_G_SELECTION <VIDIOC_SUBDEV_G_SELECTION>`.

The sensor scales the input clock to a (usually) fixed pixel readout rate. Thus
the frame interval is determined by two fundamental timing registers: **line
length in pixels (LLP)** and **frame length in lines (FLL)**.

LLP is the total number of pixels per line, including both the active readout
width and horizontal blanking. FLL is the total number of lines per frame,
including both the active readout height and vertical blanking. These two
parameters allow controlling the total time taken to read out an image by
adjusting the blanking intervals.

These registers may go by other names for different sensors, like HMAX and
VMAX, or HTOTAL and VTOTAL, or similar. They are exposed to userspace
applications through the horizontal and vertical blanking controls.

For some sensors the value for the horizontal timing (LLP/HMAX/HTOTAL) may be
in cycles of an internal clock, where multiple pixels are read out per cycle.
In such cases the driver should handle this scaling internally, ensuring the
userspace control for the horizontal blanking is always in units of pixels.

So the frame interval can be calculated as::

        frame interval = (LLP * FLL) / pixel rate

See the note below on how applications can map this to the blanking and pixel
rate controls:

.. note::

	Horizontal and vertical blanking are specified by :ref:`V4L2_CID_HBLANK
	<v4l2-cid-hblank>` and :ref:`V4L2_CID_VBLANK <v4l2-cid-vblank>`,
	respectively. The unit of ``HBLANK`` is pixels, and the unit of
	``VBLANK`` is lines.

	The pixel rate in the sensor's pixel array is specified by
	:ref:`V4L2_CID_PIXEL_RATE <v4l2-cid-pixel-rate>`, in units of pixels
	per second.

	Sensor drivers are required to implement sub-device nodes that expose
	these controls. They can be read-only or configurable depending on the
	device.

	For most raw sensors the blanking is defined relative to the size of
	the image being sent out to the host over the bus (source pad format)::

                LLP = output width + horizontal blanking
                FLL = output height + vertical blanking

        For CCS-compliant raw sensors (that use the CCS driver), the blanking
        controls are defined relative to the analogue crop rectangle::

                LLP = analogue crop width + horizontal blanking
                FLL = analogue crop height + vertical blanking

Some sensors may lower the minimum allowed values for LLP and/or FLL when
binning or subsampling, thus increasing the effective frame rate.

The driver shall update the minimum and maximum values of the blanking controls
so that the resulting LLP and FLL registers are programmed within the range
permitted by the sensor hardware for the current mode.

USB cameras etc. devices
~~~~~~~~~~~~~~~~~~~~~~~~

USB video class hardware, as well as many cameras offering a similar higher
level interface natively, generally use the concept of frame interval (or frame
rate) on device level in firmware or hardware. This means lower level controls
implemented by raw cameras may not be used on uAPI (or even kAPI) to control the
frame interval on these devices.

Rotation, orientation and flipping
----------------------------------

Some systems have the camera sensor mounted upside down compared to its natural
mounting rotation. In such cases, drivers shall expose the information to
userspace with the :ref:`V4L2_CID_CAMERA_SENSOR_ROTATION
<v4l2-camera-sensor-rotation>` control.

Sensor drivers shall also report the sensor's mounting orientation with the
:ref:`V4L2_CID_CAMERA_SENSOR_ORIENTATION <v4l2-camera-sensor-orientation>`.

Sensor drivers that have any vertical or horizontal flips embedded in the
register programming sequences shall initialize the :ref:`V4L2_CID_HFLIP
<v4l2-cid-hflip>` and :ref:`V4L2_CID_VFLIP <v4l2-cid-vflip>` controls with the
values programmed by the register sequences. The default values of these
controls shall be 0 (disabled). Especially these controls shall not be inverted,
independently of the sensor's mounting rotation.
