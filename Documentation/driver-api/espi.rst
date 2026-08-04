.. SPDX-License-Identifier: GPL-2.0-or-later

===========================================
eSPI (Enhanced Serial Peripheral Interface)
===========================================

Introduction
============

eSPI is a bus defined by Intel that replaces the legacy LPC bus. Unlike
SPI it is a structured, capability-negotiated, message-oriented protocol
with four logically independent channels (Peripheral, Virtual Wire, OOB,
Flash) over a shared physical link, and asynchronous target-to-controller
events, so it is modelled as its own bus type rather than an extension of
the SPI subsystem.

Architecture
============

* ``struct espi_controller`` - host controller, created with
  espi_controller_alloc() and registered with espi_controller_register().
  It is not itself a device on espi_bus_type.
* ``struct espi_device`` - a target on the bus, matched to a
  ``struct espi_driver`` via its modalias.
* ``struct espi_controller_ops`` - the optional hardware-op table; the
  channel API returns -EOPNOTSUPP for ops a controller does not provide.

Writing a controller driver
===========================

A controller driver allocates and registers a controller from its
``probe()`` function::

    ctrl = espi_controller_alloc(&pdev->dev, sizeof(*priv));
    if (IS_ERR(ctrl))
        return PTR_ERR(ctrl);

    priv = espi_controller_get_devdata(ctrl);
    ctrl->ops = &my_espi_ops;
    ctrl->max_targets = 1;

    /* populate ctrl->caps from hardware capability registers */
    ctrl->caps.supported_channels = ESPI_CHANNEL_ALL;
    ctrl->caps.max_freq_mhz       = 33;
    ctrl->caps.io_mode            = ESPI_IO_MODE_SINGLE;

    ret = espi_controller_register(ctrl);
    if (ret)
        goto err_put;

After registration the controller calls espi_new_device() for each
target enumerated from firmware (ACPI or device tree)::

    struct espi_board_info info = {
        .type = "my-ec",
        .cs   = 0,
    };
    edev = espi_new_device(ctrl, &info);

On removal::

    espi_remove_device(edev);
    espi_controller_unregister(ctrl);
    espi_controller_put(ctrl);

Writing a slave driver
======================

A slave driver declares a device ID table and a ``struct espi_driver``::

    static const struct espi_device_id my_ec_ids[] = {
        { "my-ec", 0 },
        { }
    };
    MODULE_DEVICE_TABLE(espi, my_ec_ids);

    static int my_ec_probe(struct espi_device *edev)
    {
        /* register for hardware events */
        nb->notifier_call = my_ec_event;
        espi_register_notifier(edev->ctrl, nb);
        return 0;
    }

    static void my_ec_remove(struct espi_device *edev)
    {
        espi_unregister_notifier(edev->ctrl, nb);
    }

    static struct espi_driver my_ec_driver = {
        .driver   = { .name = "my-ec" },
        .id_table = my_ec_ids,
        .probe    = my_ec_probe,
        .remove   = my_ec_remove,
    };
    module_espi_driver(my_ec_driver);

Channel-independent commands
============================

espi_get_configuration(), espi_set_configuration(), espi_inband_reset()
and espi_get_status(). GET_STATUS is optional: controllers whose hardware
does not implement the wire command leave .get_status unset.

Capability negotiation and channel management
=============================================

At boot the controller driver reads the target's capability registers via
espi_get_configuration(), negotiates link parameters (I/O mode, clock
frequency, CRC) via espi_set_configuration(), then enables each channel
with espi_enable_channel(). espi_channel_is_enabled() may be called at
any time to query the current state. Channels may be disabled individually
with espi_disable_channel(), for example before an in-band reset.

Channel APIs
============

Peripheral channel
------------------

Carries I/O and memory cycles between the host and target endpoints.

* espi_periph_io_read() / espi_periph_io_write() — 16-bit I/O port
  access; ``width`` is the access size in bytes (1, 2, or 4).
* espi_periph_mem_read() / espi_periph_mem_write() — 32-bit memory
  mapped access.

Virtual Wire channel
--------------------

Carries logical signal state (power sequencing, SMI#, SCI#, IRQs) as
indexed wire groups. Each group carries up to four wire values with
individual valid bits.

* espi_vwire_get() — read a wire group from the target.
* espi_vwire_put() — send a PUT_VIRTUAL_WIRE command to the target.
  Named after the eSPI PUT_VW wire command, not a reference-count
  release.

Wire changes from the target generate an ``ESPI_EVENT_VWIRE_CHANGED``
event delivered through the notifier chain.

OOB channel
-----------

Tunnels SMBus/I2C messages between the host and target out-of-band
processor (BMC, EC). Messages are exchanged as opaque byte buffers with
a tag field for matching requests to responses.

* espi_oob_send() / espi_oob_recv()

Incoming OOB messages generate an ``ESPI_EVENT_OOB_RECEIVED`` event.

Flash Access channel
--------------------

Provides access to a SPI flash device attached to the target. The target
acts as a proxy for flash read, write, and erase operations.

* espi_flash_read() / espi_flash_write() / espi_flash_erase()

Alert mechanism
===============

When the target has upstream data pending it asserts ``ALERT#``. The
controller's hard-IRQ handler acknowledges the interrupt and defers
processing to a threaded IRQ or workqueue. From that process context the
controller driver calls espi_handle_alert(), which acquires the
controller lock and dispatches to ``ops->handle_alert``. The hardware
callback reads the target's status register (GET_STATUS), identifies the
pending channel, and calls espi_notify_event() to deliver the appropriate
``ESPI_EVENT_*`` to all registered slave driver notifiers::

    ALERT# asserted by target
          |
          v
    hard-IRQ handler (controller driver)
          |
          v
    threaded IRQ / workqueue
          |
          v
    espi_handle_alert(ctrl)          [espi-core.c]
          |
          v
    ops->handle_alert(ctrl)          [controller driver]
          | reads GET_STATUS, decodes channel
          v
    espi_notify_event(ctrl, &event)  [espi-slave.c]
          |
          v
    slave driver notifier callback

espi_handle_alert() must always be called from process context; it must
never be called from a hard-IRQ handler.

Events and concurrency
======================

Hardware events (Virtual Wire changes, OOB messages, Peripheral channel
completions, channel state changes) are delivered through a per-controller
blocking notifier chain (espi_register_notifier()/espi_notify_event()).
Callbacks run in process context; controllers deliver events from a
threaded IRQ or workqueue, never from hardirq and never while holding the
controller lock.

API Reference
=============

.. kernel-doc:: include/linux/espi/espi.h

.. kernel-doc:: drivers/espi/espi-slave.c
   :export:
