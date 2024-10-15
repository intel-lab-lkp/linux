.. SPDX-License-Identifier: GPL-2.0

===============================================
General description of the remoteproc subsystem
===============================================

Authors:
	- anish kumar <yesanishhere@gmail.com>

.. Contents:

   1.  Introduction
   2.  Remoteproc framework responsibilities
   3.  Remoteproc driver responsibilities
   4.  Virtio and rpmsg

1. Introduction
======================

Modern System on Chips (SoCs) typically integrate heterogeneous remote
processor devices in asymmetric multiprocessing (AMP) configurations.
These processors may run different operating systems, such as Linux and
various real-time operating systems (RTOS).

For example, the OMAP4 platform features dual Cortex-A9 cores, dual
Cortex-M3 cores, and a C64x+ DSP. In a standard setup, the Cortex-A9
cores execute Linux in a symmetric multiprocessing (SMP) configuration,
while the M3 cores and DSP run independent instances of an RTOS.

The remoteproc framework allows various platforms and architectures to
manage remote processors, including operations such as powering on,
loading firmware, and powering off. This framework abstracts hardware
differences, promoting code reuse and minimizing duplication. It also
supports rpmsg virtio devices for remote processors that utilize this
communication method. Consequently, platform-specific remoteproc drivers
need only implement a few low-level handlers, enabling seamless operation
of all rpmsg drivers. (For more details about the virtio-based rpmsg
bus and its drivers, refer to :doc:`Documentation/staging/rpmsg.rst`.)

Additionally, the framework allows for the registration of various
virtio devices. Firmware can publish the types of virtio devices it
supports, facilitating their addition to the remoteproc framework. This
flexibility enables the reuse of existing virtio drivers with remote
processor backends at minimal development cost.

The primary purpose of the remoteproc framework is to download firmware
for remote processors and manage their lifecycle. The framework consists
of several key components:

- **Character Driver**: Provides userspace access to control the remote
  processor.
- **ELF Utility**: Offers functions for handling ELF files and managing
  resources requested by the remote processor.
- **Remoteproc Core**: Manages firmware downloads and recovery actions
  in case of a remote processor crash.
- **Coredump**: Provides facilities for coredumping and tracing from
  the remote processor in the event of a crash.
- **Userspace Interaction**: Uses sysfs and debugfs to manage the
  lifecycle and status of the remote processor.
- **Virtio Support**: Facilitates interaction with the virtio and
  rpmsg bus.

From here on, references to "framework" denote the remoteproc
framework, and "driver" refers to the remoteproc driver that utilizes
the framework for managing remote processors.

2. Remoteproc framework Responsibilities
========================================

The framework begins by gathering information about the firmware file
to be downloaded through the request_firmware function. It supports
the ELF format and parses the firmware image to identify the physical
addresses that need to be populated from the corresponding ELF sections.
The framework also requires knowledge of the logical or I/O-mapped
addresses in the application processor. Once this information is
obtained from the driver, the framework transfers the data to the
specified addresses and starts the remote, along with
any devices physically or logically connected to it.

Dependent devices, referred to as `subdevices` within the framework,
are also managed post-registration by their respective drivers.
Subdevices can register themselves using `rproc_(add/remove)_subdev`.
Non-remoteproc drivers can use subdevices as a way to logically connect
to remote and get lifecycle notifications of the remote.

The framework oversees the lifecycle of the remote and
provides the `rproc_report_crash` function, which the driver invokes
upon receiving a crash notification from the remote. The
notification method can differ based on the design of the remote
processor and its communication with the application processor. For
instance, if the remote is a DSP equipped with a watchdog,
unresponsive behavior triggers the watchdog, generating an interrupt
that routes to the application processor, allowing it to call
`rproc_report_crash` in the driver's interrupt context.

During crash handling, the framework performs the following actions:

a. Sends a request to stop the remote and any connected or
   dependent subdevices.
b. Generates a coredump, dumping all `resources` requested by the
   remote alongside relevant debugging information. Resources are
   explained below.
c. Reloads the firmware and restarts the remote.

If the `RPROC_FEAT_ATTACH_ON_RECOVERY` flag is set, the detach and
attach callbacks of the driver are invoked without reloading the
firmware. This is useful when the remote requires no
assistance for recovery, or when the application processor can restart
independently. After recovery, the application processor can reattach
to the remote.

The remote can request resources from the framework, which
allocates a ".resource_table" section. During the ELF parsing phase,
the framework identifies this section and calls the appropriate
handler to allocate the requested resources.

Resource management within the framework can accommodate any type of
`fw_resource_type`.

.. code-block:: c

   enum fw_resource_type {
       RSC_CARVEOUT      = 0,
       RSC_DEVMEM        = 1,
       RSC_TRACE         = 2,
       RSC_VDEV          = 3,
       RSC_LAST          = 4,
       RSC_VENDOR_START  = 128,
       RSC_VENDOR_END    = 512,
   };

   struct resource_table {
       u32 ver;
       u32 num;
       u32 reserved[2];
       u32 offset[];
   } __packed;

   struct fw_rsc_hdr {
       u32 type;
       u8 data[];
   } __packed;

For example, if the remote requests both `RSC_TRACE` and
`RSC_CARVEOUT` for memory allocation, the ELF firmware can be structured
as follows:

.. code-block:: c

   #define MAX_SHARED_RESOURCE 2
   #define LOG_BUF_SIZE 1000
   #define CARVEOUT_DUMP_PA 0x12345678
   #define CARVEOUT_DUMP_SIZE 2000

   struct shared_resource_table {
       u32 ver;
       u32 num;
       u32 reserved[2];
       u32 offset[MAX_SHARED_RESOURCE];
       struct fw_rsc_trace log_trace;
       struct fw_rsc_carveout dump_carveout;
   };

   volatile struct shared_resource_table table = {
       .ver = 1,
       .num = 2,
       .reserved = {0, 0},
       .offset = {
           offsetof(struct resource_table, log_trace),
           offsetof(struct resource_table, dump_carveout),
       },
       .log_trace = {
           RSC_TRACE,
           (u32)log_buf, LOG_BUF_SIZE, 0, "log_trace",
       },
       .dump_carveout = {
           RSC_CARVEOUT,
           (u32)FW_RSC_ADDR_ANY, CARVEOUT_PA, 0, "carveout_dump",
       },
   };

The framework creates a sysfs file when it encounters the `RSC_TRACE`
type to expose log information to userspace. Other resource types are
handled accordingly. In the example above, `CARVEOUT_DUMP_SIZE` bytes
of DMA memory will be allocated starting from `CARVEOUT_DUMP_PA`.


3. Remoteproc driver responsibilities
=====================================

The driver must provide the following information to the core:

a. Translate device addresses (physical addresses) found in the ELF
   firmware to virtual addresses in Linux using the `da_to_va`
   callback. This allows the framework to copy ELF firmware from the
   filesystem to the addresses expected by the remote since
   the framework cannot directly access those physical addresses.
b. Prepare/unprepare the remote prior to firmware loading,
   which may involve allocating carveout and reserved memory regions.
c. Implement methods for starting and stopping the remote,
   whether by setting registers or sending explicit interrupts,
   depending on the hardware design.
d. Provide attach and detach callbacks to start the remote
   without loading the firmware. This is beneficial when the remote
   processor is already loaded and running.
e. Implement a load callback for firmware loading, typically using
   the ELF loader provided by the framework; currently, only ELF
   format is supported.
f. Invoke the framework's crash handler API upon detecting a remote
   crash.

Drivers must fill the `rproc_ops` structure and call `rproc_alloc`
to register themselves with the framework.

.. code-block:: c

   struct rproc_ops {
       int (*prepare)(struct rproc *rproc);
       int (*unprepare)(struct rproc *rproc);
       int (*start)(struct rproc *rproc);
       int (*stop)(struct rproc *rproc);
       int (*attach)(struct rproc *rproc);
       int (*detach)(struct rproc *rproc);
       void * (*da_to_va)(struct rproc *rproc, u64 da, size_t len,
                          bool *is_iomem);
       int (*parse_fw)(struct rproc *rproc, const struct firmware *fw);
       int (*handle_rsc)(struct rproc *rproc, u32 rsc_type,
                         void *rsc, int offset, int avail);
       int (*load)(struct rproc *rproc, const struct firmware *fw);
       //snip
   };


4. Virtio and Remoteproc
========================

The firmware must provide remoteproc with information regarding the
virtio devices it supports and their configurations: an `RSC_VDEV`
resource entry should detail the virtio device ID (as defined in
`virtio_ids.h`), virtio features, virtio config space, vrings
information, etc.

Upon registration of a new remote, the remoteproc framework
searches for its resource table and registers the supported virtio
devices. A firmware may support multiple virtio devices, of various
types (a single remote can support multiple rpmsg virtio
devices if required).

Moreover, `RSC_VDEV` resource entries suffice for static allocation
of virtio devices. Dynamic allocations will also be supported using
the rpmsg bus, akin to the handling of dynamic allocations for rpmsg
channels. For more information, refer to `rpmsg.txt`.
