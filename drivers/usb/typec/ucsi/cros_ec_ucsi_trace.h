/* SPDX-License-Identifier: GPL-2.0 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM cros_ec_ucsi

#if !defined(__CROS_EC_UCSI_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define __CROS_EC_UCSI_TRACE_H

#include <linux/tracepoint.h>

#define decode_cmd(cmd)									\
	__print_symbolic(cmd,								\
		{ 0,				"Unknown command"		},	\
		{ UCSI_PPM_RESET,		"PPM_RESET"			},	\
		{ UCSI_CONNECTOR_RESET,		"CONNECTOR_RESET,"		},	\
		{ UCSI_ACK_CC_CI,		"ACK_CC_CI"			},	\
		{ UCSI_SET_NOTIFICATION_ENABLE,	"SET_NOTIFICATION_ENABLE"	},	\
		{ UCSI_GET_CAPABILITY,		"GET_CAPABILITY"		},	\
		{ UCSI_GET_CONNECTOR_CAPABILITY, "GET_CONNECTOR_CAPABILITY"	},	\
		{ UCSI_SET_UOM,			"SET_UOM"			},	\
		{ UCSI_SET_UOR,			"SET_UOR"			},	\
		{ UCSI_SET_PDM,			"SET_PDM"			},	\
		{ UCSI_SET_PDR,			"SET_PDR"			},	\
		{ UCSI_GET_ALTERNATE_MODES,	"GET_ALTERNATE_MODES"		},	\
		{ UCSI_GET_CAM_SUPPORTED,	"GET_CAM_SUPPORTED"		},	\
		{ UCSI_GET_CURRENT_CAM,		"GET_CURRENT_CAM"		},	\
		{ UCSI_SET_NEW_CAM,		"SET_NEW_CAM"			},	\
		{ UCSI_GET_PDOS,		"GET_PDOS"			},	\
		{ UCSI_GET_CABLE_PROPERTY,	"GET_CABLE_PROPERTY"		},	\
		{ UCSI_GET_CONNECTOR_STATUS,	"GET_CONNECTOR_STATUS"		},	\
		{ UCSI_GET_ERROR_STATUS,	"GET_ERROR_STATUS"		})

#define decode_offset(offset)					\
	__print_symbolic(offset,				\
		{ UCSI_VERSION,		"VER"		},	\
		{ UCSI_CCI,		"CCI"		},	\
		{ UCSI_CONTROL,		"CTRL"		},	\
		{ UCSI_MESSAGE_IN,	"MSG_IN"	},	\
		{ UCSI_MESSAGE_OUT,	"MSG_OUT"	},	\
		{ UCSIv2_MESSAGE_OUT,	"MSG_OUTv2"	})

DECLARE_EVENT_CLASS(cros_ec_opm_to_ppm,
	TP_PROTO(u16 offset, const void *value, size_t length),
	TP_ARGS(offset, value, length),
	TP_STRUCT__entry(
		__field(u8, cmd)
		__field(u16, offset)
		__field(size_t, length)
		__dynamic_array(char, msg, length)
	),
	TP_fast_assign(
		__entry->cmd = *((u64 *) value + 3);
		__entry->offset = offset;
		__entry->length = length;
		memcpy(__get_dynamic_array(msg), value, length);
	),
	TP_printk("(%s) %s: %s",
		decode_offset(__entry->offset),
		__entry->offset == UCSI_CONTROL ?
		decode_cmd(__entry->cmd) : "",
		__print_hex(__get_dynamic_array(msg), __entry->length))
);

DEFINE_EVENT(cros_ec_opm_to_ppm, cros_ec_opm_to_ppm_rd,
	TP_PROTO(u16 offset, const void *value, size_t length),
	TP_ARGS(offset, value, length)
);

DEFINE_EVENT(cros_ec_opm_to_ppm, cros_ec_opm_to_ppm_wr,
	TP_PROTO(u16 offset, const void *value, size_t length),
	TP_ARGS(offset, value, length)
);

TRACE_EVENT(cros_ec_ppm_to_opm,
	TP_PROTO(int x),
	TP_ARGS(x),
	TP_STRUCT__entry(__array(char, x, 0)),
	TP_fast_assign((void)x),
	TP_printk("notification%s", "")
);

#endif /* __CROS_EC_UCSI_TRACE_H */

/* This part must be outside protection */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE cros_ec_ucsi_trace

#include <trace/define_trace.h>
