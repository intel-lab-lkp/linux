.. SPDX-License-Identifier: GPL-2.0

============
XFRM Syscall
============

/proc/sys/net/core/xfrm_* Variables:
====================================

xfrm_acq_expires - INTEGER
	default 30 - hard timeout in seconds for acquire requests

xfrm_iptfs_max_qsize - UNSIGNED INTEGER
        The default IPTFS max output queue size in octets. The output queue is
        where received packets destined for output over an IPTFS tunnel are
        stored prior to being output in aggregated/fragmented form over the
        IPTFS tunnel.

        Default 1M.

xfrm_iptfs_drop_time - UNSIGNED INTEGER
        The default IPTFS drop time in microseconds. The drop time is the amount
        of time before a missing out-of-order IPTFS tunnel packet is considered
        lost. See also the reorder window.

        Default 1s (1000000).

xfrm_iptfs_init_delay - UNSIGNED INTEGER
        The default IPTFS initial output delay in microseconds. The initial
        output delay is the amount of time prior to servicing the output queue
        after queueing the first packet on said queue. This applies anytime
        the output queue was previously empty.

        Default 0.

xfrm_iptfs_reorder_window - UNSIGNED INTEGER
        The default IPTFS reorder window size. The reorder window size dictates
        the maximum number of IPTFS tunnel packets in a sequence that may arrive
        out of order.

        Default 3.
