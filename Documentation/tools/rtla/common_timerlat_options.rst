.. |threshold|  replace:: **-a/--auto**, **-i/--irq**, or **-T/--thread**
.. |thresharg|  replace:: -T
.. |tracer|     replace:: timerlat

.. |actionsperf| replace::
        For time-sensitive actions, it is recommended to run **rtla timerlat** with BPF
        support and RT priority. Note that due to implementational limitations, actions
        might be delayed up to one second after tracing is stopped if BPF mode is not
        available or disabled.

**-a**, **--auto** *us*

        Set the automatic trace mode. This mode sets some commonly used options
        while debugging the system. It is equivalent to use **-T** *us* **-s** *us*
        **-t**. By default, *timerlat* tracer uses FIFO:95 for *timerlat* threads,
        thus equilavent to **-P** *f:95*.

**-p**, **--period** *us*

        Set the *timerlat* tracer period in microseconds.

**-i**, **--irq** *us*

        Stop trace if the *IRQ* latency is higher than the argument in us.

**-T**, **--thread** *us*

        Stop trace if the *Thread* latency is higher than the argument in us.

**-s**, **--stack** *us*

        Save the stack trace at the *IRQ* if a *Thread* latency is higher than the
        argument in us.

**-t**, **--trace** \[*file*]

        Save the stopped trace to [*file|timerlat_trace.txt*].

**--dma-latency** *us*
        Set the /dev/cpu_dma_latency to *us*, aiming to bound exit from idle latencies.
        *cyclictest* sets this value to *0* by default, use **--dma-latency** *0* to have
        similar results.

**--deepest-idle-state** *n*
        Disable idle states higher than *n* for cpus that are running timerlat threads to
        reduce exit from idle latencies. If *n* is -1, all idle states are disabled.
        On exit from timerlat, the idle state setting is restored to its original state
        before running timerlat.

        Requires rtla to be built with libcpupower.

**-k**, **--kernel-threads**

        Use timerlat kernel-space threads, in contrast of **-u**.

**-u**, **--user-threads**

        Set timerlat to run without a workload, and then dispatches user-space workloads
        to wait on the timerlat_fd. Once the workload is awakes, it goes to sleep again
        adding so the measurement for the kernel-to-user and user-to-kernel to the tracer
        output. **--user-threads** will be used unless the user specify **-k**.

**-U**, **--user-load**

        Set timerlat to run without workload, waiting for the user to dispatch a per-cpu
        task that waits for a new period on the tracing/osnoise/per_cpu/cpu$ID/timerlat_fd.
        See linux/tools/rtla/sample/timerlat_load.py for an example of user-load code.

**--bpf-action** *bpf-program*

        Loads a BPF program from an ELF file and executes it when a latency threshold is exceeded.

        The BPF program must be a valid ELF file loadable with libbpf. The program must contain
        a function named ``action_handler``, declared with ``SEC("tp/timerlat_action")`` for libbpf
        to correctly detect the program type.

        The program receives a ``struct trace_event_raw_timerlat_sample`` parameter
        containing timerlat sample data.

        A sample BPF action program is provided in ``tools/tracing/rtla/sample/timerlat_bpf_action.c``.
        This example demonstrates how to create a BPF program that prints latency information when
        thresholds are exceeded.

        **Note**: BPF actions require BPF support to be available. If BPF is not available or disabled,
        the tool will fall back to tracefs mode and BPF actions will not be supported.
