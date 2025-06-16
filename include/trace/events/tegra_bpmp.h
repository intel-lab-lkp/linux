/* SPDX-License-Identifier: GPL-2.0-or-later */
/* tegra memory controller tracepoints
 *
 * Copyright (c) 2025 Codethink Ltd.
 * Ben Dooks <ben.dooks@codethink.co.uk>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM tegra_bpmp

#if !defined(_TRACE_TEGRA_BPMP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TEGRA_BPMP_H

#include <linux/tracepoint.h>

#include <soc/tegra/bpmp.h>

#ifdef CREATE_TRACE_POINTS
static inline const char *tegra_bpmp_mrq_name(unsigned mrq)
{
	switch (mrq) {
	case MRQ_PING: return "ping";
	case MRQ_QUERY_TAG: return "query-tag";
	case MRQ_THREADED_PING: return "threaded-ping";
	case MRQ_DEBUGFS: return "debugfs";
	case MRQ_RESET: return "reset";
	case MRQ_I2C: return "i2c";
	case MRQ_CLK: return "clk";
	case MRQ_QUERY_ABI: return "query-abi";
	case MRQ_THERMAL: return "thermal";
	case MRQ_CPU_VHINT: return "cpu-vhint";
	case MRQ_ABI_RATCHET: return "abi-ratchet";
	case MRQ_EMC_DVFS_LATENCY: return "emc-dvfs-latency";
	case MRQ_RINGBUF_CONSOLE: return "ringbuf-console";
	case MRQ_PG: return "pg";
	case MRQ_CPU_NDIV_LIMITS: return "cpu-ndiv-limits";
	case MRQ_STRAP: return "strap";
	case MRQ_UPHY: return "uphy";
	case MRQ_CPU_AUTO_CC3: return "cpu-auto-cc3";
	case MRQ_QUERY_FW_TAG: return "query-fw-tag";
	case MRQ_FMON: return "fwmon";
	case MRQ_EC: return "ec";
	case MRQ_DEBUG: return "debug";
	case MRQ_EMC_DVFS_EMCHUB: return "emc-dvfs-emchub";
	case MRQ_BWMGR: return "bwmgr";
	case MRQ_ISO_CLIENT: return "iso-client";
	case MRQ_EMC_DISP_RFL: return "emc-disp-rfl";
	case MRQ_TELEMETRY: return "telemetry";
	case MRQ_PWR_LIMIT: return "pwr-limit";
	case MRQ_GEARS: return "gears";
	case MRQ_BWMGR_INT: return "bwmgr-int";
	case MRQ_OC_STATUS: return "oc-status";
	}

	return "";
}

#define __pm_state(__s) ((__s) ? "suspended" : "active")
#endif /* CREATE_TRACE_POINTS */

TRACE_EVENT(tegra_bpmp_pm,
	    TP_PROTO(struct tegra_bpmp *bpmp, bool suspend),
	    TP_ARGS(bpmp, suspend),
	    TP_STRUCT__entry(
		    __string(dev, dev_name(bpmp->dev))
		    __field(bool, suspend)
		    __field(bool, prev)),
	    TP_fast_assign(
		    __assign_str(dev);
		    __entry->prev = bpmp->suspended;
		    __entry->suspend = suspend;
		    ),
	    TP_printk("bpmp %s: pm %s to %s",
		      __get_str(dev),
		      __pm_state(__entry->prev), __pm_state(__entry->suspend))
);

TRACE_EVENT(tegra_bpmp_transfer,
	    TP_PROTO(struct tegra_bpmp *bpmp,
		     struct tegra_bpmp_message *msg),
	    TP_ARGS(bpmp, msg),
	    TP_STRUCT__entry(
			     __string(dev, dev_name(bpmp->dev))
			     __field_struct(struct tegra_bpmp_message, msg)
	    ),
	    TP_fast_assign(
			   __assign_str(dev);
			   __entry->msg = *msg;
	    ),
	    TP_printk("bpmp %s: tx %px, %zd rx %px, %zd, mrq %u (%s), flags %lu",
		      __get_str(dev),
		      __entry->msg.tx.data, __entry->msg.tx.size,
		      __entry->msg.rx.data, __entry->msg.rx.size,
		      __entry->msg.mrq, tegra_bpmp_mrq_name(__entry->msg.mrq),
		      __entry->msg.flags)
);

TRACE_EVENT(tegra_bpmp_channel_write,
	    TP_PROTO(struct tegra_bpmp_channel *channel,
		     unsigned int mrq, unsigned long flags,
		     const void *data, size_t size),
	    TP_ARGS(channel, mrq, flags, data, size),
	    TP_STRUCT__entry(
			     __field(unsigned int, index)
			     __field(unsigned int, mrq)
			     __field(unsigned long, flags)
			     ),
	    TP_fast_assign(
		    __entry->index = channel->index;
		    __entry->mrq = mrq;
		    __entry->flags = flags;
		    ),
	    TP_printk("channel %u: mrq %u (%s), flags %lu",
		      __entry->index,
		      __entry->mrq, tegra_bpmp_mrq_name(__entry->mrq),
		      __entry->flags)
);

TRACE_EVENT(tegra_bpmp_channel_read,
	    TP_PROTO(struct tegra_bpmp_channel *channel,
		     void *data, size_t size, int *ret, int err),
	    TP_ARGS(channel, data, size, ret, err),
	    TP_STRUCT__entry(
		    __field(unsigned int, index)
		    __field(int, ret)
		    __field(int, err)
		    ),
	    TP_fast_assign(
		    __entry->index = channel->index;
		    __entry->ret = ret ? *ret : -ENOENT;
		    __entry->err = err;
		    ),
	    TP_printk("channel %u: read returned %d, err %d",
		      __entry->index, __entry->ret, __entry->err)
);

#endif /* _TRACE_TEGRA_BPMP_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
