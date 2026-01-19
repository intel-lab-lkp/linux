#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2019-2022 Red Hat, Inc. Daniel Bristot de Oliveira <bristot@kernel.org>
#
# Automata object: parse an automata in dot file digraph format into a python object
#
# For further information, see:
#   Documentation/trace/rv/deterministic_automata.rst

import ntpath


class AutomataError(OSError):
    """Exception raised for errors in automata parsing and validation.

    Raised when DOT file processing fails due to invalid format, I/O errors,
    or malformed automaton definitions.
    """


class Automata:
    """Automata class: Reads a dot file and parses it as an automata.

    Attributes:
        dot_file: A dot file with an state_automaton definition.
    """

    invalid_state_str = "INVALID_STATE"
    init_marker = "__init_"

    def __init__(self, file_path, model_name=None):
        self.__dot_path = file_path
        self.name = model_name or self.__get_model_name()
        self.__dot_lines = self.__open_dot()
        self.states, self.initial_state, self.final_states = (
            self.__get_state_variables()
        )
        self.events = self.__get_event_variables()
        self.function = self.__create_matrix()
        self.events_start, self.events_start_run = self.__store_init_events()

    def __get_model_name(self):
        basename = ntpath.basename(self.__dot_path)
        if not basename.endswith(".dot") and not basename.endswith(".gv"):
            print("not a dot file")
            raise AutomataError(f"not a dot file: {self.__dot_path}")

        model_name = ntpath.splitext(basename)[0]
        if not model_name:
            raise AutomataError(f"not a dot file: {self.__dot_path}")

        return model_name

    def __open_dot(self):
        cursor = 0
        dot_lines = []
        try:
            with open(self.__dot_path) as dot_file:
                dot_lines = dot_file.read().splitlines()
        except OSError as exc:
            raise AutomataError(f"Cannot open the file: {self.__dot_path}") from exc

        # checking the first line:
        line = dot_lines[cursor].split()

        if (line[0] != "digraph") or (line[1] != "state_automaton"):
            raise AutomataError(f"Not a valid .dot format: {self.__dot_path}")
        else:
            cursor += 1
        return dot_lines

    def __get_cursor_begin_states(self):
        cursor = 0
        while self.__dot_lines[cursor].split()[0] != "{node":
            cursor += 1
        return cursor

    def __get_cursor_begin_events(self):
        cursor = 0
        while self.__dot_lines[cursor].split()[0] != "{node":
            cursor += 1
        while self.__dot_lines[cursor].split()[0] == "{node":
            cursor += 1
        # skip initial state transition
        cursor += 1
        return cursor

    def __get_state_variables(self):
        # wait for node declaration
        states = []
        final_states = []
        initial_state = None

        has_final_states = False
        cursor = self.__get_cursor_begin_states()

        # process nodes
        while self.__dot_lines[cursor].split()[0] == "{node":
            line = self.__dot_lines[cursor].split()
            raw_state = line[-1]

            #  "enabled_fired"}; -> enabled_fired
            state = raw_state.replace('"', "").replace("};", "").replace(",", "_")
            if state.startswith(self.init_marker):
                initial_state = state[len(self.init_marker) :]
            else:
                states.append(state)
                if "doublecircle" in self.__dot_lines[cursor]:
                    final_states.append(state)
                    has_final_states = True

                if "ellipse" in self.__dot_lines[cursor]:
                    final_states.append(state)
                    has_final_states = True

            cursor += 1

        if initial_state is None:
            raise AutomataError("The automaton doesn't have a initial state")

        states = sorted(set(states))

        # Insert the initial state at the beginning of the states
        states.insert(0, initial_state)

        if not has_final_states:
            final_states.append(initial_state)

        return states, initial_state, final_states

    def __get_event_variables(self):
        # here we are at the begin of transitions, take a note, we will return later.
        cursor = self.__get_cursor_begin_events()

        events = []
        while self.__dot_lines[cursor].lstrip()[0] == '"':
            # transitions have the format:
            # "all_fired" -> "both_fired" [ label = "disable_irq" ];
            #  ------------ event is here ------------^^^^^
            if self.__dot_lines[cursor].split()[1] == "->":
                line = self.__dot_lines[cursor].split()
                event = line[-2].replace('"', "")

                # when a transition has more than one labels, they are like this
                # "local_irq_enable\nhw_local_irq_enable_n"
                # so split them.

                event = event.replace("\\n", " ")
                for i in event.split():
                    events.append(i)
            cursor += 1

        return sorted(set(events))

    def __create_matrix(self):
        # transform the array into a dictionary
        events = self.events
        states = self.states
        events_dict = {}
        states_dict = {}
        nr_event = 0
        for event in events:
            events_dict[event] = nr_event
            nr_event += 1

        nr_state = 0
        for state in states:
            states_dict[state] = nr_state
            nr_state += 1

        # declare the matrix....
        matrix = [
            [self.invalid_state_str for x in range(nr_event)] for y in range(nr_state)
        ]

        # and we are back! Let's fill the matrix
        cursor = self.__get_cursor_begin_events()

        while self.__dot_lines[cursor].lstrip()[0] == '"':
            if self.__dot_lines[cursor].split()[1] == "->":
                line = self.__dot_lines[cursor].split()
                origin_state = line[0].replace('"', "").replace(",", "_")
                dest_state = line[2].replace('"', "").replace(",", "_")
                possible_events = line[-2].replace('"', "").replace("\\n", " ")
                for event in possible_events.split():
                    matrix[states_dict[origin_state]][events_dict[event]] = dest_state
            cursor += 1

        return matrix

    def __store_init_events(self):
        events_start = [False] * len(self.events)
        events_start_run = [False] * len(self.events)
        for i, _ in enumerate(self.events):
            curr_event_will_init = 0
            curr_event_from_init = False
            curr_event_used = 0
            for j, _ in enumerate(self.states):
                if self.function[j][i] != self.invalid_state_str:
                    curr_event_used += 1
                if self.function[j][i] == self.initial_state:
                    curr_event_will_init += 1
            if self.function[0][i] != self.invalid_state_str:
                curr_event_from_init = True
            # this event always leads to init
            if curr_event_will_init and curr_event_used == curr_event_will_init:
                events_start[i] = True
            # this event is only called from init
            if curr_event_from_init and curr_event_used == 1:
                events_start_run[i] = True
        return events_start, events_start_run

    def is_start_event(self, event):
        return self.events_start[self.events.index(event)]

    def is_start_run_event(self, event):
        # prefer handle_start_event if there
        if any(self.events_start):
            return False
        return self.events_start_run[self.events.index(event)]
