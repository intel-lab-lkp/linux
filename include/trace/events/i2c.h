/* SPDX-License-Identifier: GPL-2.0-or-later */
/* I2C message transfer tracepoints
 *
 * Copyright (C) 2013 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM i2c

#if !defined(_TRACE_I2C_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_I2C_H

#include <linux/i2c.h>
#include <linux/tracepoint.h>

/*
 * drivers/i2c/i2c-core-base.c
 */
#ifndef I2C_TRACE_REASON_ENUM_DEFINED
#define I2C_TRACE_REASON_ENUM_DEFINED

#define I2C_TRACE_REASON \
EM(HOST_NOTIFY,			"IRQ 0: could not get irq from Host Notify") \
EM(FROM_OF,			"IRQ 0: could not get irq from OF") \
EM(FROM_ACPI,			"IRQ 0: could not get irq from ACPI") \
EM(EPROBE_DEFER_IRQ,		"IRQ 0: could not get IRQ due to EPROBE_DEFER") \
EM(NO_I2C_ID_TABLE,		"no I2C ID table") \
EM(ACPI_ID_MISMATCH,		"ACPI ID table mismatch") \
EM(OF_ID_MISMATCH,		"OF ID table mismatch") \
EM(NO_ID_MATCH,		"no I2C ID table and no ACPI/OF match") \
EM(EPROBE_DEFER_WAKEIRQ,	"get wake IRQ due to EPROBE_DEFER") \
EM(SET_DED_WAKE_FAILED,	"failed to set dedicated wakeup IRQ") \
EM(SET_WAKE_FAILED,		"failed to set wakeup IRQ") \
EM(NO_IRQ,			"no IRQ") \
EM(SET_DEF_CLOCKS,		"set default clocks") \
EM(ATTACH_PM_DOMAIN,		"attach PM domain") \
EM(OPEN_DEVRES_GROUP,		"open devres group") \
EM(PROBE_FAILED,		"specific driver probe failed") \
EMe(NO_PROBE,			"no probe defined")

#undef EM
#undef EMe
#define EM(a, b)	I2C_TRACE_REASON_##a,
#define EMe(a, b)	I2C_TRACE_REASON_##a
enum {
	I2C_TRACE_REASON
};

#undef EM
#undef EMe
#define EM(a, b)	TRACE_DEFINE_ENUM(I2C_TRACE_REASON_##a);
#define EMe(a, b)	TRACE_DEFINE_ENUM(I2C_TRACE_REASON_##a);
I2C_TRACE_REASON

#undef EM
#undef EMe
#define EM(a, b)		{ I2C_TRACE_REASON_##a, b },
#define EMe(a, b)	{ I2C_TRACE_REASON_##a, b }

#endif /* I2C_TRACE_REASON_ENUM_DEFINED */

TRACE_EVENT(i2c_device_probe_debug,

	TP_PROTO(struct device *dev, int err_reason),

	TP_ARGS(dev, err_reason),

	TP_STRUCT__entry(
		__string(	devname,	dev_name(dev)	)
		__field(	int,		err_reason	)
	),

	TP_fast_assign(
		__assign_str(devname);
		__entry->err_reason = err_reason;
	),

	TP_printk("device=%s: %s",
		__get_str(devname),
		__print_symbolic(__entry->err_reason, I2C_TRACE_REASON))
);

TRACE_EVENT(i2c_device_probe_complete,

	TP_PROTO(struct device *dev, int status, int err_reason),

	TP_ARGS(dev, status, err_reason),

	TP_STRUCT__entry(
		__string(	dev_name,	dev_name(dev)	)
		__field(	int,		status		)
		__field(	int,		err_reason	)
	),

	TP_fast_assign(
		__assign_str(dev_name);
		__entry->status = status;
		__entry->err_reason = err_reason;
	),

	TP_printk("%s probe %s: %s",
		__get_str(dev_name),
		__entry->status ? "failed" : "succeeded",
		__entry->status ? __print_symbolic(__entry->err_reason, I2C_TRACE_REASON) : "")
);

extern int i2c_transfer_trace_reg(void);
extern void i2c_transfer_trace_unreg(void);

/*
 * __i2c_transfer() write request
 */
TRACE_EVENT_FN(i2c_write,
	       TP_PROTO(const struct i2c_adapter *adap, const struct i2c_msg *msg,
			int num),
	       TP_ARGS(adap, msg, num),
	       TP_STRUCT__entry(
		       __field(int,	adapter_nr		)
		       __field(__u16,	msg_nr			)
		       __field(__u16,	addr			)
		       __field(__u16,	flags			)
		       __field(__u16,	len			)
		       __dynamic_array(__u8, buf, msg->len)	),
	       TP_fast_assign(
		       __entry->adapter_nr = adap->nr;
		       __entry->msg_nr = num;
		       __entry->addr = msg->addr;
		       __entry->flags = msg->flags;
		       __entry->len = msg->len;
		       memcpy(__get_dynamic_array(buf), msg->buf, msg->len);
			      ),
	       TP_printk("i2c-%d #%u a=%03x f=%04x l=%u [%*phD]",
			 __entry->adapter_nr,
			 __entry->msg_nr,
			 __entry->addr,
			 __entry->flags,
			 __entry->len,
			 __entry->len, __get_dynamic_array(buf)
			 ),
	       i2c_transfer_trace_reg,
	       i2c_transfer_trace_unreg);

/*
 * __i2c_transfer() read request
 */
TRACE_EVENT_FN(i2c_read,
	       TP_PROTO(const struct i2c_adapter *adap, const struct i2c_msg *msg,
			int num),
	       TP_ARGS(adap, msg, num),
	       TP_STRUCT__entry(
		       __field(int,	adapter_nr		)
		       __field(__u16,	msg_nr			)
		       __field(__u16,	addr			)
		       __field(__u16,	flags			)
		       __field(__u16,	len			)
				),
	       TP_fast_assign(
		       __entry->adapter_nr = adap->nr;
		       __entry->msg_nr = num;
		       __entry->addr = msg->addr;
		       __entry->flags = msg->flags;
		       __entry->len = msg->len;
			      ),
	       TP_printk("i2c-%d #%u a=%03x f=%04x l=%u",
			 __entry->adapter_nr,
			 __entry->msg_nr,
			 __entry->addr,
			 __entry->flags,
			 __entry->len
			 ),
	       i2c_transfer_trace_reg,
		       i2c_transfer_trace_unreg);

/*
 * __i2c_transfer() read reply
 */
TRACE_EVENT_FN(i2c_reply,
	       TP_PROTO(const struct i2c_adapter *adap, const struct i2c_msg *msg,
			int num),
	       TP_ARGS(adap, msg, num),
	       TP_STRUCT__entry(
		       __field(int,	adapter_nr		)
		       __field(__u16,	msg_nr			)
		       __field(__u16,	addr			)
		       __field(__u16,	flags			)
		       __field(__u16,	len			)
		       __dynamic_array(__u8, buf, msg->len)	),
	       TP_fast_assign(
		       __entry->adapter_nr = adap->nr;
		       __entry->msg_nr = num;
		       __entry->addr = msg->addr;
		       __entry->flags = msg->flags;
		       __entry->len = msg->len;
		       memcpy(__get_dynamic_array(buf), msg->buf, msg->len);
			      ),
	       TP_printk("i2c-%d #%u a=%03x f=%04x l=%u [%*phD]",
			 __entry->adapter_nr,
			 __entry->msg_nr,
			 __entry->addr,
			 __entry->flags,
			 __entry->len,
			 __entry->len, __get_dynamic_array(buf)
			 ),
	       i2c_transfer_trace_reg,
	       i2c_transfer_trace_unreg);

/*
 * __i2c_transfer() result
 */
TRACE_EVENT_FN(i2c_result,
	       TP_PROTO(const struct i2c_adapter *adap, int num, int ret),
	       TP_ARGS(adap, num, ret),
	       TP_STRUCT__entry(
		       __field(int,	adapter_nr		)
		       __field(__u16,	nr_msgs			)
		       __field(__s16,	ret			)
				),
	       TP_fast_assign(
		       __entry->adapter_nr = adap->nr;
		       __entry->nr_msgs = num;
		       __entry->ret = ret;
			      ),
	       TP_printk("i2c-%d n=%u ret=%d",
			 __entry->adapter_nr,
			 __entry->nr_msgs,
			 __entry->ret
			 ),
	       i2c_transfer_trace_reg,
	       i2c_transfer_trace_unreg);

#endif /* _TRACE_I2C_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
