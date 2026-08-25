.. SPDX-License-Identifier: GPL-2.0

========================================
Freescale Fast Ethernet Controller (FEC)
========================================

The Fast Ethernet Controller (FEC), also known as ENET, is the Ethernet MAC
found on many Freescale/NXP SoCs, including the i.MX and Vybrid families. This
document describes driver-specific configuration that is not covered by the
generic networking documentation.

Ethtool private flags
======================

Some hardware features that are specific to the FEC and have no generic ethtool
control are exposed as ethtool private flags. The set of available flags depends
on the SoC and on the driver configuration (for example, the number of receive
queues), so the flags are enumerated at runtime::

	$ ethtool --show-priv-flags ethX

	$ ethtool --set-priv-flags ethX <flag> on|off

rx-flush-qN
-----------

On multi-queue capable controllers the driver exposes one ``rx-flush-qN``
private flag per receive queue (``rx-flush-q0``, ``rx-flush-q1``, ...), which
enables RX flushing for that queue. RX flushing is disabled by default.

The controller uses a single RX FIFO that is shared by all receive queues. A
received frame is only removed from the head of the FIFO once it has been
copied into the buffer descriptor ring of the queue it is destined for. If that
ring cannot currently accept the frame - i.e. its next buffer descriptor is not
marked empty (``RxBD[EMPTY]`` is clear) or the ring has not been (re)activated
(``ENET_RDARn`` is clear) - the frame stays at the head of the FIFO and blocks
all subsequent frames, including those destined for other, non-congested queues.

When RX flushing is enabled for a queue, a frame that would otherwise block the
FIFO in this way is instead discarded (flushed), so that frames for the other
queues can keep flowing. This is controlled through the ``RX_FLUSHn`` bits of
the ``FEC_QOS_SCHEME`` (``ENET_QOS``) register; see the "Receive flush" and
"ENET_QOS field descriptions" sections of the SoC reference manual (for example
the i.MX 8DualX/8DualXPlus/8QuadXPlus Applications Processor Reference Manual,
IMX8DQXPRM).

.. note::

   Due to erratum ERR050395 (see the applicable Mask Set Errata document, e.g.
   IMX8X_0N99Z for the i.MX 8QuadXPlus), enabling RX flushing on more than one
   receive queue at a time can, under certain traffic conditions, lock up the
   receive path instead of flushing the blocking frame. To avoid triggering the
   erratum the driver rejects (with ``-EINVAL``) any attempt to enable
   ``rx-flush-qN`` on more than one queue simultaneously; only a single queue
   may have RX flushing enabled.
