.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

==================================
NVIDIA LSTP USB channel framework
==================================

The LSTP core driver binds the USB interface of an LSTP device, reads the
channel descriptors through channel 0, and offers each channel of a supported
type to the auxiliary bus. A subsystem driver (GPIO, I2C, SPI, UART, ...)
binds that auxiliary device and does its I/O through the helpers declared in
``include/linux/usb/lstp.h``.

The core is the ``lstp`` module. Each subsystem driver is a module of its own
on the auxiliary bus: ``gpio-lstp``, ``i2c-lstp``, ``spi-lstp``, ``lstp_tty``
and ``ipmi-lstp`` bind the ``lstp.<type>.<n>`` devices the core adds. A GPIO
channel's LED pins are a sub-device, ``lstp.led.<n>``, which ``gpio-lstp``
adds and owns below its own device and ``leds-lstp`` binds. Each module loads
on demand from its devices' modaliases.

This document describes three rules those drivers rely on and the code
comments point to: how long a channel lives, when its I/O is refused, and how
it gets a firmware node. It ends with the limits of the packet handling.


Channel lifetime and pins
=========================

Each channel of a supported type lives in a private container,
``struct lstp_channel_priv``, that embeds the channel's auxiliary device. The
reference count of that device owns the container, which
``lstp_channel_dev_release()`` frees on the last put. Channel 0 and channels
of a type the driver does not implement never get an initialised auxiliary
device (``LSTP_AUXDEV_RAW``); they are freed when the USB interface is
unbound.

A subsystem whose state can outlive the bind behind a userspace handle (an
open file descriptor, a queued tty) pins the channel with
lstp_channel_get() when it creates that state, and drops the pin with
lstp_channel_put() when it frees it, from its own code. While a pin is held
the container, and with it the fence the I/O helpers test, stays valid after
the USB device is gone and the subsystem driver has been unbound: the channel
can still be inspected, and every lstp_send() and lstp_request*() on it fails
with ``-ENODEV`` rather than touching freed state.

A pin does not keep I/O refused. An unbind raises the fence only until the
next bind lowers it, so state that outlives its bind refuses I/O through a
per-bind flag of its own, raised at teardown and never cleared. That state is
reached through the handle, never through ``lstp_channel.priv``, which the
core clears when the driver unbinds.

Only a channel offered to a subsystem can be pinned: lstp_channel_get()
returns ``NULL`` for a ``LSTP_AUXDEV_RAW`` channel, whose embedded device was
never initialised, and lstp_channel_put() ignores one the same way; both
warn once.


The I/O fence
=============

A channel is fenced while either of two flags in its container is set;
lstp_channel_disconnected() reports it. A fenced channel refuses lstp_send()
and lstp_request*() with ``-ENODEV`` under the channel's TX mutex, wakes
response waiters with ``-ENODEV``, and refuses a bind in ``lstp_aux_probe()``.
The fence frees nothing: a pinned container stays valid.

``gone``
  The permanent half: the device is not coming back. Raised by
  ``lstp_channel_mark_gone()``, from the teardown path and from RX completion
  (atomic) once the URB reports the device or HCD away. Never cleared.

``unbound``
  The half a bind clears, so an unbind and rebind resume I/O on a device still
  present. Raised by ``lstp_channel_mark_unbound()``, from the channel's
  devres unwind, or from the bus notifier's BIND arm when it could not book
  that unwind. Cleared by the BIND arm, the edge before ``.probe()``, so a
  probe can reach its own channel. False from creation until the first bind,
  which keeps channel 0 usable for discovery config reads: channel 0 is never
  offered to the bus, so it never binds.

The two are separate so that a bind edge cannot un-fence a channel whose
device has already departed. Both are stored with ``smp_store_release()``;
lstp_channel_disconnected() reads them with the paired ``smp_load_acquire()``.

Unbind
------

The BIND arm books ``lstp_channel_quiesce_unbound()`` as devres on the
channel's auxiliary device before ``.probe()``, so it lands at the tail of the
driver's own unwind on every edge. No notifier arm can do it: the driver core
reports ``BUS_NOTIFY_DRIVER_NOT_BOUND`` before ``device_unbind_cleanup()`` and
``BUS_NOTIFY_UNBOUND_DRIVER`` after it, so a failed probe would be fenced
mid-release, and a release that does LSTP I/O, as led_classdev_unregister()
does when it writes ``LED_OFF``, would get ``-ENODEV``.

``lstp_channel_quiesce_unbound()`` raises ``unbound``, takes the TX mutex so
that a request that passed the fence finishes, and kills both TX URBs under
it. If ``gone`` is already up it kills nothing and leaves the URBs to
interface teardown, which frees them without the channel device's lock: an
unbind on the auxiliary bus does not take the USB interface's lock, so the
two can run at once. The TX mutex orders the two sides, because teardown
raises ``gone`` before it takes each channel's TX mutex and frees the URBs
only after that. The TX mutex covers only the request URB as a submitter.
The IRQ response URB has two submitters: the ``irq_packet`` upcall, which
cannot run by then (UNBIND retracted dispatch, BOUND never published it, or
interface teardown poisoned RX), and the core's ``LSTP_NACK`` from the RX
completion, which can. A NACK that meets the kill fails, and one sent after
it completes on its own; the URB and its buffer are interface devres, so
teardown kills the URB before freeing either. The URBs are killed and not
poisoned: poison rejects submissions for the life of the URB and would leave
the channel dead across a rebind.

A request the device sends while no driver takes it (none bound yet, or
dispatch retracted) is dropped. On a UART channel the device waits for the
answer to each such write, so the core answers it with ``LSTP_NACK`` and the
device retries; it does so only once the channel's auxiliary device has been
added, when its response slot exists, and not once the USB device is gone.
GPIO and IPMI events take no response. So while no driver is bound to a UART
channel (before ``lstp_tty`` loads, or after it is removed or unbound), every
write the device sends is NACKed and counted in the channel's ``rx_dropped``.
The device's retry pacing, which the specification leaves open, sets the
rate.

Two teardowns are not reported by the bus:

- Under ``CONFIG_DEBUG_TEST_DRIVER_REMOVE``, ``really_probe()`` runs
  ``.remove()`` and ``device_unbind_cleanup()`` after a successful probe and
  then re-probes, emitting no BOUND, UNBIND or UNBOUND. The devres fence fires
  in that unwind and the second BIND lifts it.

- Interface teardown unregisters the notifier before the channel that
  registered it is deleted, so that channel's delete reports nothing. RX is
  already poisoned, and nothing reads the channel's ``priv`` after the delete.

Disconnect
----------

``lstp_teardown()`` runs on USB disconnect and on the probe error path, since
the USB core does not call ``.disconnect()`` after a failed probe. It stops RX
before any channel driver's ``.remove()`` can run, raises ``gone`` on every
channel, takes and drops each channel's TX mutex so that a caller
mid-submission finishes, and poisons the TX URBs.

That drain only covers callers already inside the framework. Subsystem
teardown runs later, against a channel whose framework buffers are already
freed: they are interface devres nodes registered after
``lstp_channel_destroy_action()``, so they unwind first. What keeps a
teardown-time write off a freed buffer is the fence raised here, which the
public I/O helpers test under the TX mutex and refuse on. A teardown step that
reaches channel state without them has no such protection.

The TX URB of a fenced request
------------------------------

Outside the fence, every request retires its TX URB before returning, even on
success. Nothing on the wire ties a response to its request, so a response
left over from a request that already timed out is taken for this one's
answer, and it can arrive before this request's own transfer completes, since
the device has not seen it yet. The next request reuses the URB and refills
the TX buffer, so the URB has to retire first: otherwise ``usb_submit_urb()``
refuses it as still active, and the refill rewrites a buffer still on the
wire.

On a fenced channel the URB is deliberately left queued, whether the request
timed out or completed: ``usb_kill_urb()`` blocks until the URB retires and
can be reached holding a lock teardown waits on (gpiolib's SRCU section),
which wedges teardown under load. ``lstp_channel_quiesce_unbound()`` reclaims
it at unbind, and once ``gone`` is up it yields to interface teardown, where
``lstp_stop_tx()`` and the devres release of the URB reclaim it. The test is
the fence and not whether the USB device is gone: the fence is also what
stops the next request from resubmitting the URB, and the device already
reads as gone before ``.disconnect()`` raises the fence.


Firmware nodes
==============

A channel's firmware node is optional. Channels are discovered from the
device at probe time; a node only attaches properties to a channel or
describes devices behind it. Channel nodes are children of a per-interface
node, which ``lstp_find_intf_fwnode()`` resolves once per probe;
``lstp_init_channel_fwnode()`` then matches each channel among its children
on its address (the channel ID) and ``compatible``. The address is ``reg`` on
device tree and software nodes and ``_ADR`` on ACPI
(``lstp_fwnode_read_address()``). The fwnode property accessors read device
tree, ACPI and software nodes alike. A channel that firmware does not
describe gets no node and comes up with no firmware properties.

The bindings are in ``Documentation/devicetree/bindings/usb/nvidia,lstp.yaml``; the ACPI
spelling is in ``Documentation/firmware-guide/acpi/lstp.rst``.

The interface node
------------------

Device tree
  The node the USB core attached to the interface in
  ``usb_set_configuration()``: an ``interface@N`` node with a two-cell
  ``reg``, or the device node itself on a device with a combined node. It is
  taken as the core resolved it rather than re-matched, so the driver sees
  exactly what every other consumer of the interface's ``of_node`` sees.

  Two layouts leave every channel of the interface silently undescribed, and
  ``lstp_check_of_layout()`` warns about both. An interface node with a
  one-cell ``reg``: ``usb_of_get_interface_node()`` reads two cells, so the
  core attaches nothing to the interface. And channels wrapped in an interface
  node on a device with a combined node (``usb_of_has_combined_node()``): the
  core hands the interface the device node itself, so channel nodes belong
  directly under it.

ACPI and software nodes
  The interface has no companion of its own, since the core hands it the
  device's (for ACPI, the port object), so the interface node is the child of
  that node whose address is ``bInterfaceNumber``.

  On ACPI the address is the full 64-bit ``_ADR`` of a ``Device()``, read
  with ``acpi_get_local_u64_address()`` and compared with the interface
  number or channel ID as it is; ``reg`` is not read. A ``_DSD`` data node
  has no ``_ADR`` and never matches. ``lstp_check_acpi_data_node()`` warns
  once per probe about each data node among the interface object's children,
  and among the port object's children when no interface object matched.

Channel nodes
-------------

On device tree every child is considered, disabled or not, so that a channel
whose node is marked ``status = "disabled"`` is found and then not created.
``fwnode_for_each_child_node()`` would skip it
(``of_get_next_available_child()``), and the channel would come up
undescribed instead. A disabled node means the channel is not created at all,
as for an MFD cell or a USB interface. This is logged at info level, as the
USB core logs a disabled interface, because the channel is otherwise simply
absent.

Everywhere else the generic fwnode iterator walks ACPI and software nodes.
On ACPI it returns ``_DSD`` data nodes as well as ``Device()`` objects, and
a data node never matches. Where the ACPI walk is
``acpi_get_next_present_subnode()``, it also skips a ``Device()`` whose
``_STA`` reports it absent.

No match is both the ordinary answer and what a wrong address or a misspelled
``compatible`` produce (and, where the ACPI walk filters on ``_STA``, an
absent device), so the outcome is only logged at debug level.

A ``_DSD`` data node could not stand in for a channel even if it matched: it
has no ACPI companion, so ACPI-enumerated children and ACPI references to the
channel would never resolve.

Reference ownership
-------------------

The match is stored in ``lstp_channel.fwnode``, which the core hands to the
channel's auxiliary device with ``device_set_node()`` before adding it. That
takes no reference of its own: the channel owns the single reference and
drops it in ``lstp_channel_free()``. A subsystem that points another device
at the node must not put it.


Limitations
===========

LSTP Specification 1.0 lets a device pack several LSTP packets into one USB
transfer, spread one packet over several, and describe more than 255 GPIOs
with further config blocks. The drivers support none of these. The core takes
one packet from each transfer and ignores the bytes after its payload, and
rejects a packet that claims more payload than one transfer holds;
``gpio-lstp`` reads a channel's first config block only, so at most 255 GPIOs.
The host's own requests are sized so that each response fits one transfer.


Subsystem driver API
====================

.. kernel-doc:: include/linux/usb/lstp.h

.. kernel-doc:: drivers/usb/misc/lstp.c
   :export:
