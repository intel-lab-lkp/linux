.. SPDX-License-Identifier: GPL-2.0

=============
Old Microcode
=============

The kernel keeps a table of released microcode. Systems that have
microcode older than this will say "Vulnerable".  This means that the
system is vulnerable to some known CPU issue. It could be security or
functional, the kernel does not know or care.

Update the CPU microcode to mitigate any exposure. This is usually
accomplished by updating the files in /lib/firmware/intel-ucode/
via normal distribution updates. Intel also distributes these files
in a github repo:

	https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files.git

