.. SPDX-License-Identifier: GPL-2.0

Media Controller devices
------------------------

Media Controller
~~~~~~~~~~~~~~~~

The media controller userspace API is documented in
:ref:`the Media Controller uAPI book <media_controller>`. This document focus
on the kernel-side implementation of the media framework.

Abstract media device model
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Discovering a device internal topology, and configuring it at runtime, is one
of the goals of the media framework. To achieve this, hardware devices are
modelled as an oriented graph of building blocks called entities connected
through pads.

An entity is a basic media hardware building block. It can correspond to
a large variety of logical blocks such as physical hardware devices
(CMOS sensor for instance), logical hardware devices (a building block
in a System-on-Chip image processing pipeline), DMA channels or physical
connectors.

A pad is a connection endpoint through which an entity can interact with
other entities. Data (not restricted to video) produced by an entity
flows from the entity's output to one or more entity inputs. Pads should
not be confused with physical pins at chip boundaries.

A link is a point-to-point oriented connection between two pads, either
on the same entity or on different entities. Data flows from a source
pad to a sink pad.

Media device
^^^^^^^^^^^^

A media device is represented by a struct media_device
instance, defined in ``include/media/media-device.h``.
Allocation of the structure is handled by the media device driver, usually by
embedding the :c:type:`media_device` instance in a larger driver-specific
structure.

Drivers initialise media device instances by calling
:c:func:`media_device_init()`. After initialising a media device instance, it is
registered by calling :c:func:`__media_device_register()` via the macro
``media_device_register()`` and unregistered by calling
:c:func:`media_device_unregister()`. An initialised media device must be
eventually cleaned up by calling :c:func:`media_device_cleanup()`.

Note that it is not allowed to unregister a media device instance that was not
previously registered, or clean up a media device instance that was not
previously initialised.

Entities
^^^^^^^^

Entities are represented by a struct media_entity
instance, defined in ``include/media/media-entity.h``. The structure is usually
embedded into a higher-level structure, such as
:c:type:`v4l2_subdev` or :c:type:`video_device`
instances, although drivers can allocate entities directly.

Drivers initialize entity pads by calling
:c:func:`media_entity_pads_init()`.

Drivers register entities with a media device by calling
:c:func:`media_device_register_entity()`
and unregistered by calling
:c:func:`media_device_unregister_entity()`.

Interfaces
^^^^^^^^^^

Interfaces are represented by a
struct media_interface instance, defined in
``include/media/media-entity.h``. Currently, only one type of interface is
defined: a device node. Such interfaces are represented by a
struct media_intf_devnode.

Drivers initialize and create device node interfaces by calling
:c:func:`media_devnode_create()`
and remove them by calling:
:c:func:`media_devnode_remove()`.

Pads
^^^^
Pads are represented by a struct media_pad instance,
defined in ``include/media/media-entity.h``. Each entity stores its pads in
a pads array managed by the entity driver. Drivers usually embed the array in
a driver-specific structure.

Pads are identified by their entity and their 0-based index in the pads
array.

Both information are stored in the struct media_pad,
making the struct media_pad pointer the canonical way
to store and pass link references.

Pads have flags that describe the pad capabilities and state.

``MEDIA_PAD_FL_SINK`` indicates that the pad supports sinking data.
``MEDIA_PAD_FL_SOURCE`` indicates that the pad supports sourcing data.

.. note::

  One and only one of ``MEDIA_PAD_FL_SINK`` or ``MEDIA_PAD_FL_SOURCE`` must
  be set for each pad.

Links
^^^^^

Links are represented by a struct media_link instance,
defined in ``include/media/media-entity.h``. There are two types of links:

**1. pad to pad links**:

Associate two entities via their PADs. Each entity has a list that points
to all links originating at or targeting any of its pads.
A given link is thus stored twice, once in the source entity and once in
the target entity.

Drivers create pad to pad links by calling:
:c:func:`media_create_pad_link()` and remove with
:c:func:`media_entity_remove_links()`.

**2. interface to entity links**:

Associate one interface to a Link.

Drivers create interface to entity links by calling:
:c:func:`media_create_intf_link()` and remove with
:c:func:`media_remove_intf_links()`.

.. note::

   Links can only be created after having both ends already created.

Links have flags that describe the link capabilities and state. The
valid values are described at :c:func:`media_create_pad_link()` and
:c:func:`media_create_intf_link()`.

Graph traversal
^^^^^^^^^^^^^^^

The media framework provides APIs to traverse media graphs, locating connected
entities and links.

To iterate over all entities belonging to a media device, drivers can use
the media_device_for_each_entity macro, defined in
``include/media/media-device.h``.

..  code-block:: c

    struct media_entity *entity;

    media_device_for_each_entity(entity, mdev) {
    // entity will point to each entity in turn
    ...
    }

Helper functions can be used to find a link between two given pads, or a pad
connected to another pad through an enabled link
(:c:func:`media_entity_find_link()`, :c:func:`media_pad_remote_pad_first()`,
:c:func:`media_entity_remote_source_pad_unique()` and
:c:func:`media_pad_remote_pad_unique()`).

Use count and power handling
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Due to the wide differences between drivers regarding power management
needs, the media controller does not implement power management. However,
the struct media_entity includes a ``use_count``
field that media drivers
can use to track the number of users of every entity for power management
needs.

The :c:type:`media_entity<media_entity>`.\ ``use_count`` field is owned by
media drivers and must not be
touched by entity drivers. Access to the field must be protected by the
:c:type:`media_device`.\ ``graph_mutex`` lock.

Links setup
^^^^^^^^^^^

Link properties can be modified at runtime by calling
:c:func:`media_entity_setup_link()`.

Pipelines and media streams
^^^^^^^^^^^^^^^^^^^^^^^^^^^

A media stream is a stream of pixels or metadata originating from one or more
source devices (such as a sensors) and flowing through media entity pads
towards the final sinks. The stream can be modified on the route by the
devices (e.g. scaling or pixel format conversions), or it can be split into
multiple branches, or multiple branches can be merged.

A media pipeline is a set of media streams which are interdependent. This
interdependency can be caused by the hardware (e.g. configuration of a second
stream cannot be changed if the first stream has been enabled) or by the driver
due to the software design. Most commonly a media pipeline consists of a single
stream which does not branch.

When starting streaming, drivers must notify all entities in the pipeline to
prevent link states from being modified during streaming by calling
:c:func:`media_pipeline_start()`.

The function will mark all the pads which are part of the pipeline as streaming.

The struct media_pipeline instance pointed to by the pipe argument will be
stored in every pad in the pipeline. Drivers should embed the struct
media_pipeline in higher-level pipeline structures and can then access the
pipeline through the struct media_pad pipe field.

Calls to :c:func:`media_pipeline_start()` can be nested.
The pipeline pointer must be identical for all nested calls to the function.

:c:func:`media_pipeline_start()` may return an error. In that case,
it will clean up any of the changes it did by itself.

When stopping the stream, drivers must notify the entities with
:c:func:`media_pipeline_stop()`.

If multiple calls to :c:func:`media_pipeline_start()` have been
made the same number of :c:func:`media_pipeline_stop()` calls
are required to stop streaming.
The :c:type:`media_entity`.\ ``pipe`` field is reset to ``NULL`` on the last
nested stop call.

Link configuration will fail with ``-EBUSY`` by default if either end of the
link is a streaming entity. Links that can be modified while streaming must
be marked with the ``MEDIA_LNK_FL_DYNAMIC`` flag.

If other operations need to be disallowed on streaming entities (such as
changing entities configuration parameters) drivers can explicitly check the
media_entity stream_count field to find out if an entity is streaming. This
operation must be done with the media_device graph_mutex held.

Link validation
^^^^^^^^^^^^^^^

Link validation is performed by :c:func:`media_pipeline_start()`
for any entity which has sink pads in the pipeline. The
:c:type:`media_entity`.\ ``link_validate()`` callback is used for that
purpose. In ``link_validate()`` callback, entity driver should check
that the properties of the source pad of the connected entity and its own
sink pad match. It is up to the type of the entity (and in the end, the
properties of the hardware) what matching actually means.

Subsystems should facilitate link validation by providing subsystem specific
helper functions to provide easy access for commonly needed information, and
in the end provide a way to use driver-specific callbacks.

Pipeline traversal
^^^^^^^^^^^^^^^^^^

Once a pipeline has been constructed with :c:func:`media_pipeline_start()`,
drivers can iterate over entities or pads in the pipeline with the
:c:macro:´media_pipeline_for_each_entity` and
:c:macro:´media_pipeline_for_each_pad` macros. Iterating over pads is
straightforward:

.. code-block:: c

   media_pipeline_pad_iter iter;
   struct media_pad *pad;

   media_pipeline_for_each_pad(pipe, &iter, pad) {
       /* 'pad' will point to each pad in turn */
       ...
   }

To iterate over entities, the iterator needs to be initialized and cleaned up
as an additional steps:

.. code-block:: c

   media_pipeline_entity_iter iter;
   struct media_entity *entity;
   int ret;

   ret = media_pipeline_entity_iter_init(pipe, &iter);
   if (ret)
       ...;

   media_pipeline_for_each_entity(pipe, &iter, entity) {
       /* 'entity' will point to each entity in turn */
       ...
   }

   media_pipeline_entity_iter_cleanup(&iter);

Media Controller Device Allocator API
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When the media device belongs to more than one driver, the shared media
device is allocated with the shared struct device as the key for look ups.

The shared media device should stay in registered state until the last
driver unregisters it. In addition, the media device should be released when
all the references are released. Each driver gets a reference to the media
device during probe, when it allocates the media device. If media device is
already allocated, the allocate API bumps up the refcount and returns the
existing media device. The driver puts the reference back in its disconnect
routine when it calls :c:func:`media_device_delete()`.

The media device is unregistered and cleaned up from the kref put handler to
ensure that the media device stays in registered state until the last driver
unregisters the media device.

**Driver Usage**

Drivers should use the appropriate media-core routines to manage the shared
media device life-time handling the two states:
1. allocate -> register -> delete
2. get reference to already registered device -> delete

call :c:func:`media_device_delete()` routine to make sure the shared media
device delete is handled correctly.

**driver probe:**
Call :c:func:`media_device_usb_allocate()` to allocate or get a reference
Call :c:func:`media_device_register()`, if media devnode isn't registered

**driver disconnect:**
Call :c:func:`media_device_delete()` to free the media_device. Freeing is
handled by the kref put handler.

Media Jobs Framework
^^^^^^^^^^^^^^^^^^^^

The media jobs framework exists to facilitate situations in which multiple
drivers must work together to properly operate a media pipeline in a driver
agnostic way. The archetypical example is of a memory to memory ISP that does
not include its own DMA input engine, and which must interact with the driver
for one that has been integrated. Because the DMA engine and its driver may be
different between each implementation, hardcoding calls of functions exported by
the DMA engine driver would not be appropriate. The media jobs framework allows
the drivers to define the steps that each must execute to correctly push data
through the pipeline and then schedule the sequence of steps to run in a work
queue.

To start with each driver must acquire a reference to a
:c:type:`media_jobs_scheduler` by calling :c:func:`media_jobs_get_scheduler()`,
passing the pointer to their :c:type:`media_device`. This ensures that all of
the drivers are working with the same scheduler. Drivers must then call
:c:func:`media_jobs_add_job_setup_func()` to register a function that populates
each job with the dependencies that must be cleared to allow it to operate, and
the steps that must be carried out to execute it. For example:

.. code-block:: c

   static void isp_driver_run_step(void *data)
   {
       struct isp *isp = data;

       /*
        * Logic here to actually execute the necessary steps, for example we
        * might configure some hardware registers.
        */
       ...;
   }

   static struct media_job_dep_ops ops = {
       ...,
   };

   static int isp_driver_add_job_setup_func(struct media_job *job, void *data)
   {
       int ret;

       ret = media_jobs_add_job_dep(job, &ops, data);
       if (ret)
           return ret;

       ret = media_jobs_add_job_step(job, isp_driver_run_step, data,
                                     MEDIA_JOBS_FL_STEP_ANYWHERE, 0);
       if (ret)
           return ret;

       return 0;
   }

The flags parameter of `media_jobs_add_job_step()` must be one of
:c:macro:`MEDIA_JOBS_FL_STEP_ANYWHERE`, :c:macro:`MEDIA_JOBS_FL_STEP_FROM_FRONT`
or :c:macro:`MEDIA_JOBS_FL_STEP_FROM_BACK`. The flag and pos parameters together
define the order of the step within the job. Steps added with
`MEDIA_JOBS_FL_STEP_ANYWHERE` will go after all steps that are added with
`MEDIA_JOBS_FL_STEP_FROM_FRONT` and all steps with `MEDIA_JOBS_FL_STEP_ANYWHERE`
that either have a lower `pos` or were previously added. They will go before all
those added with `MEDIA_JOBS_FL_STEP_FROM_BACK` and all steps with
`MEDIA_JOBS_FL_STEP_ANYWHERE` that have a higher `pos`. Steps added with
`MEDIA_JOBS_FL_STEP_FROM_FRONT` will go `pos` places from the front of the list,
and steps added with `flags` set to `MEDIA_JOBS_FL_STEP_FROM_BACK`` will go
`pos` places from the end of the list. This allows multiple drivers to quite
precisely define which steps need to be executed and what order they should be
executed in.

Adding a step with the same `flags` and `pos` as a previously added step will
result in an error.

The functions held in :c:type:`media_job_dep_ops` define how the media jobs
framework handles job dependencies. It is expected that there will be some hard
dependencies before a job can be executed; for example pushing a buffer of image
data through an ISP pipeline necessarily requires that an input buffer be ready
and an output buffer be ready to accept the processed data. The operations ask
the driver if the dependencies are met, tell the driver that a job has been
queued and reset the dependencies in the event the job is cancelled:

.. code-block:: c

   struct isp {

       ...;

       struct {
           struct list_head pending;
	   struct list_head processing;
       } buffers;
   }

   static bool isp_driver_check_dep(void *data)
   {
       struct isp *isp = data;

       /*
        * Do we have a buffer queued ready to accept the ISP's output data?
        */
       if (list_empty(isp->buffers.pending))
           return false;

       return true;
   }

   static void isp_driver_clear_dep(void *data)
   {
       struct isp *isp = data;
       struct buf *buf;

       /*
        * We need to "consume" the buffer so that it's not also considered as
        * meeting this dependency for the next attempt to queue a job
        */
       buf = list_first_entry(&isp->buffers.pending, struct buf, list);
       list_move_tail(&buf->list, isp->buffers.processing);
   }

   static void isp_driver_reset_dep(void *data)
   {
       struct isp *isp = data;
       struct buf *buf;

       /*
        * If a queued job is cancelled then we need to return the dependency to
        * its original state, which in this example means returning it to the
        * pending queue.
        */
       buf = list_first_entry(&isp->buffers.pending, struct buf, list);
       list_move_tail(&buf->list, isp->buffers.pending);
   }

   static struct media_job_dep_ops ops = {
       .check_dep = isp_driver_check_dep,
       .clear_dep = isp_driver_clear_dep,
       .reset_dep = isp_driver_reset_dep,
   };

The actual creation and queueing of the jobs should be done by the drivers by
calling :c:func:`media_jobs_try_queue_job()` at any time a dependency of the
job is met - for example (following the earlier example) when a buffer is queued
to either the ISP or DMA engine's driver. When all of the dependencies that are
necessary for a job to be queued are met, this function will push a job to the
scheduler's queue.

The scheduler has a workqueue that runs the jobs. This is triggered by calls to
the :c:func:`media_jobs_run_jobs()` function, which must be called periodically
as the pipeline is running. When the streaming is finished the drivers should
shut down the workqueue and cancel the queued jobs by calling
:c:func:`media_jobs_cancel_jobs()`.

API Definitions
^^^^^^^^^^^^^^^

.. kernel-doc:: include/media/media-device.h

.. kernel-doc:: include/media/media-devnode.h

.. kernel-doc:: include/media/media-entity.h

.. kernel-doc:: include/media/media-jobs.h

.. kernel-doc:: include/media/media-request.h

.. kernel-doc:: include/media/media-dev-allocator.h
