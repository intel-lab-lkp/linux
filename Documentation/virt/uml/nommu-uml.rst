.. SPDX-License-Identifier: GPL-2.0

UML has been built with CONFIG_MMU since day 0.  The patchset
introduces the nommu mode on UML in a different angle from what Linux
Kernel Library tried.

.. contents:: :local:

What is it for ?
================

- Alleviate syscall hook overhead implemented with ptrace(2)
- To exercises nommu code over UML (and over KUnit)
- Less dependency to host facilities


How it works ?
==============

To illustrate how this feature works, the below shows how syscalls are
called under nommu/UML environment.

- boot kernel, setup zpoline trampoline code (detailed later) at address 0x0
- (userspace starts)
- calls vfork/execve syscalls
- during execve, more specifically during load_elf_fdpic_binary()
  function, kernel translates `syscall/sysenter` instructions with `call
  *%rax`, which usually point to address 0 to NR_syscalls (around
  512), where trampoline code was installed during startup.
- when syscalls are issued by userspace, it jumps to `*%rax`, slides
  until `nop` instructions end, and jump to hooked function,
  `__kernel_vsyscall`, which is an entrypoint for syscall under nommu
  UML environment.
- call handler function in sys_call_table[] and follow how UML syscall
  works.
- return to userspace


What are the differences from MMU-full UML ?
============================================

The current nommu implementation adds 3 different functions which
MMU-full UML doesn't have:

- kernel address space can directly be accessible from userspace
  - so, uaccess() always returns 1
  - generic implementation of memcpy/strcpy/futex is also used
- alternate syscall entrypoint without ptrace
- translation of syscall/sysenter instructions to a trampoline code
  and syscall hooks

With those modifications, it allows us to use unmodified userspace
binaries with nommu UML.


History
=======

This feature was originally introduced by Ricardo Koller at Open
Source Summit NA 2020, then integrated with the syscall translation
functionality with the clean up to the original code.

Building and run
================

```
% make ARCH=um x86_64_nommu_defconfig
% make ARCH=um
```

will build UML with CONFIG_MMU=n applied.

Kunit tests can run with the following command:

```
% ./tools/testing/kunit/kunit.py run --kconfig_add CONFIG_MMU=n
```

To run a typical Linux distribution, we need nommu-aware userspace.
We can use a stock version of Alpine Linux with nommu-built version of
busybox and musl-libc.


Preparing root filesystem
=========================

nommu UML requires to use a specific standard library which is aware
of nommu kernel.  We have tested custom-build musl-libc and busybox,
both of which have built-in support for nommu kernels.

There are no available Linux distributions for nommu under x86_64
architecture, so we need to prepare our own image for the root
filesystem.  We use Alpine Linux as a base distribution and replace
busybox and musl-libc on top of that.  The following are the step to
prepare the filesystem for the quick start.

```
     container_id=$(docker create ghcr.io/thehajime/alpine:3.20.3-um-nommu)
     docker start $container_id
     docker wait $container_id
     docker export $container_id > alpine.tar
     docker rm $container_id

     mnt=$(mktemp -d)
     dd if=/dev/zero of=alpine.ext4 bs=1 count=0 seek=1G
     sudo chmod og+wr "alpine.ext4"
     yes 2>/dev/null | mkfs.ext4 "alpine.ext4" || true
     sudo mount "alpine.ext4" $mnt
     sudo tar -xf alpine.tar -C $mnt
     sudo umount $mnt
```

This will create a file image, `alpine.ext4`, which contains busybox
and musl with nommu build on the Alpine Linux root filesystem.  The
file can be specified to the argument `ubd0=` to the UML command line.

```
  ./vmlinux eth0=tuntap,tap100,0e:fd:0:0:0:1,172.17.0.1 ubd0=./alpine.ext4 rw mem=1024m loglevel=8 init=/sbin/init
```

We plan to upstream apk packages for busybox and musl so that we can
follow the proper procedure to set up the root filesystem.


Quick start with docker
=======================

There is a docker image that you can quickly start with a simple step.

```
  docker run -it -v /dev/shm:/dev/shm --rm ghcr.io/thehajime/alpine:3.20.3-um-nommu
```

This will launch a UML instance with an pre-configured root filesystem.

Benchmark
=========

The below shows an example of performance measurement conducted with
lmbench and (self-crafted) getpid benchmark (with v6.12-rc2 uml/next
tree).

### lmbench (usec)

||native|um|um-nommu|
|--|--|--|--|
|select-10    |0.5644|31.0917|0.2743|
|select-100   |2.3869|31.4651|1.1472|
|select-1000  |20.4004|36.4966|9.7533|
|syscall      |0.1733|25.9904|0.1053|
|read         |0.3438|27.4873|0.1451|
|write        |0.2862|25.8794|0.1361|
|stat         |1.9250|37.5072|0.4532|
|open/close   |3.8961|65.1736|0.7665|
|fork+sh      |1173.8889|5404.5000|20577.0000|
|fork+execve  |535.2105|2179.2000|4716.3333|

### do_getpid bench (nsec)

||native|um|um-nommu|
|--|--|--|--|
|getpid | 172 | 25602 | 103|


Limitations
===========

generic nommu limitations
-------------------------
Since this port is a kernel of nommu architecture so, the
implementation inherits the characteristics of other nommu kernels
(riscv, arm, etc), described below.

- vfork(2) should be used instead of fork(2)
- ELF loader only loads PIE (position independent executable) binaries
- processes share the address space among others
- mmap(2) offers a subset of functionalities (e.g., unsupported
  MMAP_FIXED)

Thus, we have limited options to userspace programs.  We have tested
Alpine Linux with musl-libc, which has a support nommu kernel.

access to mmap_min_addr
----------------------
As the mechanism of syscall translations relies on an ability to
write/read memory address zero (0x0), we need to configure host kernel
with the following command:

```
% sh -c "echo 0 > /proc/sys/vm/mmap_min_addr"
```

supported architecture
----------------------
The current implementation of nommu UML only works on x86_64 SUBARCH.
We have not tested with 32-bit environment.

target of syscall translation
-----------------------------
The syscall translation only applies to the executable and interpreter
of ELF binary files which are processed by execve(2) syscall for the
moment: other libraries such as linked library and dlopen-ed one
aren't translated; we may be able to trigger the translation by
LD_PRELOAD.  JIT compiler generated code is also generated after execve
thus, it is not currently translated.

Note that with musl-libc in Alpine Linux which we've been tested, most
of syscalls are implemented in the interpreter file
(ld-musl-x86_64.so) and calling syscall/sysenter instructions from the
linked/loaded libraries might be rare.  But it is definitely possible
so, a workaround with LD_PRELOAD is effective.


Further readings about NOMMU UML
================================

- NOMMU UML (original code by Ricardo Koller)
https://static.sched.com/hosted_files/ossna2020/ec/kollerr_linux_um_nommu.pdf

- zpoline: syscall translation mechanism
https://www.usenix.org/conference/atc23/presentation/yasukata
