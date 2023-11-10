.. SPDX-License-Identifier: GPL-2.0

============
XFRM Syscall
============

/proc/sys/net/core/xfrm_* Variables:
====================================

xfrm_acq_expires - INTEGER
	default 30 - hard timeout in seconds for acquire requests

xfrm_iptfs_maxqsize - UNSIGNED INTEGER
        The default IPTFS max output queue size. The output queue is where
        received packets destined for output over an IPTFS tunnel are stored
        prior to being output in aggregated/fragmented form over the IPTFS
        tunnel.

        Default 1M.

xfrm_iptfs_drptime - UNSIGNED INTEGER
        The default IPTFS drop time. The drop time is the amount of time before
        a missing out-of-order IPTFS tunnel packet is considered lost. See also
        the reorder window.

        Default 1s (1000000).

xfrm_iptfs_idelay - UNSIGNED INTEGER
        The default IPTFS initial output delay. The initial output delay is the
        amount of time prior to servicing the output queue after queueing the
        first packet on said queue.

        Default 0.

xfrm_iptfs_rewin - UNSIGNED INTEGER
        The default IPTFS reorder window size. The reorder window size dictates
        the maximum number of IPTFS tunnel packets in a sequence that may arrive
        out of order.

        Default 3.
