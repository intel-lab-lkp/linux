.. SPDX-License-Identifier: (GPL-2.0+ OR MIT)

=============================
Firmware files and ABI epochs
=============================

Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

The nova-core driver loads GSP-RM and its boot images from files that
linux-firmware installs. When a firmware change breaks compatibility with
the driver, the files it affects are renamed instead of replaced.

Such a break is a last resort, for a fix that cannot be made within the
existing ABI and cannot wait. In practice that means a critical security fix.

Compatibility
=============

The kernel and linux-firmware are distributed and updated separately, so
the installed kernel and the installed firmware can be from different
releases. Every such pair has to bring up the GPU. An older kernel keeps
requesting the filenames it was built for. A newer kernel loads whatever is
installed, including a firmware release older than itself.

A combination that uses an older kernel or older firmware provides only the
features and fixes that both support, security fixes included. A fix that
requires a firmware ABI change takes effect only after both the kernel and
the firmware are updated.

:doc:`../../../driver-api/firmware/firmware-usage-guidelines` states the
kernel-wide rules for firmware distributed through linux-firmware:

- A newer major version of a file stays compatible with every kernel that
  loads that major version.
- A new major version requires the kernel to keep loading the older major
  versions.

The epoch number defined below is that major version, and adding an epoch
deprecates no existing file.

Directory layout
================

The driver requests paths relative to the firmware search path, and
linux-firmware installs the corresponding files below ``/lib/firmware``.
Each chip has a directory named with the lowercase chip name, and the images
are in a ``gsp`` subdirectory::

    /lib/firmware/nvidia/gb202/gsp/
        fmc.tlv
        gsp.bin -> ../../ga102/gsp/gsp.bin
        gsp_bootloader.tlv
        gsp.tlv
        ucodes.bin
        ucodes.tlv

For a chip that uses another chip's image, linux-firmware installs a link to
that image instead of another copy. The link can be the chip directory
(``gb203 -> gb202``), the ``gsp`` subdirectory
(``ga103/gsp -> ../ga102/gsp``), or a single file. The ``gsp.bin`` payload
above is a link, so one copy of the GSP-RM image serves chips that each have
their own ``gsp.tlv``.

The driver requests these TLV files for every chip:

- ``gsp.tlv``, the GSP-RM image
- ``gsp_bootloader.tlv``, the bootloader that loads GSP-RM
- ``ucodes.tlv``, the microcode images GSP-RM reads at startup

The remaining TLV files depend on how the chip boots the GSP:

- Turing and GA100: ``booter_load.tlv``, ``booter_unload.tlv``,
  ``gen_bootloader.tlv``
- Ampere other than GA100, and Ada: ``booter_load.tlv``,
  ``booter_unload.tlv``
- Hopper and later: ``fmc.tlv``

A TLV file either stores its image in a ``BLOB`` tag or names a companion
file that stores the image. The ``FILE`` tag gives the companion's name and
the ``SIZE`` tag gives its size. A companion file is in the same directory
as the TLV file that names it. ``gsp.bin`` and ``ucodes.bin`` are companion
files. See :doc:`tlv` for the format.

Planned move under nvidia/gpu/
------------------------------

nouveau requests its own GSP-RM images from the same per-chip directory,
under basenames that carry the GSP-RM release:
``nvidia/gb202/gsp/gsp-570.144.bin``, ``bootloader-570.144.bin``, and
``fmc-570.144.bin``. No filename collides today, because nova-core's
basenames differ from nouveau's. Both sets are installed in one directory,
and both kinds of suffix are a dash and a number before the extension:
``-570.144`` is a nouveau release, and ``-1`` is a nova-core epoch.

The planned layout inserts a ``gpu`` component, so nova-core requests
``nvidia/gpu/<chip>/gsp/`` and nouveau keeps requesting
``nvidia/<chip>/gsp/``. nouveau needs no change. Under the planned layout,
nova-core requests paths at the new location, derives companion paths there
from a ``FILE`` tag, and names the new paths in its ``MODULE_FIRMWARE``
entries. linux-firmware installs the nova tree at that location. Everything
else in this document holds under either layout.

ABI breaks
==========

Most firmware releases keep the interface the driver already implements.
Such a release replaces an epoch's files as a complete set under the same
names, stays compatible with every kernel that implements that epoch, and
needs no kernel change.

A firmware release breaks the ABI when a driver built against the previous
release cannot boot the new firmware, or cannot work with it after boot. The
following changes break the ABI:

- removing or redefining a TLV tag that the driver requires
- changing the command and message interface between the driver and GSP-RM
- changing the boot parameter or startup argument structures
- changing the framebuffer regions that the firmware expects to own

Adding a TLV tag does not break the ABI. The parser reads the tags it needs
and ignores the rest.

Renaming for a new ABI epoch
============================

The filenames without a numeric suffix are epoch 0, the only epoch that
exists today. A new epoch renames each file by inserting a dash and the epoch
number before the extension of its epoch 0 name. The numbers count up without
limit::

    /lib/firmware/nvidia/gb202/gsp/
        fmc-1.tlv
        fmc.tlv
        gsp-1.bin
        gsp-1.tlv
        gsp.bin -> ../../ga102/gsp/gsp.bin
        gsp_bootloader-1.tlv
        gsp_bootloader.tlv
        gsp.tlv
        ucodes-1.bin
        ucodes-1.tlv
        ucodes.bin
        ucodes.tlv

Adding an epoch neither modifies nor removes any file from a prior epoch, so
every older epoch stays available under its existing names and a kernel that
predates epoch 1 continues to bring up the GPU.

Every file for an affected chip is rebuilt at the new epoch, including a file
whose contents did not change. The images are interdependent and some of them
are signed, so no file from one epoch may be used with a file from another. A
suffixed name resolves only to files of its own epoch, so ``gsp-1.bin`` never
links to an unsuffixed ``gsp.bin``. A TLV names a companion file from the
same epoch, and the driver reads that filename from the ``FILE`` tag instead
of constructing it.

A ``FILE`` tag names the conventional companion basename for its epoch,
meaning ``gsp-1.bin`` for ``gsp-1.tlv`` and ``ucodes-1.bin`` for
``ucodes-1.tlv``. The driver takes the companion's name from the tag, but the
``MODULE_FIRMWARE`` entries name the conventional basenames, because they are
fixed when the driver is built and cannot name a file that a tag chooses at
run time. A companion under any other name is missing from the module
metadata, and a driver running from an initramfs built from that metadata
cannot load it.

What nova-core requests
=======================

Only epoch 0 exists so far, so nova-core requests the unsuffixed filenames
and has no lower epoch to fall back to. The rule below takes effect once
the driver implements a higher epoch.

The driver requests the highest epoch it implements. If that epoch is not
installed, it requests successively lower epochs down to epoch 0, and uses
the first installed epoch. A kernel that implements epoch 1 boots a GPU
whose firmware is still at epoch 0. Refusing to load epoch 0 would not
install epoch 1 firmware, and the GPU would not come up.

Firmware from a newer epoch has no effect on nova-core, which never requests
an epoch it does not implement. The driver does not scan the firmware
directory, and the set of epochs it can name is fixed when it is built.

The driver finds the epoch by requesting the GSP-RM TLV that every chip has:
``gsp.tlv`` at epoch 0, ``gsp-1.tlv`` at epoch 1, and one name per epoch
above that. The driver falls back to the next lower epoch only when that
request returns ``-ENOENT``. These requests use ``firmware_request_nowarn``,
so a system with epoch 0 firmware installed logs nothing for the higher
epochs the driver tried first.

The epoch of the GSP-RM TLV sets the epoch of every remaining file for that
chip, and nova-core requests each one by its exact name in that epoch. A file
that is missing, malformed, or rejected fails the probe instead of falling
back to a lower epoch. For a missing file, ``request_firmware`` logs the path
it could not open::

    Direct firmware load for nvidia/gb202/gsp/fmc-1.tlv failed with error -2

The driver logs the epoch it loaded. When that epoch is lower than the
highest one the driver implements, the message is a warning: the installed
firmware lacks the fix that each newer epoch was created for.

The ``MODULE_FIRMWARE`` entries name every file of every epoch nova-core can
load, because the driver cannot know at build time which epoch it will find
installed.

Cost of an epoch
================

The driver keeps a code path for every epoch it can load, and each of those
paths has to stay tested. A new epoch adds an ABI implementation to the
driver, a set of firmware files to linux-firmware, and the matching module
firmware metadata.

linux-firmware keeps every epoch's files because older kernels request them,
so each retained epoch increases the size of the installed firmware tree. The
links limit what an epoch adds: one copy of each payload that changed, plus a
TLV for each affected chip. A distribution that includes nova-core in the
initramfs for early-boot display copies the firmware named in the module
metadata, so each epoch increases the initramfs size.

Retiring an epoch
=================

A kernel needs an epoch when it implements no higher one, because it can load
nothing else. Every newer kernel finds a higher epoch installed and uses it
instead. An epoch's files stay in linux-firmware while any supported kernel
still needs them. The epoch becomes eligible for removal once every kernel
that needs it has reached end of life, and the longest-supported enterprise
kernels set that date, a decade or more after the epoch was added.

Removal is proposed on nova-gpu@lists.linux.dev, naming the epoch, the
kernel releases that need it, and the end-of-life date of each. The driver's
code path for that epoch goes in the same proposal, because a system whose
firmware was never updated is still at that epoch.
