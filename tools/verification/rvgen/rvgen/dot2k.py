#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2019-2022 Red Hat, Inc. Daniel Bristot de Oliveira <bristot@kernel.org>
#
# dot2k: transform dot files into a monitor for the Linux kernel.
#
# For further information, see:
#   Documentation/trace/rv/da_monitor_synthesis.rst

from .dot2c import Dot2c
from .generator import Monitor


class dot2k(Monitor, Dot2c):
    template_dir = "dot2k"

    def __init__(self, file_path, MonitorType, extra_params={}):
        self.monitor_type = MonitorType
        Monitor.__init__(self, extra_params)
        Dot2c.__init__(self, file_path, extra_params.get("model_name"))
        self.enum_suffix = f"_{self.name}"

    def fill_monitor_type(self):
        return self.monitor_type.upper()

    def fill_tracepoint_handlers_skel(self):
        buff = []
        for event in self.events:
            buff.append(f"static void handle_{event}(void *data, /* XXX: fill header */)")
            buff.append("{")
            handle = "handle_event"
            if self.is_start_event(event):
                buff.append("\t/* XXX: validate that this event always leads to the initial state */")
                handle = "handle_start_event"
            elif self.is_start_run_event(event):
                buff.append("\t/* XXX: validate that this event is only valid in the initial state */")
                handle = "handle_start_run_event"
            if self.monitor_type == "per_task":
                buff.append("\tstruct task_struct *p = /* XXX: how do I get p? */;")
                buff.append(f"\tda_{handle}_{self.name}(p, {event}{self.enum_suffix});")
            else:
                buff.append(f"\tda_{handle}_{self.name}({event}{self.enum_suffix});")
            buff.append("}")
            buff.append("")
        return '\n'.join(buff)

    def fill_tracepoint_attach_probe(self):
        buff = []
        for event in self.events:
            buff.append(f"\trv_attach_trace_probe(\"{self.name}\", /* XXX: tracepoint */, handle_{event});")
        return '\n'.join(buff)

    def fill_tracepoint_detach_helper(self):
        buff = []
        for event in self.events:
            buff.append(f"\trv_detach_trace_probe(\"{self.name}\", /* XXX: tracepoint */, handle_{event});")
        return '\n'.join(buff)

    def fill_model_h_header(self):
        buff = []
        buff.append("/* SPDX-License-Identifier: GPL-2.0 */")
        buff.append("/*")
        buff.append(f" * Automatically generated C representation of {self.name} automaton")
        buff.append(" * For further information about this format, see kernel documentation:")
        buff.append(" *   Documentation/trace/rv/deterministic_automata.rst")
        buff.append(" */")
        buff.append("")

        return buff

    def fill_model_h(self):
        #
        # Adjust the definition names
        #
        self.enum_states_def = f"states_{self.name}"
        self.enum_events_def = f"events_{self.name}"
        self.struct_automaton_def = f"automaton_{self.name}"
        self.var_automaton_def = f"automaton_{self.name}"

        buff = self.fill_model_h_header()
        buff += self.format_model()

        return '\n'.join(buff)

    def fill_monitor_class_type(self):
        if self.monitor_type == "per_task":
            return "DA_MON_EVENTS_ID"
        return "DA_MON_EVENTS_IMPLICIT"

    def fill_monitor_class(self):
        if self.monitor_type == "per_task":
            return "da_monitor_id"
        return "da_monitor"

    def fill_tracepoint_args_skel(self, tp_type):
        buff = []
        tp_args_event = [
                ("char *", "state"),
                ("char *", "event"),
                ("char *", "next_state"),
                ("bool ",  "final_state"),
                ]
        tp_args_error = [
                ("char *", "state"),
                ("char *", "event"),
                ]
        tp_args_id = ("int ", "id")
        tp_args = tp_args_event if tp_type == "event" else tp_args_error
        if self.monitor_type == "per_task":
            tp_args.insert(0, tp_args_id)
        tp_proto_c = ", ".join([a+b for a,b in tp_args])
        tp_args_c = ", ".join([b for a,b in tp_args])
        buff.append(f"	     TP_PROTO({tp_proto_c}),")
        buff.append(f"	     TP_ARGS({tp_args_c})")
        return '\n'.join(buff)

    def fill_main_c(self):
        main_c = super().fill_main_c()

        min_type = self.get_minimun_type()
        nr_events = len(self.events)
        monitor_type = self.fill_monitor_type()

        main_c = main_c.replace("%%MIN_TYPE%%", min_type)
        main_c = main_c.replace("%%NR_EVENTS%%", str(nr_events))
        main_c = main_c.replace("%%MONITOR_TYPE%%", monitor_type)

        return main_c
