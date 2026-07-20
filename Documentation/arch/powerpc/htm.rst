.. SPDX-License-Identifier: GPL-2.0
.. _htm:

===================================
HTM (Hardware Trace Macro)
===================================

Athira Rajeev, 2 Mar 2025

.. contents::
    :depth: 3


Basic overview
==============

H_HTM is used as an interface for executing Hardware Trace Macro (HTM)
functions, including setup, configuration, control and dumping of the HTM data.
For using HTM, it is required to setup HTM buffers and HTM operations can
be controlled using the H_HTM hcall. The hcall can be invoked for any core/chip
of the system from within a partition itself.

To use this feature, a debugfs folder called "htmdump" is present under
/sys/kernel/debug/powerpc. Another interface is via perf.

HTM debugfs example usage
=========================

.. code-block:: sh

  #  ls /sys/kernel/debug/powerpc/htmdump/
  coreindexonchip  htmcaps  htmconfigure  htmflags  htminfo  htmsetup
  htmstart  htmstatus  htmtype  nodalchipindex  nodeindex  trace

Details on each file:

* nodeindex, nodalchipindex, coreindexonchip specifies which partition to configure the HTM for.
* htmtype: specifies the type of HTM. Supported target is hardwareTarget.
* trace: is to read the HTM data.
* htmconfigure: Configure/Deconfigure the HTM. Writing 1 to the file will configure the trace, writing 0 to the file will do deconfigure.
* htmstart: start/Stop the HTM. Writing 1 to the file will start the tracing, writing 0 to the file will stop the tracing.
* htmstatus: get the status of HTM. This is needed to understand the HTM state after each operation.
* htmsetup: set the HTM buffer size. Size of HTM buffer is in power of 2
* htminfo: provides the system processor configuration details. This is needed to understand the appropriate values for nodeindex, nodalchipindex, coreindexonchip.
* htmcaps : provides the HTM capabilities like minimum/maximum buffer size, what kind of tracing the HTM supports etc.
* htmflags : allows to pass flags to hcall. Currently supports controlling the wrapping of HTM buffer.

To see the system processor configuration details:

.. code-block:: sh

  # cat /sys/kernel/debug/powerpc/htmdump/htminfo > htminfo_file

The result can be interpreted using hexdump.

To collect HTM traces for a partition represented by nodeindex as
zero, nodalchipindex as 1 and coreindexonchip as 12

.. code-block:: sh

  # cd /sys/kernel/debug/powerpc/htmdump/
  # echo 2 > htmtype
  # echo 33 > htmsetup ( sets 8GB memory for HTM buffer, number is size in power of 2 )

This requires a CEC reboot to get the HTM buffers allocated.

.. code-block:: sh

  # cd /sys/kernel/debug/powerpc/htmdump/
  # echo 2 > htmtype
  # echo 0 > nodeindex
  # echo 1 > nodalchipindex
  # echo 12 > coreindexonchip
  # echo 1 > htmflags     # to set noWrap for HTM buffers
  # echo 1 > htmconfigure # Configure the HTM
  # echo 1 > htmstart     # Start the HTM
  # echo 0 > htmstart     # Stop the HTM
  # echo 0 > htmconfigure # Deconfigure the HTM
  # cat htmstatus         # Dump the status of HTM entries as data

Above will set the htmtype and core details, followed by executing respective HTM operation.

Read the HTM trace data
========================

After starting the trace collection, run the workload
of interest. Stop the trace collection after required period
of time, and read the trace file.

.. code-block:: sh

  # cat /sys/kernel/debug/powerpc/htmdump/trace > trace_file

This trace file will contain the relevant instruction traces
collected during the workload execution. And can be used as
input file for trace decoders to understand data.

HTM perf interface usage
========================

The HTM (Hardware Trace Macro) perf interface enables collection and analysis
of hardware trace data from PowerPC systems. This interface allows users to
capture detailed execution traces for performance analysis and debugging.

Event Configuration
-------------------

Use ``perf record`` with the htm PMU event. The event is configured using
named parameters that specify the target hardware location and trace type:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Parameter
     - Description
   * - htm_type
     - Type of HTM trace to collect (bits 0-3)
   * - nodeindex
     - Node index in the system topology (bits 4-11)
   * - nodalchipindex
     - Chip index within the specified node (bits 12-19)
   * - coreindexonchip
     - Core index on the specified chip (bits 20-27)

- event: "config:0-27"
- htm_type: "config:0-3"
- nodeindex: "config:4-11"
- nodalchipindex: "config:12-19"
- coreindexonchip: "config:20-27"

1) nodeindex, nodalchipindex, coreindexonchip: this specifies
   which partition to configure the HTM for.
2) htmtype: specifies the type of HTM.

Event Syntax
------------

The event configuration uses named parameters::

   htm/nodeindex=N,nodalchipindex=C,coreindexonchip=R,htm_type=T/

To open the event on a specific cpu can be specified using::

   htm/nodeindex=N,nodalchipindex=C,coreindexonchip=R,htm_type=T,cpu=x/

Where:

- N = node index
- C = chip index within the node
- R = core index on the chip
- T = HTM type
- x = CPU number

Basic Usage Example
-------------------

To collect HTM trace data for a specific chip:

.. code-block:: sh

   # perf record -C 1 -e htm/nodalchipindex=2,nodeindex=0,htm_type=1/ <workload>

In this example:

- ``-C 1``: Collect on CPU 1
- ``nodeindex=0``: Target node 0
- ``nodalchipindex=2``: Target chip 2 within node 0
- ``htm_type=1``: HTM trace type 1

.. code-block:: sh

   # perf record -m,256 -e htm/coreindexonchip=6,nodalchipindex=0,nodeindex=0,htm_type=2,cpu=16/ -a sleep 1

In this example:

- ``cpu=16``: Collect on CPU 16
- ``nodeindex=0``: Target node 0
- ``nodalchipindex=0``: Target chip 0 within node 0
- ``coreindexonchip=6``: Target code 6
- ``htm_type=2``: HTM trace type 2
- ``-m,256``: specifies number of mmap pages

Running trace collection for multiple targets:

.. code-block:: sh

   # perf record -m,256 -e htm/nodalchipindex=2,nodeindex=0,htm_type=1,cpu=8/ -e htm/nodalchipindex=1,nodeindex=0,htm_type=1,cpu=9/ -a sleep 1


In this example, trace is collected for two events on different target chips

Output Files
------------

After running ``perf record``, the following files are generated:

.. code-block:: sh

   # ls htm.bin.*
   htm.bin.n0.p2.c0 htm.bin.n1.p3.c0  # Binary trace files

   # ls translation.*
   translation.n0.p2.c0  translation.n1.p3.c0  # Memory configuration files

These files contain:

- **htm.bin.*** - Raw HTM trace data in binary format
- **translation.*** - Memory address translation information for decoding

Complete Workflow Example
--------------------------

Here's a complete example of collecting and analyzing HTM traces:

.. code-block:: sh

   # Step 1: Collect trace data
   perf record -C 1 -e htm/nodalchipindex=2,nodeindex=0,htm_type=1/ sleep 5

   # Step 2: Verify output files
   perf report -D

   ls htm.bin.*        # Binary trace files
   ls translation.*    # Memory configuration files
   ls perf.data        # Perf data file

Benefits of using HTM interface
=======================================

It is now possible to collect traces for a particular core/chip
from within any partition of the system and decode it. Through
this enablement, a small partition can be dedicated to collect the
trace data and analyze to provide important information for Performance
analysis, Software tuning, or Hardware debug.
