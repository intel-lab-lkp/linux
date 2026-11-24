.. SPDX-License-Identifier: GPL-2.0

===========================================
Arm Mali-C720 Image Signal Processor driver
===========================================

Introduction
============

This file documents the driver for Arm's Mali-C720AE Image Signal Processor. The
driver is located under drivers/media/platform/arm/mali-c720.

The Mali-C720 ISP receives raw Bayer data from sensors through an input
frontend. The driver uses the first of the hardware's four parallel input
interfaces.
The ISP features an image-processing pipeline targeting human vision (HV)
applications.

Output images are written to memory through an internal DMA engine. The
full-resolution (FR0) output pipeline supports cropping and scaling, with
simultaneous RGB and YUV outputs.

Two independent test pattern generators feed the input frontend, allowing the
ISP to produce images without a connected camera sensor. The driver module is
named ``mali-c720`` and is enabled through ``CONFIG_VIDEO_MALI_C720``.

The driver implements the V4L2, Media Controller and V4L2 subdevice interfaces.
Camera sensors and receivers connected to the ISP are expected to expose V4L2
subdevices.

Mali-C720 ISP hardware
======================

The ISP exposes a set of hardware ID registers. The driver matches these
against a set of supported hardware versions and rejects unsupported versions.
Different hardware versions may expose different functionality, including
available image formats, pipeline topology, and supported statistics and
parameter modules.

The ISP is also configurable at synthesis time, allowing certain pipelines and
processing modules to be included or excluded. The ISP exposes a discovery
table describing the resulting hardware configuration. The driver queries this
table during initialization to register the appropriate subdevices and
programmable blocks.

A high-level functional view of the Mali-C720 ISP is shown below. Live inputs
and the test pattern generators feed the input frontend::

  +---------------------+    +---------------+
  |  4 parallel inputs  |    | TPG 0 / TPG 1 |
  +---------------------+    +---------------+
             |                       |
             +------------+----------+
                          |
            +-------------v---------------+
            |       Input frontend        |
            +-----------------------------+
                          |
                          |
            +-------------v---------------+
            |       Context manager       |
            +-----------------------------+
                          |
                          |
            +-------------v---------------+
            |      Input processing       |
            +-----------------------------+
                          |
                          |
                 +--------v--------+
                 |  HV processing  |
                 +-----------------+
                          |
                          | RGB
                          |
            +-------------v---------------+
            |             FR0             |
            |        Scale / crop         |
            +----+-------------------+----+
                 |                   |
                 | RGB               | YUV
                 |                   |
                 v                   v
              Memory              Memory

The FR0 pipeline has simultaneous RGB/YUV outputs. All image outputs shown
as Memory use the internal output DMA engine.

Configuration DMA engine (CDMA)
===============================

The ISP hardware includes a configuration DMA (CDMA) engine that loads ISP
configuration from allocated memory into registers and stores computed fields,
including statistics, back into that memory. Configuration is loaded at the
beginning of frame processing. Computed fields are stored as they become
available, typically around frame end.

The CDMA engine also includes a scatter-gather unit that transfers ISP
registers and internal memory tables to and from selected memory offsets,
with configurable transfer sizes.

For each context, the driver allocates a contiguous DMA region for the ISP
configuration and initializes it for a default use case. At stream start, it
configures additional fields such as the frame geometry and colour filter
array (CFA) pattern.

The driver uses the scatter-gather unit to store ISP statistics blocks in the
queued videobuf2 (VB2) buffer. The UAPI structures for these blocks reflect the
data packing produced by the CDMA engine.

Media Controller Topology
=========================

An example of the ISP's topology (as implemented in a system with an IMX415
camera sensor and generic CSI-2 receiver) is below.

.. kernel-figure:: mali-c720-graph.dot
    :alt: Mali-C720 media controller topology
    :align: center

The driver has up to six V4L2 subdevices:

- ``mali-c720 tpg N``: Test pattern generator (0-1).
- ``mali-c720 input``: Sensor input frontend.
- ``mali-c720 isp``: ISP input pipeline.
- ``mali-c720 hv pipe``: Human vision pipeline.
- ``mali-c720 fr 0 pipe``: Full-resolution crop/scale pipeline.

The driver has four V4L2 video devices:

- ``mali-c720 fr0-rgb`` and ``mali-c720 fr0-yuv``: FR capture devices.
- ``mali-c720 3a stats``: 3A statistics capture device.
- ``mali-c720 3a params``: 3A parameters output device.

Input formats
-------------

The ISP input is limited to resolutions between 256x256 and 8192x4608, and
this is reflected in the ISP subdevice's .set_fmt() operations.
With further crop and scale configurations, the minimum output from the ISP
can be 64x64, reflected in the full-resolution output pipeline subdevice.

Using the driver to capture video
=================================

Using the media controller APIs we can configure the input source and ISP to
capture images in a variety of formats. In the examples below, configuring the
media graph is done with the v4l-utils [1]_ package's media-ctl utility.
Capturing the images is done with yavta [2]_.

Before configuring the graph, stop any capture applications and disable the
existing links. The driver enables an image output link by default. Resetting
the links allows us to enable only the devices required for capture:

.. code-block:: none

    media-ctl -r

Configuring the input source
----------------------------

To use TPG 0, enable its link to input port 0 and connect the slot input to the
ISP.

The first step is to set the input source that we wish to use by enabling the
correct media link. Using the example topology above, we can select the
TPG as follows:

.. code-block:: none

    media-ctl -l "'csi2-rx':1->'mali-c720 input':1[0]"
    media-ctl -l "'mali-c720 tpg 0':0->'mali-c720 input':1[1]"
    media-ctl -l "'mali-c720 input':0->'mali-c720 isp':0[1]"

This links TPG-0 to the input port, and links the slot input to the ISP sink
pad.

Test pattern mode selection
---------------------------

The test pattern generator offers various patterns:
 0 (flat field), 1 (horizontal gradient), 2 (vertical gradient),
 3 (vertical bars, the default), 4 (arbitrary rectangle),
 5 (random data), and 6 (white frame on black field).

We can select the vertical bars test pattern as follows:

.. code-block:: none

    v4l2-ctl -d /dev/v4l-subdev0 --set-ctrl=test_pattern=3

Configuring the image processing pipeline
-----------------------------------------

The example below shows how we can route the image data through the human
vision pipeline of the ISP, and from there to the first full resolution
output pipe:

.. code-block:: none

    media-ctl -l "'mali-c720 isp':1->'mali-c720 hv pipe':0[1]"
    media-ctl -l "'mali-c720 hv pipe':1->'mali-c720 fr 0 pipe':0[1]"

Configuring which video devices will stream data
------------------------------------------------

The driver waits for all video devices connected through enabled links to have
their VIDIOC_STREAMON ioctl called before starting the input source. Enable
links only to the video devices that will be used. In the example below we
enable the link to the `fr0-rgb` video device.

.. code-block:: none

    media-ctl -l "'mali-c720 fr 0 pipe':1->'mali-c720 fr0-rgb':0[1]"

Capturing Bayer data and processing it to RGB
--------------------------------------------

To capture 1920x1080 Bayer data from the source and push it through the ISP's
full processing pipeline, we configure formats from the input source towards
the output.

The TPG produces 16-bit Bayer data, the input subdevice outputs this as 24-bit
Bayer. The input pipeline, represented by the ISP subdevice, transforms the
data into the square-root domain and feeds 16-bit Bayer into HV. Within HV,
the debayering occurs and then produces the 12-bit RGB input used by FR0.

For our example, the formats are configured as follows:

.. code-block:: none

    media-ctl -V "'mali-c720 tpg 0':0[fmt:SRGGB16_1X16/1920x1080]"
    media-ctl -V "'mali-c720 input':1[fmt:SRGGB16_1X16/1920x1080]"
    media-ctl -V "'mali-c720 isp':0[fmt:SRGGB24_1X24/1920x1080]"
    media-ctl -V "'mali-c720 hv pipe':0[fmt:SRGGB16_1X16/1920x1080]"
    media-ctl -V "'mali-c720 fr 0 pipe':0[fmt:RGB121212_1X36/1920x1080]"

Images can then be captured from the FR0-RGB video device:

.. code-block:: none

    yavta -f RGB24 -s 1920x1080 -c10 /dev/video0

Capturing from multiple outputs
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The RGB and YUV outputs can capture images at the same time. For example,
enable both FR0 capture nodes:

.. code-block:: none

    media-ctl -l "'mali-c720 fr 0 pipe':1->'mali-c720 fr0-rgb':0[1]"
    media-ctl -l "'mali-c720 fr 0 pipe':2->'mali-c720 fr0-yuv':0[1]"

Starting the first video device validates the entire media pipeline and
prevents further format changes. As such, when capturing from multiple outputs,
formats must be configured on all enabled subdevices and linked video devices
before starting any streams. For example, set both capture formats using v4l2-ctl:

.. code-block:: none

    v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=RGB3
    v4l2-ctl -d /dev/video1 --set-fmt-video=width=1920,height=1080,pixelformat=NV12

Then run the two capture processes concurrently:

.. code-block:: none

    yavta -c10 /dev/video0 &
    yavta -c10 /dev/video1 &
    wait

Cropping the image
~~~~~~~~~~~~~~~~~~

The full-resolution pipeline can crop to a minimum resolution of 64x64.
To crop the image, configure the pipe's source pad crop rectangle and set
the format on the video device. The FR pipeline has one shared crop for
its RGB and YUV outputs. Setting the crop on either source pad updates
both source crops and output dimensions:

.. code-block:: none

    media-ctl -V "'mali-c720 fr 0 pipe':1[crop:(640,300)/640x480]"
    yavta -f RGB24 -s 640x480 -c10 /dev/video0

Downscaling the image
~~~~~~~~~~~~~~~~~~~~~

The full-resolution pipeline can produce full-size, cropped or downscaled
images. Downscaling by up to ~16x is supported, provided the minimum 64x64
output resolution is adhered to. For the best image result the scaling ratio
for each direction should be the same. Scaling is configured with the compose
rectangle on the pipe's sink pad. RGB and YUV outputs share the same
scaling configuration:

.. code-block:: none

    media-ctl -V "'mali-c720 fr 0 pipe':0[compose:(0,0)/640x360]"
    yavta -f RGB24 -s 640x360 -c10 /dev/video0

Cropping is applied after scaling. To combine the two operations, set the
compose rectangle first, then the crop rectangle.

Capturing images in YUV formats
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Within the full resolution pipe there is a colour space conversion block for
the YUV conversion, but its design permits the RGB and YUV to be output
simultaneously. Enable the link between the pipe and the YUV video device,
then configure a capture format like NV12:

.. code-block:: none

    media-ctl -l "'mali-c720 fr 0 pipe':1->'mali-c720 fr0-rgb':0[0]"
    media-ctl -l "'mali-c720 fr 0 pipe':2->'mali-c720 fr0-yuv':0[1]"
    yavta -f NV12 -s 1920x1080 -c10 /dev/video1

.. _mali-c720-3a-stats:

Capturing ISP Statistics
========================

The ISP is capable of producing statistics for consumption by image processing
algorithms running in userspace. These statistics can be captured by queueing
buffers to the `mali-c720 3a stats` V4L2 device whilst the ISP is streaming.

Each successfully captured statistics buffer carries the processed frame's
sequence number in its :c:type:`v4l2_buffer` ``sequence`` field. This matches
the sequence number of image buffers produced from the same frame.

Only the :ref:`V4L2_META_FMT_MALI_C720_STATS <v4l2-meta-fmt-mali-c720-stats>`
format is supported, so no format-setting need be done:

.. code-block:: none

    # We assume the media graph has been configured to support RGB24 capture
    # from the mali-c720 fr0-rgb V4L2 Device, which is at /dev/video0. The
    # statistics V4L2 device is at /dev/video2. Run the capture processes
    # concurrently so that both video devices start streaming:

    media-ctl -l "'mali-c720 isp':2->'mali-c720 3a stats':0[1]"
    yavta -f RGB24 -s 1920x1080 -c10 /dev/video0
    yavta -c10 -F /dev/video2

As our hardware is configurable, the modules present can vary depending upon
its configuration. This means that certain statistics may only be present if
their respective pipelines are present. We expose a V4L2 control
:ref:`V4L2_CID_MALI_C720_STATS_CAPABILITIES <mali-c720-controls>` which allows
the userspace to query the statistics produced by the ISP.

.. _mali-c720-3a-params:

Programming ISP Parameters
==========================

The ISP can be programmed with various parameters from userspace to apply to
the hardware before and during video streaming. This allows userspace to
dynamically change values such as black level, white balance, digital gain
and so on.

Enable the parameters link before starting capture:

.. code-block:: none

    media-ctl -l "'mali-c720 3a params':0->'mali-c720 isp':3[1]"

The buffer format and how to populate it are described by the
:ref:`V4L2_META_FMT_MALI_C720_PARAMS <v4l2-meta-fmt-mali-c720-params>` format,
which should be set as the data format for the `mali-c720 3a params` video node.

Similar to statistics, our configurable hardware means that the modules present
in the ISP can vary depending on its configuration. As such, we expose a V4L2
control :ref:`V4L2_CID_MALI_C720_PARAM_CAPABILITIES <mali-c720-controls>`
which allows the userspace to query the supported parameter blocks. The
parameter buffer will be invalidated if any provided blocks are not present.

.. _mali-c720-frame-scheduling:

Frame scheduling and Media Requests
===================================

Frame scheduling depends on whether the streaming videobuf2 (VB2) queues use
direct buffer queuing or Media Requests. Use of Media Requests is optional.

Direct mode
-----------

When all streaming parameter, statistics and image capture queues use direct
buffer queuing through :ref:`VIDIOC_QBUF`, the driver schedules frames in
free-running mode. Scheduling starts only when at least one image or
statistics output buffer is ready. Once started, it continues scheduling
frames using the available buffers and the latest applied parameters.
Empty statistics or image capture queues disable their respective capture
for the scheduled frame, and an empty parameters queue leaves the ISP
configuration unchanged.

Request mode
------------

If any of these streaming VB2 queues uses Media Requests, scheduling starts
when a request is queued. Each subsequent frame also requires a queued
request.

Applications allocate a request using :ref:`MEDIA_IOC_REQUEST_ALLOC` on the
media device.
Parameter, statistics and image buffers are associated with it through
:ref:`VIDIOC_QBUF`, setting ``V4L2_BUF_FLAG_REQUEST_FD`` and ``request_fd``.
The request is then submitted using :ref:`MEDIA_REQUEST_IOC_QUEUE` on the
request file descriptor.

Controls cannot currently be associated with Media Requests. Requests
containing controls are rejected when queued.

A request does not need to include a buffer for every capture queue.
Queues without a buffer will be disabled for the scheduled frame.

All buffers associated with the request are usually applied on the same
frame, but a request could span multiple frames if multiple buffers are
queued for the same buffer queue. The parameters are applied before
processing, and the statistics and image buffers are filled with the results
from that frame. Completed buffers are returned to userspace through
:ref:`VIDIOC_DQBUF <VIDIOC_QBUF>`.

It is permitted to queue buffers directly in other VB2 queues alongside
those operating in request mode, with requests still driving scheduling.
Whenever a request is scheduled, available buffers from these queues can
be used for the same frame, alongside buffers associated with the request.

References
==========

.. [1] https://git.linuxtv.org/v4l-utils.git/
.. [2] https://git.ideasonboard.org/yavta.git/
.. [3] https://support.arm.com/compute-ip/mali-c720ae
