.. SPDX-License-Identifier: GPL-2.0

Attack Vector Controls
======================

Attack vector controls provide a simple method to configure only the mitigations
for CPU vulnerabilities which are relevant given the intended use of a system.
Administrators are encouraged to consider which attack vectors are relevant and
disable all others in order to recoup system performance.

When new relevant CPU vulnerabilities are found, they will be added to these
attack vector controls so administrators will likely not need to reconfigure
their command line parameters as mitigations will continue to be correctly
applied based on the chosen attack vector controls.

Attack Vectors
--------------

There are 5 sets of attack-vector mitigations currently supported by the kernel:

#. :ref:`user_kernel` (mitigate_user_kernel= )
#. :ref:`user_user` (mitigate_user_user= )
#. :ref:`guest_host` (mitigate_guest_host= )
#. :ref:`guest_guest` (mitigate_guest_guest=)
#. :ref:`cross_thread` (mitigate_cross_thread= )

Each control may either be specified as 'off' or 'on'.

.. _user_kernel:

User-to-Kernel
^^^^^^^^^^^^^^

The user-to-kernel attack vector involves a malicious userspace program
attempting to leak kernel data into userspace by exploiting a CPU vulnerability.
The kernel data involved might be limited to certain kernel memory, or include
all memory in the system, depending on the vulnerability exploited.

If no untrusted userspace applications are being run, such as with single-user
systems, consider disabling user-to-kernel mitigations.

Note that the CPU vulnerabilities mitigated by Linux have generally not been
shown to be exploitable from browser-based sandboxes.  User-to-kernel
mitigations are therefore mostly relevant if unknown userspace applications may
be run by untrusted users.

*mitigate_user_kernel defaults to 'on'*

.. _user_user:

User-to-User
^^^^^^^^^^^^

The user-to-user attack vector involves a malicious userspace program attempting
to influence the behavior of another unsuspecting userspace program in order to
exfiltrate data.  The vulnerability of a userspace program is based on the
program itself and the interfaces it provides.

If no untrusted userspace applications are being run, consider disabling
user-to-user mitigations.

Note that because the Linux kernel contains a mapping of all physical memory,
preventing a malicious userspace program from leaking data from another
userspace program requires mitigating user-to-kernel attacks as well for
complete protection.

*mitigate_user_user defaults to 'on'*

.. _guest_host:

Guest-to-Host
^^^^^^^^^^^^^

The guest-to-host attack vector involves a malicious VM attempting to leak
hypervisor data into the VM.  The data involved may be limited, or may
potentially include all memory in the system, depending on the vulnerability
exploited.

If no untrusted VMs are being run, consider disabling guest-to-host mitigations.

*mitigate_guest_host defaults to 'on' if KVM support is present*

.. _guest_guest:

Guest-to-Guest
^^^^^^^^^^^^^^

The guest-to-guest attack vector involves a malicious VM attempting to influence
the behavior of another unsuspecting VM in order to exfiltrate data.  The
vulnerability of a VM is based on the code inside the VM itself and the
interfaces it provides.

If no untrusted VMs, or only a single VM is being run, consider disabling
guest-to-guest mitigations.

Similar to the user-to-user attack vector, preventing a malicious VM from
leaking data from another VM requires mitigating guest-to-host attacks as well
due to the Linux kernel phys map.

*mitigate_guest_guest defaults to 'on' if KVM support is present*

.. _cross_thread:

Cross-Thread
^^^^^^^^^^^^

The cross-thread attack vector involves a malicious userspace program or
malicious VM either observing or attempting to influence the behavior of code
running on the SMT sibling thread in order to exfiltrate data.

Many cross-thread attacks can only be mitigated if SMT is disabled, which will
result in reduced CPU core count and reduced performance.  Enabling mitigations
for the cross-thread attack vector may result in SMT being disabled, depending
on the CPU vulnerabilities detected.

*mitigate_cross_thread defaults to 'off'*

Interactions with command-line options
--------------------------------------

The global 'mitigations=off' command line takes precedence over all attack
vector controls and will disable all mitigations.

Vulnerability-specific controls (e.g. "retbleed=off") take precedence over all
attack vector controls.  Mitigations for individual vulnerabilities may be
turned on or off via their command-line options regardless of the attack vector
controls.

Summary of attack-vector mitigations
------------------------------------

When a vulnerability is mitigated due to an attack-vector control, the default
mitigation option for that particular vulnerability is used.  To use a different
mitigation, please use the vulnerability-specific command line option.

The table below summarizes which vulnerabilities are mitigated when different
attack vectors are enabled and assuming the CPU is vulnerable.

=============== ============== ============ ============= ============== ============
Vulnerability   User-to-Kernel User-to-User Guest-to-Host Guest-to-Guest Cross-Thread
=============== ============== ============ ============= ============== ============
BHI                   X                           X
GDS                   X              X            X              X
L1TF                                              X                       (Note 1)
MDS                   X              X            X              X        (Note 1)
MMIO                  X              X            X              X        (Note 1)
Meltdown              X
Retbleed              X                           X                       (Note 2)
RFDS                  X              X            X              X
Spectre_v1            X
Spectre_v2            X                           X
Spectre_v2_user                      X                           X
SRBDS                 X              X            X              X
SRSO                  X                           X
SSB (Note 3)
TAA                   X              X            X              X        (Note 1)
=============== ============== ============ ============= ============== ============

Notes:
   1 --  Disables SMT if cross-thread mitigations are selected and CPU is vulnerable

   2 --  Disables SMT if cross-thread mitigations are selected, CPU is vulnerable,
   and STIBP is not supported

   3 --  Speculative store bypass is always enabled by default (no kernel
   mitigation applied) unless overridden with spec_store_bypass_disable option

When an attack-vector is disabled (e.g., *mitigate_user_kernel=off*), all
mitigations for the vulnerabilities listed in the above table are disabled,
unless mitigation is required for a different enabled attack-vector or a
mitigation is explicitly selected via a vulnerability-specific command line
option.
