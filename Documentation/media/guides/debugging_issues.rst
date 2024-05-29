.. SPDX-License-Identifier: GPL-2.0
.. include:: <isonum.txt>

===================================================
Debugging and tracing in the media subsystem
===================================================

This document serves as a starting point and lookup for debugging device
drivers in the media subsystem.

.. contents::
    :depth: 3

General debugging advice
========================

Depending on the issue, a different set of tools is available to track down the
problem or even to realize whether there is one in the first place.

As a first step you have to figure out what kind of issue you want to debug.
Depending on the answer, your methodology and choice of tools may vary.
Some typical issues are listed below. The list is not exhaustive.

What is the issue?
------------------
1. Driver doesn't behave as expected

   TBD

2. Running out of memory

  In this case you should use `Linux CLI tools <linux-tools_>`_ first to verify
  whether the issue stems from the Kernel or from userspace.

  TBD

3. Kernel panic

  The kernel provides a number of `tools <kernel_panic_analysis_tools_>`_ to
  analyse a Kernel panic, use these first, once you have identified where the
  error occurs you can further analyse the issue by adding `debug logs <Printk
  & friends_>`_ to the codebase.

4. Device doesn't probe

   TBD

5. Driver too slow

  You should start with a `performance analysis <performance>`__, to pin down the problem area.

Is timing a factor?
-------------------

It is important to understand if the problem you want to debug manifests itself
consistently (i.e. given a set of inputs you always get the same, incorrect
output), or inconsistently. If it manifests itself inconsistently, some timing
factor might be at play. If inserting delays into the code does change the
behavior, then quite likely timing is a factor.

   In this case using a simple `printk`_ won't work, a similar alternative is
   too use `trace_printk`_, which logs the debug messages to the trace file
   instead of the kernel log.

Do I have root access to the system?
------------------------------------

Are you easily able to replace the module in question or to install a new kernel?

TBD

.. _live_debug:

Do I need to debug on a live system?
------------------------------------

When the kernel is compiled with `Ftrace`_, this tool might be a good start, as you can trace specific functions to dial down to the sequence of actions that causes the misbehavior.

TBD

Is the outcome flaky?
---------------------

We will call the outcome "flaky" if multiple iterations of the same routine with the same parameters result in different outcomes.

TBD

Available tools
===============

Printk & friends
----------------

These are derivatives of printf() with varying destination and support for being dynamically turned on or off, or lack thereof.

.. _printk:

**Simple printk**
~~~~~~~~~~~~~~~~~

The classic, this can be used to great effect for quick and dirty development
of new modules or to extract arbitrary necessary data for troubleshooting.

Prerequisite: `CONFIG_PRINTK` (usually enabled by default)

**Pros**:

- No need to learn anything, simple to use
- Easy to modify exactly to your needs (formatting of the data (See: `format specifiers <../../core-api/printk-formats.html#printk-specifiers>`__), visibility in the log)
- Can cause delays in the execution of the code (beneficial to confirm whether timing is a factor)

**Cons**:

- Requires rebuilding the kernel/module, (See: `live_debug`_)
- Can cause delays in the execution of the code

`Full documentation <../../core-api/printk-basics.rst>`__

.. _trace_printk:

**Trace_printk**
~~~~~~~~~~~~~~~~

Prerequisite: `CONFIG_DYNAMIC_FTRACE` & `#include <linux/ftrace.h>`

A tiny bit less comfortable to use than `printk`_, because you will have to
read the messages from the trace file (See: `Reading the ftrace log`_ instead
of from the kernel log, but very useful when printk adds unwanted delays into
the code execution, causing issues to be flaky or hidden.)

If the processing of this still causes timing issues then you can try `trace_puts()`.

`Full Documentation <../../driver-api/basics.html#c.trace_printk>`__

**dev_dbg / v4l2_dbg**
~~~~~~~~~~~~~~~~~~~~~~

- Difference between both?

  - v4l2_dbg utilizes v4l2_printk under the hood, which further uses printk directly, thus it cannot be targeted by dynamic debug
  - dev_dbg can be targeted by dynamic debug
  - v4l2_dbg has a more specific prefix format for the media subsystem, while dev_dbg only highlights the driver name and the location of the log

**The dev_debug module parameter**

Every video device provides a `dev_debug` parameter, which allows to get further insights into the IOCTLs in the background.

`Full documentation <../../driver-api/media/v4l2-dev.html#video-device-debugging>`__

**When is it appropriate to leave a debug print in the code?**

Permanent debug statements have to be useful for a developer to troubleshoot
why the driver misbehaves. Judging that is a bit more of an art than a
science, but some guidelines are in the `Coding style guide
<../../process/coding-style.html#printing-kernel-messages>`__)

**Custom printk**
~~~~~~~~~~~~~~~~~
Example:
::

  #define core_dbg(fmt, arg...) do { \
	  if (core_debug) \
		  printk(KERN_DEBUG pr_fmt("core: " fmt), ## arg); \
	  } while (0)

**When should you do this?**

It is better to just use a `pr_debug()`, which can later be turned on/off with
dynamic debug. Additionally, a lot of drivers activate these prints via a
variable like `core_debug` set by a module parameter. However, Module
parameters `are not recommended anymore
<https://lkml.org/lkml/2024/3/27/163>`_.

Dynamic debug
-------------

Mechanism to filter what ends up in the kernel log by dis-/en-abling log
messages.

Prerequisite: `CONFIG_DYNAMIC_DEBUG`

.. _valid_dyndbg_prints:

Dynamic debug is only able to target:

- `pr_debug()`
- `dev_dbg()`
- `print_hex_dump_debug()`
- `print_hex_dump_bytes()`

Therefore the usability of this tool is quite limited in the media subsystem,
because, as of now, there is no uniform rule for adding debug prints to the codebase,
resulting in a variety of ways these prints are implemented.

Also, note that most debug statements are implemented as a variation of
`dprintk`, which have to be activated via a parameter in respective module,
dynamic debug is unable to do that step for you.

Here is one example, that enables all available `pr_debug()`'s within the file:
::

  $ alias ddcmd='echo $* > /proc/dynamic_debug/control'
  $ ddcmd '-p; file v4l2-h264.c +p'
  $ grep =p /proc/dynamic_debug/control
   drivers/media/v4l2-core/v4l2-h264.c:372 [v4l2_h264]print_ref_list_b =p "ref_pic_list_b%u (cur_poc %u%c) %s"
   drivers/media/v4l2-core/v4l2-h264.c:333 [v4l2_h264]print_ref_list_p =p "ref_pic_list_p (cur_poc %u%c) %s\n"

**When should you use this over** `Ftrace`_ **?**

- When the code contains one of the :ref:`valid print statements <valid_dyndbg_prints_>`_ or when you have added multiple pr_debug() statements during development
- When timing is not an issue, meaning if multiple `pr_debug()` statements in the code won't cause delays
- When you care more about receiving specific log messages than tracing the pattern of how a function is called

`Full documentation <../../admin-guide/dynamic-debug-howto.rst>`__

Ftrace
------

Prerequisite: `CONFIG_DYNAMIC_FTRACE`

Trace whenever the `rkvdec_try_ctrl` function is called
::

  $ cd /sys/kernel/tracing
  $ echo function > /sys/kernel/tracing/current_tracer
  $ echo rkvdec_try_ctrl > set_ftrace_filter
  $ echo 1 > tracing_on
  $ cat trace
   h264parse0:sink-6359    [001] ...1. 172714.547523: rkvdec_try_ctrl <-try_or_set_cluster
   h264parse0:sink-6359    [005] ...1. 172714.567386: rkvdec_try_ctrl <-try_or_set_cluster

Find out from where the calls originate
::

  $ echo 1 > options/func_stack_trace
   h264parse0:sink-6715    [002] ..... 172837.967762: rkvdec_try_ctrl <-try_or_set_cluster
   h264parse0:sink-6715    [002] ..... 172837.967773: <stack trace>
   => rkvdec_try_ctrl
   => try_or_set_cluster
   => try_set_ext_ctrls_common
   => try_set_ext_ctrls
   => v4l2_s_ext_ctrls
   => v4l_s_ext_ctrls
   ...
   h264parse0:sink-6715    [004] ..... 172837.985747: rkvdec_try_ctrl <-try_or_set_cluster
   h264parse0:sink-6715    [004] ..... 172837.985750: <stack trace>
   => rkvdec_try_ctrl
   => try_or_set_cluster
   => v4l2_ctrl_request_setup
   => rkvdec_run_preamble
   => rkvdec_h264_run
   => rkvdec_device_run
   ...

Trace the children of a function call and show the return values (requires config `FUNCTION_GRAPH_RETVAL`)
::

  echo function_graph > current_tracer
  echo rkvdec_h264_run > set_graph_function
  echo 4 > max_graph_depth
  echo do_interrupt_handler mutex_* > set_graph_notrace
  echo 1 > options/funcgraph-retval
   ...
   4)               |  rkvdec_h264_run [rockchip_vdec]() {
   4)               |    v4l2_ctrl_find [videodev]() {
   ...
   4)               |    rkvdec_run_preamble [rockchip_vdec]() {
   4)   4.666 us    |      v4l2_m2m_next_buf [v4l2_mem2mem](); /* = 0xffff000005782000 */
   ...
   4)               |      v4l2_ctrl_request_setup [videodev]() {
   4)   4.667 us    |        media_request_object_find [mc](); /* = 0xffff000005e3aa98 */
   4)   1.750 us    |        find_ref [videodev](); /* = 0xffff00000833b2a0 */
   ...
   4)   1.750 us    |      v4l2_m2m_buf_copy_metadata [v4l2_mem2mem](); /* = 0x0 */
   4) ! 114.333 us  |    } /* rkvdec_run_preamble [rockchip_vdec] = 0x0 */
   4)   2.334 us    |    v4l2_h264_init_reflist_builder [v4l2_h264](); /* = 0x3e */
   ...
   4)               |    v4l2_h264_build_b_ref_lists [v4l2_h264]() {
   ...
   4)               |    rkvdec_run_postamble [rockchip_vdec]() {
   ...
   4) ! 444.208 us  |  } /* rkvdec_h264_run [rockchip_vdec] = 0x0 */

**Reading the ftrace log**
~~~~~~~~~~~~~~~~~~~~~~~~~~

The `trace` file can be read just like any other file (`cat`, `tail`, `head`,
`vim`, etc.), the size of the file is limited by the `buffer_size_kb` (``echo
1000 > buffer_size_kb``). The `trace_pipe` will behave similar to the `trace`
file, but whenever you read from the file the content is consumed.

**Kernelshark**
~~~~~~~~~~~~~~~

`Kernelshark documentation <https://kernelshark.org/Documentation.html>`_

`Full documentation <../../trace/ftrace.rst>`__

DebugFS
-------

Prerequisite: `CONFIG_DEBUG_FS` & `#include <linux/debugfs.h>`

DebugFS differs from the other approaches of debugging, as it doesn't write messages to the kernel log nor add traces to the code. Instead it allows the developer to handle a set of files.
With these files you can either store values of variables or make register/memory dumps or you can make these files writable and modify values/settings in the driver.
Possible use-cases among others:

- Store register values
- Keep track of variables
- Store errors
- Store settings
- Toggle a setting like debug on/off
- Error injection

This is especially useful, when the size of a data dump would be hard to digest as
part of the general kernel log (for example when dumping raw bitstream data) or
when you are not interested in all the values all the time, but with the
possibility to inspect them.

The general idea is:

- Create a directory during probe (`struct dentry *parent = debugfs_create_dir("my_driver", NULL);`)
- Create a file (`debugfs_create_u32("my_value", 444, parent, &my_variable);`)

  - In this example the file is found in `/sys/kernel/debug/my_driver/my_value` (with read permissions for user/group/all)
  - any update of `my_variable` will update the value in the file

- Clean up the folder when removing the device (`debugfs_remove_recursive(parent);`)

`Full documentation <../../filesystems/debugfs.rst>`__

Perf & alternatives
-------------------

The tools mentioned above provide ways to inspect kernel code, results, variable values, etc.
Sometimes you have to find out first where to look and for those cases a box of performance tracking tools can help you to frame the issue.

.. _performance:

**Why should you do a performance analysis?**
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A performance analysis is a good first step when among other reasons:

- you cannot define the issue
- you do not know where it occurs
- the running system should not be interrupted or it is a remote system, where you cannot install a new module/kernel

.. _linux-tools:

**How to do a simple analysis with linux tools?**
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For the start of a performance analysis, you can start with the usual tools like:

- `top` / `htop` / `atop` (*get an overview of the system load, see spikes on specific processes*)
- `mpstat -P ALL` (*look at the load distribution among CPUs*)
- `iostat -x` (*observe input and output devices utilization and performance*)
- `vmstat` (*overview of memory usage on the system*)
- `pidstat` (*similar to* `vmstat` *but per process, to dial it down to the target*)
- `strace -tp $PID` (*once you know the process, you can figure out how it communicates with the Kernel*)

These should help to figure out restrict the areas to look at sufficiently.

**Diving deeper with perf**
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The **perf** tool provides a series of metrics and events to further dial down on issues.

Prerequisite: TBD

Gather statistics data for a decoding job: (This example is on a RK3399 SoC with the rkvdec codec driver using the `fluster test suite <https://github.com/fluendo/fluster>`__)
::

  perf stat -d python3 fluster.py run -d GStreamer-H.264-V4L2SL-Gst1.0 -ts JVT-AVC_V1 -tv AUD_MW_E -j1
  ...
  Performance counter stats for 'python3 fluster.py run -d GStreamer-H.264-V4L2SL-Gst1.0 -ts JVT-AVC_V1 -tv AUD_MW_E -j1 -v':

           7794.23 msec task-clock:u                     #    0.697 CPUs utilized
                 0      context-switches:u               #    0.000 /sec
                 0      cpu-migrations:u                 #    0.000 /sec
             11901      page-faults:u                    #    1.527 K/sec
         882671556      cycles:u                         #    0.113 GHz                         (95.79%)
         711708695      instructions:u                   #    0.81  insn per cycle              (95.79%)
          10581935      branches:u                       #    1.358 M/sec                       (15.13%)
           6871144      branch-misses:u                  #   64.93% of all branches             (95.79%)
         281716547      L1-dcache-loads:u                #   36.144 M/sec                       (95.79%)
           9019581      L1-dcache-load-misses:u          #    3.20% of all L1-dcache accesses   (95.79%)
   <not supported>      LLC-loads:u
   <not supported>      LLC-load-misses:u

      11.180830431 seconds time elapsed

       1.502318000 seconds user
       6.377221000 seconds sys

The availability of events and metrics depends on the system you are running.

`Full documentation <https://perf.wiki.kernel.org/index.php/Main_Page>`__

**Perfetto**
~~~~~~~~~~~~

TBD

.. _error_checking:

KASAN, UBSAN, lockdep and other error checkers
----------------------------------------------

TBD

.. _kernel_panic_analysis_tools:

Kernel panic analysis tools
---------------------------

  To analyse the crash dump please use `Kdump` & `Kexec`.

  TBD

  `Full documentation <../../admin-guide/kdump/kdump.rst>`__

  In order to find the corresponding line in the code you can use `faddr2line
  <https://elixir.bootlin.com/linux/latest/source/scripts/faddr2line>`__, note
  that you need to enable `CONFIG_DEBUG_INFO` for that to work.

  An alternative to using `faddr2line` is the use of `objdump` (and it's
  derivatives for the different platforms like `aarch64-linux-gnu-objdump`),
  take this line as an example:

  `[  +0.000240]  rkvdec_device_run+0x50/0x138 [rockchip_vdec]`.

  We can find the corresponding line of code by executing:
  ::

    aarch64-linux-gnu-objdump -dS drivers/staging/media/rkvdec/rockchip-vdec.ko | grep rkvdec_device_run\>: -A 40
    0000000000000ac8 <rkvdec_device_run>:
     ac8:	d503201f 	nop
     acc:	d503201f 	nop
    {
     ad0:	d503233f 	paciasp
     ad4:	a9bd7bfd 	stp	x29, x30, [sp, #-48]!
     ad8:	910003fd 	mov	x29, sp
     adc:	a90153f3 	stp	x19, x20, [sp, #16]
     ae0:	a9025bf5 	stp	x21, x22, [sp, #32]
        const struct rkvdec_coded_fmt_desc *desc = ctx->coded_fmt_desc;
     ae4:	f9411814 	ldr	x20, [x0, #560]
        struct rkvdec_dev *rkvdec = ctx->dev;
     ae8:	f9418015 	ldr	x21, [x0, #768]
        if (WARN_ON(!desc))
     aec:	b4000654 	cbz	x20, bb4 <rkvdec_device_run+0xec>
        ret = pm_runtime_resume_and_get(rkvdec->dev);
     af0:	f943d2b6 	ldr	x22, [x21, #1952]
        ret = __pm_runtime_resume(dev, RPM_GET_PUT);
     af4:	aa0003f3 	mov	x19, x0
     af8:	52800081 	mov	w1, #0x4                   	// #4
     afc:	aa1603e0 	mov	x0, x22
     b00:	94000000 	bl	0 <__pm_runtime_resume>
        if (ret < 0) {
     b04:	37f80340 	tbnz	w0, #31, b6c <rkvdec_device_run+0xa4>
        dev_warn(rkvdec->dev, "Not good\n");
     b08:	f943d2a0 	ldr	x0, [x21, #1952]
     b0c:	90000001 	adrp	x1, 0 <rkvdec_try_ctrl-0x8>
     b10:	91000021 	add	x1, x1, #0x0
     b14:	94000000 	bl	0 <_dev_warn>
        *bad = 1;
     b18:	d2800001 	mov	x1, #0x0                   	// #0
     ...


  To find the matching line we just have to add `0x50` (from
  `rkvdec_device_run+0x50`) to `0xac8` (from `0000000000000ac8
  <rkvdec_device_run>:`), which yields `0xb18` corresponding with `*bad = 1`.

**Copyright** |copy| 2024 : Collabora
