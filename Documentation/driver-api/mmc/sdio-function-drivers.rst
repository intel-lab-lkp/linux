.. SPDX-License-Identifier: GPL-2.0

=================================
SDIO function driver introduction
=================================

This document describes how to write a driver for an SDIO function
(sometimes referred to as an SDIO client driver). It covers the driver
skeleton, the bus access rules and the interrupt model that the MMC core
expects drivers to follow.

Bus model
=========

An SDIO card carries one or more functions. Function 0 is the card
itself and is managed by the MMC core; the remaining functions (1-7) are
the actual devices. For every present function the core registers a
``struct sdio_func`` device on the SDIO bus, which is described by
``include/linux/mmc/sdio_func.h``.

Function drivers bind to these devices with ``sdio_register_driver()``
using a ``struct sdio_driver``. Matching is done through the vendor and
device id, or through the standard interface class::

	static const struct sdio_device_id my_ids[] = {
		{ SDIO_DEVICE(0x1234, 0x5678) },
		{ SDIO_DEVICE_CLASS(SDIO_CLASS_WLAN) },
		{ }
	};
	MODULE_DEVICE_TABLE(sdio, my_ids);

Use the ``SDIO_DEVICE()`` macro for specific devices and
``SDIO_DEVICE_CLASS()`` for standard function classes. Device and
vendor ids are defined in ``include/linux/mmc/sdio_ids.h`` and new ids
should be added there. The ``class``, ``vendor``, ``device``,
``max_blksize`` and ``cur_blksize`` fields of ``struct sdio_func``
describe the bound function.

Bus claiming
============

All register access on an SDIO function requires exclusive access to
the bus. This is done with ``sdio_claim_host()`` and
``sdio_release_host()``, which wrap ``mmc_claim_host()`` and may sleep.
Every call to ``sdio_readb()``, ``sdio_writesb()``, ``sdio_enable_func()``,
``sdio_claim_irq()`` and friends **must** happen while the host is
claimed. Failure to do so triggers a ``WARN_ON(!host->claimed)`` from
the core at best, and corrupted transfers at worst.

.. note::

   Never hold the claim for long periods of time, since this prevents
   the core from servicing other functions on the same bus.

Probing and removing
====================

A typical ``->probe()`` claims the bus, enables the function, sets the
block size and registers the interrupt handler. ``->remove()`` must undo
this in reverse order, and this includes releasing the interrupt (see
`Interrupts`_). The following example shows the expected sequence::

	static int my_probe(struct sdio_func *func,
			    const struct sdio_device_id *id)
	{
		struct my_dev *dev;
		int ret;

		dev = devm_kzalloc(&func->dev, sizeof(*dev), GFP_KERNEL);
		if (!dev)
			return -ENOMEM;

		dev->func = func;

		sdio_claim_host(func);

		ret = sdio_enable_func(func);
		if (ret)
			goto err_release;

		ret = sdio_set_block_size(func, 512);
		if (ret)
			goto err_disable;

		ret = sdio_claim_irq(func, my_irq_handler);
		if (ret)
			goto err_disable;

		sdio_release_host(func);

		sdio_set_drvdata(func, dev);
		return 0;

	err_disable:
		sdio_disable_func(func);
	err_release:
		sdio_release_host(func);
		return ret;
	}

	static void my_remove(struct sdio_func *func)
	{
		struct my_dev *dev = sdio_get_drvdata(func);

		sdio_claim_host(func);
		sdio_release_irq(func);
		sdio_disable_func(func);
		sdio_release_host(func);
	}

``sdio_enable_func()`` polls the function ready status for up to
``func->enable_timeout`` milliseconds, so it must not be called from an
atomic context. Likewise, ``sdio_claim_host()`` can block, so the IRQ
handler must not be used to claim the bus on its behalf (see below).

Register and data access
========================

Two classes of access exist, and the buffer requirements differ.

Single byte access (CMD52)
--------------------------

``sdio_readb()``, ``sdio_writeb()`` and ``sdio_writeb_readb()``
transfer the data inside the command itself. No data phase is involved,
so these calls are safe with stack allocated buffers.

The ``sdio_f0_readb()`` and ``sdio_f0_writeb()`` variants address the
common area (CCCR and FBR registers) instead of the function's own
address space. Note that ``sdio_f0_writeb()`` only accepts addresses
0xF0-0xFF unless the card advertises the lenient function 0 quirk.

Word access is provided by ``sdio_readw()``, ``sdio_readl()``,
``sdio_writew()`` and ``sdio_writel()``. These are implemented as small
CMD53 transfers on a DMA-able scratch buffer owned by the core, so
drivers can use them without allocating anything.

Bulk access (CMD53)
-------------------

``sdio_memcpy_fromio()``, ``sdio_memcpy_toio()``, ``sdio_readsb()``,
``sdio_writesb()`` transfer a data phase. The MMC core builds a
scatterlist directly from the buffer supplied by the driver and the host
controller driver maps it for DMA. The buffer must therefore be:

- A single, contiguous, physically mappable allocation. Use
  ``kmalloc()`` (or a similar DMA-able allocator, such as ``skb``
  buffers).
- Not memory obtained from ``vmalloc()`` and not memory on the stack.
  Those buffers are not part of the kernel linear map, so
  ``virt_to_page()`` on them yields invalid pages. With
  ``CONFIG_DEBUG_SG`` enabled the kernel will BUG(), without it, DMA
  silently corrupts memory.

Buffer sizes should be aligned with ``sdio_align_size()`` for optimal
performance. The core splits larger transfers internally, but only
within the one linear buffer supplied by the driver.

Interrupts
==========

An SDIO function signals interrupts with a dedicated DAT1 line. Function
drivers register a handler with ``sdio_claim_irq()`` and release it with
``sdio_release_irq()``. Both calls must be made while the bus is
claimed.

The handler is always called by the core with the bus already claimed,
and it must not sleep. Consequently it must not call
``sdio_claim_host()``, ``sdio_release_host()`` or any function that
sleeps. If more work is needed, hand it over to a workqueue and signal
it from the handler.

Internally the core processes interrupts either from a kernel thread
(``ksdioirqd``) or, if the host sets ``MMC_CAP2_SDIO_IRQ_NOTHREAD``,
from a work item. Host drivers that handle their own interrupt line may
call ``sdio_signal_irq()`` when they detect that the SDIO function
asserted an interrupt. The core
checks for enabled interrupts when the work runs, so work that races
with ``->remove()`` simply returns without calling back into the
driver. This is why ``sdio_release_irq()`` in ``->remove()`` is
mandatory: the core removes the function devices from
``mmc_remove_host()``, and the SDIO function driver ``->remove()``
callback is the last chance to release the interrupts. The commit
6feadbecdae6 ("mmc: core: Cancel SDIO IRQ work before freeing host") is
an example of what goes wrong when this contract is broken.

Power management
================

The core suspends and resumes the SDIO card as a whole. Function
drivers may implement ``struct dev_pm_ops`` and assign it to the
``drv.pm`` member of ``struct sdio_driver``. The bus uses the generic
PM callbacks, so a driver only implements what it needs.

To support wakeup from a suspended host controller, check
``sdio_get_host_pm_caps()`` and call ``sdio_set_host_pm_flags()`` with
``MMC_PM_KEEP_POWER`` or ``MMC_PM_WAKE_SDIO_IRQ`` as appropriate.

Reference drivers
=================

These existing drivers are good reading material:

 - ``drivers/bluetooth/btsdio.c`` - small, self-contained driver
   showing probe/remove and CMD53 transfers.
 - ``drivers/net/wireless/ti/wl1251/sdio.c`` - shows power management
   and retuning helpers.
 - ``drivers/net/wireless/marvell/libertas/if_sdio.c`` - interrupt
   handling.
 - ``drivers/net/wireless/broadcom/brcm80211/brcmfmac/bcmsdh.c`` - a
   full featured driver with block mode transfers.