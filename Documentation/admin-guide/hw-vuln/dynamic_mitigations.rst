.. SPDX-License-Identifier: GPL-2.0

Dynamic Mitigations
-------------------

Dynamic mitigation support enables the re-configuration of CPU vulnerability
mitigations through sysfs.  The file /sys/devices/system/cpu/mitigations
contains the current set of mitigation-related options.  The file can be written
to in order to make the kernel re-select and re-apply mitigations without a
reboot or kexec.

The data written to the file should be command line options related to
mitigation controls (e.g., "mitigations=auto spectre_v2=retpoline mds=off").
When the file is written, all previous selections related to mitigation controls
are discarded and the new options are evaluated.  Any non-mitigation related
options are ignored.

Dynamic mitigations are available if the CONFIG_DYNAMIC_MITIGATIONS option is
selected.

Purpose
-------

Dynamic mitigations serve two primary purposes:

Move Policy To Userspace
^^^^^^^^^^^^^^^^^^^^^^^^

Mitigation choices are related to the security policy and posture of the system.
Most mitigations are only necessary on shared, multi-user systems if untrusted
code may be run on the system, such as through untrusted userspace or untrusted
virtual machines.  The kernel may not know how the system will be used on boot,
and therefore must adopt a strong security posture for safety.

With dynamic mitigations, userspace can re-select mitigations once the needs of
the system can be determined and more policy information is available.

Mitigation Testing
^^^^^^^^^^^^^^^^^^

Dynamic mitigation support makes it easy to toggle individual mitigations or
choose between different mitigation options without the expense of a reboot or
kexec.  This may be useful when evaluating the performance of various
mitigation options.  It can also be useful for performing bug fixes without a
reboot, in case a particular mitigation is undesired or buggy.

Caveats
-------

There are a few limitations to dynamic mitigation support:

Runtime Limitations
^^^^^^^^^^^^^^^^^^^

There are a few mitigations that cannot be toggled at runtime due to the way
they are structured.  Specifically, kernel PTI (page table isolation) cannot be
toggled because of the complexity of this mitigation.  Additionally, SMT cannot
be disabled at runtime.  Therefore, if a bug mitigation requires disabling SMT,
a warning message will be printed.

BPF JIT
^^^^^^^

There is currently no way to recompile already JIT'd BPF programs.  This can
present a security problem if moving from a less secure security posture to a
more secure one.  It is recommended to either unload BPF programs prior to
re-configuring mitigations, ensure that security settings only become less
restrictive over time, or disable use of the BPF JIT.

Performance
-----------

Re-configuring mitigations is done under the biggest of hammers.  All tasks are
frozen, all cores are stopped, interrupts are masked, etc.  This may affect
system responsiveness if lots of patching must be done.
