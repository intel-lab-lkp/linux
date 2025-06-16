/* SPDX-License-Identifier: GPL-2.0-or-later */
/* tegra memory controller tracepoints
 *
 * Copyright (c) 2025 Codethink Ltd.
 * Ben Dooks <ben.dooks@codethink.co.uk>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM tegra_mc

#if !defined(_TRACE_TEGRA_MC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TEGRA_MC_H

#include <linux/device.h>
#include <linux/tracepoint.h>

TRACE_EVENT(tegra_mc_hotreset,
	    TP_PROTO(struct device *dev, unsigned long id, bool assert),
	    TP_ARGS(dev, id, assert),
	    TP_STRUCT__entry(
			     __string(dev, dev_name(dev))
			     __field(unsigned long, id)
			     __field(bool, assert)),
	    TP_fast_assign(
			   __assign_str(dev);
			   __entry->id = id;
			   __entry->assert = assert;
			   ),
	    TP_printk("rcdev %s, id %lu, %s\n",
		      __get_str(dev), __entry->id, __entry->assert ? "assert" : "release")
);

TRACE_EVENT(tegra_mc_err,
	    TP_PROTO(struct device *dev, const char *client,
		     bool secure, bool write, phys_addr_t addr,
		     const char *error, const char *desc, const char *perm),
	    TP_ARGS(dev, client, secure, write, addr, error, desc, perm),
	    TP_STRUCT__entry(
			     __string(dev,		dev_name(dev))
			     __field(const char *,	client)
			     __field(bool, secure)
			     __field(bool, write)
			     __field(phys_addr_t, addr)
			     __field(const char *, error)
			     __field(const char *, desc)
			     __string(perm, perm)),
	    TP_fast_assign(
			   __assign_str(dev);
			   __entry->client = client;
			   __entry->secure = secure;
			   __entry->write = write;
			   __entry->addr = addr;
			   __entry->desc = desc;
			   __assign_str(perm);
			   ),
	    TP_printk("%s: %s: %s%c @%pa %s (%s%s)",
		      __get_str(dev),
		      __entry->client, __entry->secure ? "secure/" : "",
		      __entry->write ? 'w' : 'r',
		      &__entry->addr,
		      __entry->error,
		      __entry->desc,
		      __get_str(perm))
);

#endif /* _TRACE_TEGRA_MC_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
