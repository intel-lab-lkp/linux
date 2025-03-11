#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import argparse
import ntpath
import os
import platform
from pathlib import Path
import sys
import ltl

parser = argparse.ArgumentParser(description='transform ltl file into kernel rv monitor')
parser.add_argument('-l', "--ltl", dest="ltl", required=True)
parser.add_argument('-n', "--model_name", dest="model_name", required=True)
parser.add_argument('-o', "--outdir", dest="outdir", required=True)
params = parser.parse_args()

with open(params.ltl) as f:
    atoms, graph = ltl.create_graph(f.read())
states = []
transitions = []
init_conditions = []

COLUMN_LIMIT = 100

def build_condition_string(node: ltl.GraphNode):
    if not node.labels:
        return "(true)"

    result = "("

    first = True
    for l in sorted(node.labels):
        if not first:
            result += " && "
        result += '(' + l + ')'
        first = False

    result += ")"

    return result

def build_states() -> str:
    states = ''
    for node in graph:
        states += "\t%s,\n" % node.name
    return states

def build_atoms() -> str:
    global atoms
    s = ''
    for a in sorted(atoms):
        s += "\t%s,\n" % a
    return s

def build_transitions() -> list[str]:
    transitions = []
    for node in graph:
        transitions.append("\tcase %s:\n" % node.name)

        result = ""
        first = True
        for o in sorted(node.outgoing):
            if first:
                result += "\t\tif "
                cursor = 19
            else:
                result += "\t\telse if "
                cursor = 24

            condition = build_condition_string(o)

            while len(condition) + cursor > COLUMN_LIMIT:
                i = condition[:COLUMN_LIMIT - cursor].rfind(' ')
                result += condition[:i]
                if cursor == 19:
                    result += '\n\t\t   '
                elif cursor == 24:
                    result += '\t\t\t\t'
                else:
                    raise ValueError()
                condition = condition[i + 1:]

            result += condition

            result += '\n'
            result += ("\t\t\tmon->state = %s;\n" % o.name)
            first = False
        result += "\t\telse\n\t\t\tillegal_state(task, mon);\n"
        result += "\t\tbreak;\n"
        transitions.append(result)
    return transitions

def build_initial_conditions() -> str:
    result = ""
    first = True

    for node in graph:
        if not node.init:
            continue

        if not first:
            result += "\telse if "
            cursor = 16
        else:
            result += "\tif "
            cursor = 11

        condition = build_condition_string(node)
        while len(condition) + cursor > COLUMN_LIMIT:
            i = condition[:COLUMN_LIMIT - cursor].rfind(' ')
            result += condition[:i]
            if cursor == 16:
                result += '\n\t\t'
            elif cursor == 11:
                result += '\n\t   '
            else:
                raise ValueError()
            condition = condition[i + 1:]

        result += condition
        result += '\n'
        result += ("\t\tmon->state = %s;\n" % node.name)
        first = False
    result += "\telse\n\t\tillegal_state(task, mon);\n"
    return result

template = Path(__file__).with_name('template.h')
out = os.path.join(params.outdir, "ba.h")
with template.open() as template, open(out, "w") as file:
    for line in template:
        if line == "%%ATOM_LIST%%\n":
            file.write(build_atoms())
        else:
            line = line.replace("%%MODEL_NAME%%", params.model_name)
            file.write(line)

template = Path(__file__).with_name('template.c')
out = os.path.join(params.outdir, "ba.c")
with template.open() as template, open(out, "w") as file:
    for line in template:
        if line == "%%STATE_LIST%%\n":
            file.write(build_states())
        elif line == "%%BUCHI_START%%\n":
            file.write(build_initial_conditions())
        elif line == "%%BUCHI_TRANSITIONS%%\n":
            for t in build_transitions():
                file.write(t)
        elif line == "%%ERROR_MESSAGE%%\n":
            file.write(build_error_message())
        else:
            line = line.replace("%%MODEL_NAME%%", params.model_name)
            line = line.replace("%%LTL%%", params.ltl)
            file.write(line)
