#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
from . import generator
from . import ltl2ba

COLUMN_LIMIT = 100

def line_len(line: str) -> int:
    tabs = line.count('\t')
    return tabs * 7 + len(line)

def break_long_line(line: str, indent='') -> list[str]:
    result = []
    while line_len(line) > COLUMN_LIMIT:
        i = line[:COLUMN_LIMIT - line_len(line)].rfind(' ')
        result.append(line[:i])
        line = indent + line[i + 1:]
    if line:
        result.append(line)
    return result

def build_condition_string(node: ltl2ba.GraphNode):
    if not node.labels:
        return "(true)"

    result = "("

    first = True
    for label in sorted(node.labels):
        if not first:
            result += " && "
        result += label
        first = False

    result += ")"

    return result

def abbreviate_atoms(atoms: list[str]) -> list[str]:
    def shorten(s: str) -> str:
        skip = ["is", "by", "or", "and"]
        return '_'.join([x[:2] for x in s.lower().split('_') if x not in skip])

    abbrs = []
    for atom in atoms:
        for i in range(len(atom), -1, -1):
            if sum(a.startswith(atom[:i]) for a in atoms) > 1:
                break
        share = atom[:i]
        unique = atom[i:]
        abbrs.append((shorten(share) + shorten(unique)))
    return abbrs

class ltl2k(generator.Monitor):
    template_dir = "ltl2k"

    def __init__(self, file_path, MonitorType, extra_params={}):
        if MonitorType == "per_task":
            self._target_arg = "struct task_struct *task"
            self._target = "task"
        elif MonitorType == "per_cpu":
            self._target_arg = "unsigned int cpu"
            self._target = "cpu"
        else:
            raise NotImplementedError(f"LTL does not support monitor type {MonitorType}")
        super().__init__(extra_params)
        self.monitor_type = MonitorType
        with open(file_path) as f:
            self.atoms, self.ba, self.ltl = ltl2ba.create_graph(f.read())
        self.atoms_abbr = abbreviate_atoms(self.atoms)
        self.name = extra_params.get("model_name")
        if not self.name:
            self.name = Path(file_path).stem

    def _fill_monitor_type(self) -> str:
        if self.monitor_type == "per_task":
            return "#define LTL_MONITOR_TYPE RV_MON_PER_TASK"
        if self.monitor_type == "per_cpu":
            return "#define LTL_MONITOR_TYPE RV_MON_PER_CPU"
        assert False

    def _fill_states(self) -> str:
        buf = [
            "enum ltl_buchi_state {",
        ]

        for node in self.ba:
            buf.append("\tS%i," % node.id)
        buf.append("\tRV_NUM_BA_STATES")
        buf.append("};")
        buf.append("static_assert(RV_NUM_BA_STATES <= RV_MAX_BA_STATES);")
        return buf

    def _fill_atoms(self):
        buf = ["enum ltl_atom {"]
        for a in sorted(self.atoms):
            buf.append("\tLTL_%s," % a)
        buf.append("\tLTL_NUM_ATOM")
        buf.append("};")
        buf.append("static_assert(LTL_NUM_ATOM <= RV_MAX_LTL_ATOM);")
        return buf

    def _fill_atoms_to_string(self):
        buf = [
            "static const char *ltl_atom_str(enum ltl_atom atom)",
            "{",
            "\tstatic const char *const names[] = {"
        ]

        for name in self.atoms_abbr:
            buf.append("\t\t\"%s\"," % name)

        buf.extend([
            "\t};",
            "",
            "\treturn names[atom];",
            "}"
        ])
        return buf

    def _fill_atom_values(self, required_values):
        buf = []
        for node in self.ltl:
            if str(node) not in required_values:
                continue

            if isinstance(node.op, ltl2ba.AndOp):
                buf.append("\tbool %s = %s && %s;" % (node, node.op.left, node.op.right))
                required_values |= {str(node.op.left), str(node.op.right)}
            elif isinstance(node.op, ltl2ba.OrOp):
                buf.append("\tbool %s = %s || %s;" % (node, node.op.left, node.op.right))
                required_values |= {str(node.op.left), str(node.op.right)}
            elif isinstance(node.op, ltl2ba.NotOp):
                buf.append("\tbool %s = !%s;" % (node, node.op.child))
                required_values.add(str(node.op.child))

        for atom in self.atoms:
            if atom.lower() not in required_values:
                continue
            buf.append("\tbool %s = test_bit(LTL_%s, mon->atoms);" % (atom.lower(), atom))

        buf.reverse()

        buf2 = []
        for line in buf:
            buf2.extend(break_long_line(line, "\t     "))
        return buf2

    def _fill_transitions(self):
        buf = [
            "static void",
            "ltl_possible_next_states(struct ltl_monitor *mon, unsigned int state, unsigned long *next)",
            "{"
        ]

        required_values = set()
        for node in self.ba:
            for o in sorted(node.outgoing):
                required_values |= o.labels

        buf.extend(self._fill_atom_values(required_values))
        buf.extend([
            "",
            "\tswitch (state) {"
        ])

        for node in self.ba:
            buf.append("\tcase S%i:" % node.id)

            for o in sorted(node.outgoing):
                line   = "\t\tif "
                indent = "\t\t   "

                line += build_condition_string(o)
                lines = break_long_line(line, indent)
                buf.extend(lines)

                buf.append("\t\t\t__set_bit(S%i, next);" % o.id)
            buf.append("\t\tbreak;")
        buf.extend([
            "\t}",
            "}"
        ])

        return buf

    def _fill_start(self):
        buf = [
            "static void ltl_start(%s, struct ltl_monitor *mon)" % self._target_arg,
            "{"
        ]

        required_values = set()
        for node in self.ba:
            if node.init:
                required_values |= node.labels

        buf.extend(self._fill_atom_values(required_values))
        buf.append("")

        for node in self.ba:
            if not node.init:
                continue

            line   = "\tif "
            indent = "\t   "

            line += build_condition_string(node)
            lines = break_long_line(line, indent)
            buf.extend(lines)

            buf.append("\t\t__set_bit(S%i, mon->states);" % node.id)
        buf.append("}")
        return buf

    def fill_tracepoint_handlers_skel(self):
        buff = []
        buff.append("static void handle_example_event(void *data, /* XXX: fill header */)")
        buff.append("{")
        buff.append("\tltl_atom_update(%s, LTL_%s, true/false);" % (self._target, self.atoms[0]))
        buff.append("}")
        buff.append("")
        return '\n'.join(buff)

    def fill_tracepoint_attach_probe(self):
        return "\trv_attach_trace_probe(\"%s\", /* XXX: tracepoint */, handle_example_event);" \
                % self.name

    def fill_tracepoint_detach_helper(self):
        return "\trv_detach_trace_probe(\"%s\", /* XXX: tracepoint */, handle_sample_event);" \
                % self.name

    def fill_atoms_init(self):
        buff = []
        for a in self.atoms:
            buff.append("\tltl_atom_set(mon, LTL_%s, true/false);" % a)
        return '\n'.join(buff)

    def fill_model_h(self):
        buf = [
            "/* SPDX-License-Identifier: GPL-2.0 */",
            "",
            "/*",
            " * C implementation of Buchi automaton, automatically generated by",
            " * tools/verification/rvgen from the linear temporal logic specification.",
            " * For further information, see kernel documentation:",
            " *   Documentation/trace/rv/linear_temporal_logic.rst",
            " */",
            "",
            "#include <linux/rv.h>",
            "",
            "#define MONITOR_NAME " + self.name,
            ""
        ]

        buf.append(self._fill_monitor_type())
        buf.append('')

        buf.extend(self._fill_atoms())
        buf.append('')

        buf.extend(self._fill_atoms_to_string())
        buf.append('')

        buf.extend(self._fill_states())
        buf.append('')

        buf.extend(self._fill_start())
        buf.append('')

        buf.extend(self._fill_transitions())
        buf.append('')

        return '\n'.join(buf)

    def fill_monitor_class_type(self):
        if self.monitor_type == "per_task":
            return "LTL_MON_EVENTS_ID"
        if self.monitor_type == "per_cpu":
            return "LTL_MON_EVENTS_CPU"
        assert False

    def fill_monitor_class(self):
        if self.monitor_type == "per_task":
            return "ltl_monitor_id"
        if self.monitor_type == "per_cpu":
            return "ltl_monitor_cpu"
        assert False

    def fill_tracepoint_args_skel(self, tp_type):
        if tp_type == "event":
            return \
                ("\tTP_PROTO(%s, char *states, char *atoms, char *next),\n" % self._target_arg) + \
                ("\tTP_ARGS(%s, states, atoms, next)" % self._target)
        if tp_type == "error":
            return \
                ("\tTP_PROTO(%s),\n" % self._target_arg) + \
                ("\tTP_ARGS(%s)" % self._target)

    def fill_main_c(self):
        main_c = super().fill_main_c()
        main_c = main_c.replace("%%ATOMS_INIT%%", self.fill_atoms_init())
        main_c = main_c.replace("%%TARGET_ARG%%", self._target_arg)

        return main_c
