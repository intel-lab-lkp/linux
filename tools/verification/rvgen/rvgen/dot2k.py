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
        self.enum_suffix = "_%s" % self.name
        self.monitor_class = extra_params["monitor_class"]

    def fill_monitor_type(self) -> str:
        if self.monitor_type == "per_obj":
            return self.monitor_type.upper() + """
typedef /* XXX: define the target type */ *monitor_target;"""
        return self.monitor_type.upper()

    def fill_tracepoint_handlers_skel(self) -> str:
        buff = []
        buff += self.fill_per_obj_definitions()
        buff += self.fill_hybrid_definitions()
        for event in self.events:
            buff.append("static void handle_%s(void *data, /* XXX: fill header */)" % event)
            buff.append("{")
            handle = "handle_event"
            if self.is_start_event(event):
                buff.append("\t/* XXX: validate that this event always leads to the initial state */")
                handle = "handle_start_event"
            elif self.is_start_run_event(event):
                buff.append("\t/* XXX: validate that this event is only valid in the initial state */")
                handle = "handle_start_run_event"
            if self.monitor_type == "per_task":
                buff.append("\tstruct task_struct *p = /* XXX: how do I get p? */;");
                buff.append("\tda_%s(p, %s%s);" % (handle, event, self.enum_suffix));
            elif self.monitor_type == "per_obj":
                buff.append("\tmonitor_target t = /* XXX: how do I get t? */;");
                buff.append("\tda_%s(t, %s%s);" % (handle, event, self.enum_suffix));
            else:
                buff.append("\tda_%s(%s%s);" % (handle, event, self.enum_suffix));
            buff.append("}")
            buff.append("")
        return '\n'.join(buff)

    def fill_tracepoint_attach_probe(self) -> str:
        buff = []
        for event in self.events:
            buff.append("\trv_attach_trace_probe(\"%s\", /* XXX: tracepoint */, handle_%s);" % (self.name, event))
        return '\n'.join(buff)

    def fill_tracepoint_detach_helper(self) -> str:
        buff = []
        for event in self.events:
            buff.append("\trv_detach_trace_probe(\"%s\", /* XXX: tracepoint */, handle_%s);" % (self.name, event))
        return '\n'.join(buff)

    def fill_model_h_header(self) -> list[str]:
        buff = []
        buff.append("/* SPDX-License-Identifier: GPL-2.0 */")
        buff.append("/*")
        buff.append(" * Automatically generated C representation of %s automaton" % (self.name))
        buff.append(" * For further information about this format, see kernel documentation:")
        buff.append(" *   Documentation/trace/rv/deterministic_automata.rst")
        buff.append(" */")
        buff.append("")
        buff.append("#define MONITOR_NAME %s" % (self.name))
        buff.append("")

        return buff

    def fill_model_h(self) -> str:
        #
        # Adjust the definition names
        #
        self.enum_states_def = "states_%s" % self.name
        self.enum_events_def = "events_%s" % self.name
        self.enum_envs_def = "envs_%s" % self.name
        self.struct_automaton_def = "automaton_%s" % self.name
        self.var_automaton_def = "automaton_%s" % self.name

        buff = self.fill_model_h_header()
        buff += self.format_model()

        return '\n'.join(buff)

    def fill_monitor_class_type(self) -> str:
        if self.monitor_type == "per_task":
            return "DA_MON_EVENTS_ID"
        return "DA_MON_EVENTS_IMPLICIT"

    def fill_monitor_class(self) -> str:
        if self.monitor_type == "per_task":
            return "da_monitor_id"
        return "da_monitor"

    def fill_tracepoint_args_skel(self, tp_type: str) -> str:
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
        tp_args_error_env = tp_args_error + [("char *", "env")]
        tp_args_dict = {
                "event": tp_args_event,
                "error": tp_args_error,
                "error_env": tp_args_error_env
                }
        tp_args_id = ("int ", "id")
        tp_args = tp_args_dict[tp_type]
        if self.monitor_type == "per_task":
            tp_args.insert(0, tp_args_id)
        tp_proto_c = ", ".join([a+b for a,b in tp_args])
        tp_args_c = ", ".join([b for a,b in tp_args])
        buff.append("	     TP_PROTO(%s)," % tp_proto_c)
        buff.append("	     TP_ARGS(%s)" % tp_args_c)
        return '\n'.join(buff)

    def fill_hybrid_definitions(self) -> list:
        """Stub, not valid for deterministic automata"""
        return []

    def fill_per_obj_definitions(self) -> list:
        if self.monitor_type == "per_obj":
            return ["""
/*
 * da_get_id - Get the id from a target
 */
static inline da_id_type da_get_id(monitor_target target)
{
	return /* XXX: define how to get an id from the target */;
}
"""]
        return []

    def fill_main_c(self) -> str:
        main_c = super().fill_main_c()

        min_type = self.get_minimun_type()
        nr_events = len(self.events)
        monitor_type = self.fill_monitor_type()

        main_c = main_c.replace("%%MIN_TYPE%%", min_type)
        main_c = main_c.replace("%%NR_EVENTS%%", str(nr_events))
        main_c = main_c.replace("%%MONITOR_TYPE%%", monitor_type)
        main_c = main_c.replace("%%MONITOR_CLASS%%", self.monitor_class)

        return main_c

class da2k(dot2k):
    """Deterministic automata only"""
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        if self.is_hybrid_automata():
            raise ValueError("Detected hybrid automata, use the 'ha' class")

class ha2k(dot2k):
    """Hybrid automata only"""
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        if not self.is_hybrid_automata():
            raise ValueError("Detected deterministic automata, use the 'da' class")
        self.trace_h = self._read_template_file("trace_hybrid.h")
        self.__parse_constraints()

    def fill_monitor_class_type(self) -> str:
        if self.monitor_type == "per_task":
            return "HA_MON_EVENTS_ID"
        return "HA_MON_EVENTS_IMPLICIT"

    def fill_monitor_class(self) -> str:
        """
        Used for tracepoint classes, since they are shared we keep da
        instead of ha (also for the ha specific tracepoints).
        The tracepoint class is not visible to the tools.
        """
        return super().fill_monitor_class()

    def __adjust_value(self, value: str | int, unit: str | None) -> str:
        """Adjust the value in ns"""
        try:
            value = int(value)
        except ValueError:
            # it's a constant, a parameter or a function
            if value.endswith("()"):
                return value.replace("()", "(ha_mon)")
            return value
        match unit:
            case "us":
                value *= 1000
            case "ms":
                value *= 1000000
            case "s":
                value *= 1000000000
        return str(value) + "ull"

    def __parse_state_constraint(self, rule: dict, value: str) -> str:
        # by default assume the timer has ns expiration
        clock_type = "ns"
        if self.env_types.get(rule["env"]) == "j":
            clock_type = "jiffy"

        return "ha_start_timer_%s(ha_mon, %s%s, %s)" % (clock_type, rule["env"],
                                                        self.enum_suffix, value)

    def __validate_constraint(self, key: tuple[int, int] | int, constr: str,
                              rule, reset) -> None:
        # event constrains are tuples and allow both rules and reset
        # state constraints are only used for expirations (e.g. clk<N)
        if self.is_event_constraint(key):
            if not rule and not reset:
                raise ValueError("Unrecognised event constraint (%s/%s: %s)"
                                 % (self.states[key[0]], self.events[key[1]], constr))
            if rule and (rule["env"] in self.env_types and
                         rule["env"] not in self.env_stored):
                raise ValueError("Clocks in hybrid automata always require a storage (%s)"
                                 % rule["env"])
        else:
            if not rule:
                raise ValueError("Unrecognised state constraint (%s: %s)"
                                 % (self.states[key], constr))
            if rule["env"] not in self.env_stored:
                raise ValueError("State constraints always require a storage (%s)"
                                 % rule["env"])
            if rule["op"] not in ["<", "<="]:
                raise ValueError("State constraints must be clock expirations like clk<N (%s)"
                                 % rule.string)

    def __parse_constraints(self) -> None:
        for key, constraint in self.constraints.items():
            rules = []
            resets = []
            for c, sep in self._split_constraint_expr(constraint):
                rule = self.constraint_rule.search(c)
                reset = self.constraint_reset.search(c)
                self.__validate_constraint(key, c, rule, reset)
                if rule:
                    value = rule["val"]
                    value_len = len(rule["val"])
                    unit = None
                    if rule.groupdict().get("unit"):
                        value_len += len(rule["unit"])
                        unit = rule["unit"]
                    c = c[:-(value_len)]
                    value = self.__adjust_value(value, unit)
                    if self.is_event_constraint(key):
                        c = "ha_get_env(ha_mon, %s%s) %s %s" % (
                                rule["env"], self.enum_suffix, rule["op"], value)
                    else:
                        c = self.__parse_state_constraint(rule, value)
                    if sep:
                        c += f" {sep} "
                    rules.append(c)
                if reset:
                    c = "ha_reset_env(ha_mon, %s%s)" % (reset["env"], self.enum_suffix)
                    resets.append(c)
            if self.is_event_constraint(key):
                res = (["res = " + "".join(rules)] if rules else []) + resets
                self.constraints[key] = ";".join(res)
            else:
                self.constraints[key] = rules[0]

    def __get_constr_condition(self) -> list[str]:
        buff = []
        _else = ""
        for edge, constr in self.constraints.items():
            # skip state constraints
            if not self.is_event_constraint(edge):
                continue
            buff.append("\t%sif (curr_state == %s%s && event == %s%s)" %
                        (_else, self.states[edge[0]], self.enum_suffix,
                         self.events[edge[1]], self.enum_suffix))
            if constr.count(";") > 0:
                buff[-1] += " {"
            buff += [ "\t\t%s;" % c for c in constr.split(";") ]
            if constr.count(";") > 0:
                buff.append("\t}")
            _else = "else "
        return buff

    def __get_state_constr_condition(self) -> list[str]:
        buff = []
        # do not even print this if no state constraint is present
        if not [c for c in self.constraints.keys() if not self.is_event_constraint(c)]:
            return buff

        # normally leaving the state with a constraint doesn't touch the timer,
        # but if that event does reset, we need to carry on with the checks
        conditions = ["next_state == curr_state"]
        conditions += ["event != %s%s" % (e, self.enum_suffix)
                       for e in self.self_loop_reset_events]
        condition_str = " && ".join(conditions)
        if len(conditions) > 1:
            condition_str = f"({condition_str})"
        buff.append("\n\tif (%s || !res)\n\t\treturn res;" % condition_str)

        _else = ""
        constrained_states = set()
        for state, constr in self.constraints.items():
            # skip event constraints
            if self.is_event_constraint(state):
                continue
            constrained_states.add(self.states[state])
            buff.append("\t%sif (next_state == %s%s)" %
                        (_else, self.states[state], self.enum_suffix))
            buff.append("\t\t%s;" % constr)
            _else = "else "
        buff.append("\telse if (%s)\n\t\tres = !ha_cancel_timer(ha_mon);" %
                    " ||".join(["curr_state == %s%s" % (s, self.enum_suffix)
                                for s in constrained_states]))
        return buff

    def fill_constr_func(self) -> list[str]:
        buff = []
        if self.constraints:
            buff.append(
"""/*
 * This function is used to validate state transitions.
 *
 * It is generated by parsing the model, there is usually no need to change it,
 * unless conditions were incorrectly specified or too complex for the parser.
 * If the monitor requires a timer, this function is responsible to arm it when
 * the next state has a constraint and cancel it in any other case. Transitions
 * to the same state never affect timers.
 */
static bool ha_verify_constraint(struct ha_monitor *ha_mon,
\t\t\t\t enum %s curr_state, enum %s event,
\t\t\t\t enum %s next_state)
{
\tbool res = true;
""" % (self.enum_states_def, self.enum_events_def, self.enum_states_def))
            buff += self.__get_constr_condition()
            buff += self.__get_state_constr_condition()
            buff.append("""\treturn res;
}\n""")
        return buff

    def __fill_env_getter(self, env: str) -> str:
        if env in self.env_types:
            match self.env_types[env]:
                case "ns" | "us" | "ms" | "s":
                    return "ha_get_clk_ns(ha_mon, env);"
                case "j":
                    return "ha_get_clk_jiffy(ha_mon, env);"
        return "/* XXX: how do I read %s? */" % env

    def __fill_env_resetter(self, env: str) -> str:
        if env in self.env_types:
            match self.env_types[env]:
                case "ns" | "us" | "ms" | "s":
                    return "ha_reset_clk_ns(ha_mon, env);"
                case "j":
                    return "ha_reset_clk_jiffy(ha_mon, env);"
        return "/* XXX: how do I reset %s? */" % env

    def fill_hybrid_get_reset_functions(self) -> list[str]:
        buff = []
        if self.is_hybrid_automata():
            for var in self.constraint_vars:
                if var.endswith("()"):
                    func_name = var.replace("()", "")
                    if func_name.isupper():
                        buff.append("#define %s(ha_mon) /* XXX: what is %s(ha_mon)? */\n" % (func_name, func_name))
                    else:
                        buff.append("static inline u64 %s(struct ha_monitor *ha_mon)\n{" % func_name)
                        buff.append("\treturn /* XXX: what is %s(ha_mon)? */;" % func_name)
                        buff.append("}\n")
                elif var.isupper():
                    buff.append("#define %s /* XXX: what is %s? */\n" % (var, var))
                else:
                    buff.append("static u64 %s = /* XXX: default value */;" % var)
                    buff.append("module_param(%s, ullong, 0644);\n" % var)
            buff.append("""/*
 * These functions define how to read and reset the environment variable.
 *
 * Common environment variables like ns-based and jiffy-based clocks have
 * pre-define getters and resetters you can use. The parser can infer the type
 * of the environment variable if you supply a measure unit in the constraint.
 * If you define your own functions, make sure to add appropriate memory
 * barriers if required.
 * Some environment variables don't require a storage as they read a system
 * state (e.g. preemption count). Those variables are never reset, so we don't
 * define a reset function on monitors only relying on this type of variables.
 */""")
            buff.append("static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs%s env)\n{" % self.enum_suffix)
            _else = ""
            for env in self.envs:
                buff.append("\t%sif (env == %s%s)\n\t\treturn %s" %
                            (_else, env, self.enum_suffix, self.__fill_env_getter(env)))
                _else = "else "
            buff.append("\treturn ENV_INVALID_VALUE;\n}\n")
            if len(self.env_stored):
                buff.append("static void ha_reset_env(struct ha_monitor *ha_mon, enum envs%s env)\n{" % self.enum_suffix)
                _else = ""
                for env in self.env_stored:
                    buff.append("\t%sif (env == %s%s)\n\t\t%s" %
                                (_else, env, self.enum_suffix, self.__fill_env_resetter(env)))
                    _else = "else "
                buff.append("}\n")
        return buff

    def fill_hybrid_definitions(self) -> list[str]:
        return self.fill_hybrid_get_reset_functions() + self.fill_constr_func()
