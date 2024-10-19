.. SPDX-License-Identifier: GPL-2.0

.. _writing_mailbox_controller_drivers:

==================================
Writing Mailbox Controller Drivers
==================================

Introduction
============

This document serves as a basic guideline for driver programmers that need
to hack a new mailbox controller driver or understand the essentials of
the existing ones.

Driver Boilerplate
==================

As a bare minimum, a mailbox controller driver needs to call
``mbox_controller_register`` function to register with the framework.

A basic driver skeleton could look like this for a mailbox hardware that
has the following characteristics:
a. It supports only a single channel, i.e., only the remote processor can
   send interrupts.
b. Data transfer is over the registers associated with mailbox hardware.
c. Mailbox hardware is configured to receive interrupts.
d. When the remote processor is ready to send data, it triggers a mailbox
   interrupt.
e. As part of interrupt handling by Linux, it copies data from the registers.

.. code-block:: c

   #include <linux/device.h>
   #include <linux/interrupt.h>
   #include <linux/io.h>
   #include <linux/kernel.h>
   #include <linux/mailbox_controller.h>
   #include <linux/module.h>
   #include <linux/of.h>
   #include <linux/platform_device.h>
   #define DRIVER_NAME "dummy_controller"

   struct dummy_mbox {
       struct device *dev;
       struct mbox_controller controller;
       int irq;
   };

   static void dummy_mbox_receive(struct mbox_chan *chan)
   {
       struct dummy_mbox *mbox = chan->con_priv;
       int val;

       // Data copied from registers
       val = read_register();
       mbox_chan_received_data(chan, &val);
   }

   static irqreturn_t dummy_mbox_irq_handler(int irq, void *data)
   {
       struct mbox_chan *chan = data;
       struct dummy_mbox *mbox = chan->con_priv;
       u32 reg;

       // Read registers to see if data is received
       dummy_mbox_receive(chan);
       mbox_chan_txdone(chan, 0);
       return reg ? IRQ_HANDLED : IRQ_NONE;
   }

   static int dummy_mbox_send_data(struct mbox_chan *chan, void *data)
   {
       // Write data in registers to send it to the remote processor
       return 0;
   }

   static int dummy_mbox_startup(struct mbox_chan *chan)
   {
       struct dummy_mbox *mbox = chan->con_priv;
       u32 reg;
       int ret;

       ret = devm_request_irq(mbox->dev, mbox->irq, dummy_mbox_irq_handler, 0,
               DRIVER_NAME, chan);
       if (ret < 0) {
           dev_err(mbox->dev, "Cannot request irq\n");
           return ret;
       }

       /* Register write to enable IRQ generation */

       return 0;
   }

   static void dummy_mbox_shutdown(struct mbox_chan *chan)
   {
       struct dummy_mbox *mbox = chan->con_priv;

       /* Disable interrupt generation */
       devm_free_irq(mbox->dev, mbox->irq, chan);
   }

   static const struct mbox_chan_ops dummy_mbox_ops = {
       .send_data = dummy_mbox_send_data,
       .startup = dummy_mbox_startup,
       .shutdown = dummy_mbox_shutdown,
   };

   static int dummy_mbox_probe(struct platform_device *pdev)
   {
       struct dummy_mbox *mbox;
       struct mbox_chan *chans;
       int ret;

       mbox = devm_kzalloc(&pdev->dev, sizeof(*mbox), GFP_KERNEL);
       if (!mbox)
           return -ENOMEM;

       /* Allocate one channel */
       chans = devm_kzalloc(&pdev->dev, sizeof(*chans), GFP_KERNEL);
       if (!chans)
           return -ENOMEM;

       mbox->base = devm_platform_ioremap_resource(pdev, 0);
       if (IS_ERR(mbox->base))
           return PTR_ERR(mbox->base);

       mbox->irq = platform_get_irq(pdev, 0);
       if (mbox->irq < 0)
           return mbox->irq;

       mbox->dev = &pdev->dev;

       /* Hardware supports only one channel. */
       mbox->controller.dev = mbox->dev;
       mbox->controller.num_chans = 1;
       mbox->controller.chans = chans;
       mbox->controller.ops = &dummy_mbox_ops;
       mbox->controller.txdone_irq = true;

       ret = devm_mbox_controller_register(mbox->dev, &mbox->controller);
       if (ret) {
           dev_err(&pdev->dev, "Could not register mailbox controller\n");
           return ret;
       }

       return ret;
   }

   static const struct of_device_id dummy_mbox_match[] = {
       { .compatible = "dummy,dummy-mailbox" },
       { },
   };

   MODULE_DEVICE_TABLE(of, dummy_mbox_match);

   static struct platform_driver dummy_mbox_driver = {
       .probe = dummy_mbox_probe,
       .driver = {
           .name = DRIVER_NAME,
           .of_match_table = dummy_mbox_match,
       },
   };

   module_platform_driver(dummy_mbox_driver);
   MODULE_LICENSE("GPL v2");
   MODULE_DESCRIPTION("Dummy mailbox controller driver");

In the above code, a couple of things are done:
a. The controller is registered in the probe along with callbacks, which in
   this case are the bare minimum: ``startup``, ``shutdown``, and
   ``send_data``.
b. IRQ is registered to get notifications from the remote processor.
c. In the IRQ handler, registers are read to copy data, and
   ``mbox_chan_received_data`` is called to hand over the data to the client.
d. ``mbox_chan_txdone`` is called to let the framework know that this data
   is the last data and no more data is to be expected for the current transfer.

