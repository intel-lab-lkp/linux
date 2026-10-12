.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

==========================================
NVIDIA LSTP USB Multi-Function Device ACPI
==========================================

This is the ACPI spelling of the description in
``Documentation/devicetree/bindings/usb/nvidia,lstp.yaml``.  The driver reads both through
the same fwnode property accessors, so the properties, their names and
their meanings are identical.  What differs is the syntax, the placement
in the namespace, and the address of an interface or channel, which ACPI
gives as ``_ADR`` where device tree has ``reg``.  Read the device tree
binding for what the properties mean, and this document for how to write
them in ASL.

Static description is optional.  Channels are discovered from firmware at
probe time and work with no ACPI or device tree description at all.  It is
needed only to attach properties to a channel (GPIO line names, LED names)
or to declare devices sitting behind an I2C or SPI channel.


Where the objects go
====================

An LSTP channel object is found by walking down from the USB device's ACPI
companion.  For a USB device on a port, that companion is the *port*
object, not an object of its own: ``usb_acpi_find_companion_for_device()``
in ``drivers/usb/core/usb-acpi.c`` returns the companion of the port the
device is attached to, which ``acpi_find_child_by_adr()`` matches by
``_ADR``.

So the interface objects are children of the port object.  This is the
namespace for a device at ``1-1.3:1.2`` -- interface 2 of the device on
port 3 of a hub attached to root-hub port 1, the same device the device
tree binding example uses::

  Device (XHCI)                    // host controller
    Device (RHUB)                  //   root hub, _ADR = 0
      Device (PRT1)                //     root-hub port 1, _ADR = raw port
        Device (PRT3)              //       hub port 3, _ADR = 3
          Device (IFC2)            //         interface, _ADR = 2
            Device (CHN1)          //           channel, _ADR = 1
            Device (CHN2)          //           channel, _ADR = 2

Interface and channel objects are identified by ``_ADR``, as the port
objects above them are: an interface object's ``_ADR`` is its
``bInterfaceNumber``, and a channel object's is its channel ID.  Do not use
``_HID`` instead -- that would advertise the object for driver binding,
which is not what these are.

Earlier versions of this driver ignored ``_ADR`` on these objects and
matched them on a ``reg`` property in their ``_DSD`` instead, with
``_ADR`` set to the same value only so the table compiles.  Tables
written to that rule keep working unchanged, since their ``_ADR`` is the
address.  The driver does not read ``reg`` on an ACPI object, so a table
that also has to work with those earlier versions may keep it, set to the
same value as ``_ADR``.  An object whose ``_ADR`` and ``reg`` differ was
matched on ``reg`` by those versions and is matched on ``_ADR`` now.

An LSTP device plugged straight into a root-hub port has one level less:
the interface objects hang off that port object directly.  The nesting
above exists because the device is behind an external hub, and each hub
level gets a port object of its own.

This is the one structural difference from device tree, where the USB
device has a node of its own between the hub and the interface node.  The
device-level ``compatible = "usb955,cf11"`` has no ACPI counterpart
because nothing in the match path reads it; the device is identified by
USB descriptors either way.  For the same reason the device tree's
combined node does not apply here: a device with a single interface still
gets an interface object, with ``_ADR`` = 0.

Object names (``IFC2``, ``CHN1``) are arbitrary.  Only ``_ADR`` and the
``_DSD`` properties below take part in matching.


Properties
==========

Interface object
  ``_ADR``
    ``bInterfaceNumber`` of the USB interface carrying LSTP traffic.
    Required.

Channel object
  ``_ADR``
    Firmware-reported channel ID, as ``reg`` is in the device tree
    binding.  This is the channel ID the device reports, which is not the
    auxiliary device instance suffix.  Required.

  ``compatible``, in ``_DSD``
    One of ``nvidia,lstp-spi``, ``nvidia,lstp-i2c``, ``nvidia,lstp-gpio``,
    ``nvidia,lstp-ipmi``, ``nvidia,lstp-uart``.  Required; a channel
    object is matched on ``_ADR`` and ``compatible`` together, so a
    mismatched ``compatible`` leaves the channel with no firmware node
    rather than binding it to the wrong driver.

  Per-channel properties are named exactly as in the device tree binding
  -- ``gpio-line-names`` and so on.

``_DSD`` properties use the device-properties UUID
``daffd814-6eba-4d8c-8a91-bc9bbf4aa301``.  An interface object needs no
``_DSD`` at all.

A single ``_DSD`` string satisfies a ``compatible`` match: reading a
string property as an array returns a count of one for a lone string
(``acpi_data_prop_read_single()``, ``drivers/acpi/property.c``), so there
is no need to wrap it in a package.


Use Device objects, not _DSD data nodes
=======================================

Interfaces and channels must be real ``Device()`` objects.  ACPI also
allows a hierarchy of *data* nodes under a single ``_DSD``, and the
driver's walk returns those too: it goes through the
``get_next_child_node`` operation, which for ACPI is
``acpi_get_next_subnode()``, or on newer kernels
``acpi_get_next_present_subnode()``, and returns child ``Device()``
objects and data nodes alike.  A data node has no ``_ADR``, so it never
matches an interface or a channel, whatever properties it carries.  The
driver warns at probe about each data node among the interface object's
children, and among the port object's children when no interface object
matches, so that the table's mistake is visible rather than inferred from
an undescribed channel.  Earlier versions of this driver matched data
nodes on their ``reg``.

The walk is worth knowing for a second reason.  On kernels whose walk is
``acpi_get_next_present_subnode()``, any child ``Device()`` whose ``_STA``
reports the device absent is skipped silently, so a channel can also go
missing because its object is present in the table but evaluates as not
present.  On older kernels such an object is matched like any other.

Even matched, a data node could not work, because it is not a device and
so has no ACPI companion, and the cores that enumerate on this driver's
behalf all gate on having one:

- ``i2c_acpi_register_devices()`` returns early unless
  ``has_acpi_companion(&adap->dev)``, which is just
  ``is_acpi_device_node(dev->fwnode)``.
- ``acpi_register_spi_devices()`` returns early unless
  ``ACPI_HANDLE(ctlr->dev.parent)`` is set.
- ``acpi_gpiochip_add()`` returns early unless
  ``ACPI_COMPANION(chip->parent)`` is set, so the chip is never registered
  with the ACPI GPIO layer.  A consumer that names this controller then
  gets ``-EPROBE_DEFER`` from ``acpi_get_gpiod()`` -- which is meant for a
  controller that has not probed yet, so here it never resolves and the
  consumer defers forever.

Nothing would be enumerated, and none of the three cores would report an
error.


Devices behind a channel
========================

The I2C and SPI cores enumerate by walking the namespace from the root and
matching each candidate's connection resource against this controller,
rather than by reading children of the channel object.  A device on the
bus may therefore live anywhere in the namespace; putting it under its
channel is convention, not a requirement.

I2C
  Give each device its own ``Device()`` with an ``I2cSerialBus()`` resource
  whose ``ResourceSource`` is the full path of the channel object.

SPI
  Give each device its own ``Device()`` with a ``SpiSerialBus()`` resource
  whose ``ResourceSource`` is the full path of the channel object.  The
  channel object is what the SPI core looks at, because the controller's
  parent is the channel's auxiliary device and carries that node.

GPIO
  There is nothing to enumerate, but the relationship runs the other way:
  a consumer elsewhere in the namespace names this channel as its GPIO
  provider, so the channel object still has to be resolvable.  Reference it
  by path from the consumer's ``_CRS``::

      Device (LEDX)
      {
          Name (_HID, "PRP0001")
          Name (_CRS, ResourceTemplate ()
          {
              GpioIo (Exclusive, PullNone, 0, 0, IoRestrictionOutputOnly,
                      "\\_SB.PCI0.XHCI.RHUB.PRT1.PRT3.IFC2.CHN3", 0,
                      ResourceConsumer, ,)
                      { 0 }                  // pin, controller-relative
          })
          Name (_DSD, Package ()
          {
              ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
              Package () { Package () { "compatible", "gpio-leds" } }
          })
      }

  The pin number is controller-relative, matching the channel's own pin
  numbering.  ``PRP0001`` only enumerates a device whose ``_DSD`` carries a
  ``compatible``; without one the object is scanned, matched and then does
  nothing.  A consumer with a real ``_HID`` needs no ``_DSD`` -- only the
  ``_CRS`` reference matters here.

  The ACPI GPIO layer does not link a consumer to the controller it
  resolved.  When the LSTP device is unbound, reset or re-enumerated, a
  consumer that is bound at that moment keeps its lines on the removed
  controller: its GPIO accesses no longer reach the device (reads fail
  with ``-ENODEV``, or return 0 on older kernels, and writes are dropped),
  it is not probed again when the channel comes back, and it keeps the
  gpio-lstp module in use.  On older kernels, unbinding the consumer also logs
  a warning for each line it held.  Unbind and bind the consumer again
  once the LSTP device is back, for example from a udev rule on the
  channel's auxiliary device.

  Pins the device firmware reports as LEDs take their LED properties from
  a ``leds`` child with one ``led@N`` child per pin, as in the device tree
  binding.  These are only read, never enumerated, so here ``_DSD`` data
  nodes are the right spelling, as in
  ``Documentation/firmware-guide/acpi/dsd/leds.rst``::

      Device (CHN3)
      {
          Name (_ADR, 3)
          Name (_DSD, Package ()
          {
              ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
              Package ()
              {
                  Package () { "compatible", "nvidia,lstp-gpio" }
              },
              ToUUID ("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
              Package () { Package () { "leds", LEDS } }
          })
          Name (LEDS, Package ()
          {
              ToUUID ("dbb8e3e6-5886-4ba6-8795-1319f52a966b"),
              Package () { Package () { "led@4", LED4 } }
          })
          Name (LED4, Package ()
          {
              ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
              Package ()
              {
                  Package () { "reg", 4 },
                  Package () { "label", "amber:status" },
                  Package () { "default-state", "on" }
              }
          })
      }

IPMI, UART
  Nothing to enumerate, nothing to reference, and no properties beyond
  ``compatible``.  An object for one of these channels
  describes it but changes nothing about how it comes up.


Example
=======

The device at ``1-1.3:1.2`` -- interface 2 of an LSTP device on port 3 of a
hub attached to root-hub port 1 -- with five channels, an I2C temperature
sensor on channel 2 and a SPI device on channel 1::

  Scope (\_SB.PCI0.XHCI.RHUB.PRT1)         // root-hub port 1, _ADR = raw port
  {
      Device (PRT3)
      {
          Name (_ADR, 3)                       // port on the external hub

          Device (IFC2)
          {
              Name (_ADR, 2)                   // bInterfaceNumber

              Device (CHN1)                    // SPI channel
              {
                  Name (_ADR, 1)                   // channel ID
                  Name (_DSD, Package ()
                  {
                      ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                      Package ()
                      {
                          Package () { "compatible", "nvidia,lstp-spi" }
                      }
                  })

                  Device (FLS0)                // flash on chip select 0
                  {
                      Name (_HID, "PRP0001")
                      Name (_CRS, ResourceTemplate ()
                      {
                          SpiSerialBus (0,               // chip select
                              PolarityLow, FourWireMode, 8,
                              ControllerInitiated, 24000000,
                              ClockPolarityLow, ClockPhaseFirst,
                              "\\_SB.PCI0.XHCI.RHUB.PRT1.PRT3.IFC2.CHN1")
                      })
                      Name (_DSD, Package ()
                      {
                          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                          Package ()
                          {
                              Package () { "compatible", "jedec,spi-nor" }
                          }
                      })
                  }
              }

              Device (CHN2)                    // I2C channel
              {
                  Name (_ADR, 2)                   // channel ID
                  Name (_DSD, Package ()
                  {
                      ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                      Package ()
                      {
                          Package () { "compatible", "nvidia,lstp-i2c" }
                      }
                  })

                  Device (TMP0)                // sensor at 0x48
                  {
                      Name (_HID, "PRP0001")
                      Name (_CRS, ResourceTemplate ()
                      {
                          I2cSerialBus (0x0048, ControllerInitiated,
                              400000, AddressingMode7Bit,
                              "\\_SB.PCI0.XHCI.RHUB.PRT1.PRT3.IFC2.CHN2",
                              0x00, ResourceConsumer,,)
                      })
                      Name (_DSD, Package ()
                      {
                          ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                          Package ()
                          {
                              Package () { "compatible", "ti,tmp102" }
                          }
                      })
                  }
              }

              Device (CHN3)                    // GPIO channel
              {
                  Name (_ADR, 3)                   // channel ID
                  Name (_DSD, Package ()
                  {
                      ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                      Package ()
                      {
                          Package () { "compatible", "nvidia,lstp-gpio" },
                          Package () { "gpio-line-names",
                                       Package () { "LED", "INT" } }
                      }
                  })
              }

              Device (CHN4)                    // IPMI channel
              {
                  Name (_ADR, 4)                   // channel ID
                  Name (_DSD, Package ()
                  {
                      ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                      Package ()
                      {
                          Package () { "compatible", "nvidia,lstp-ipmi" }
                      }
                  })
              }

              Device (CHN5)                    // UART channel
              {
                  Name (_ADR, 5)                   // channel ID
                  Name (_DSD, Package ()
                  {
                      ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                      Package ()
                      {
                          Package () { "compatible", "nvidia,lstp-uart" }
                      }
                  })
              }
          }
      }
  }

``_HID`` of ``PRP0001`` with a ``compatible`` property is the generic way
to bind an ACPI device to a driver that has only a device tree match
table; use the device's own ``_HID`` instead where it has one.


Locating the port object
========================

Each hub level in the sysfs path gets a port object.  For the device at
``1-1.3:1.2`` used above:

==============  =====================================  ==================
sysfs segment   ACPI object                            ``_ADR``
==============  =====================================  ==================
``1-``          host controller                        platform's own
``1-``          root hub, child of the controller      0
``1-1``         root-hub port 1                        raw port number
``1-1.3``       port 3 on the hub at port 1            3
``1-1.3:1.2``   interface 2                            2
==============  =====================================  ==================

The two port rows are matched differently, which matters when writing the
table.  A port on an external hub uses the port number as sysfs shows it.
A **root-hub** port does not: ``usb_acpi_get_companion_for_port()`` passes
the port through ``usb_hcd_find_raw_port_number()`` first, and on xHCI that
resolves to the controller's hardware port number
(``rhub->ports[port1 - 1]->hw_portnum + 1``), which is generally not the
sysfs number because the USB2 and USB3 root hubs are numbered separately
over one shared hardware port array.  Controllers that do not implement
``find_raw_port_number`` leave the number unchanged.  Read the hardware
port number out of the platform's own table rather than counting sysfs
segments.

The interface row is no port at all: its ``_ADR`` is the
``bInterfaceNumber``, which the USB core does not look up.  This driver
matches it among the children of the device's port object.
