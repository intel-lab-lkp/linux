#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# -*- python -*-
# -*- coding: utf-8 -*-

import perf

def main():
        cpus = perf.cpu_map()
        thread_map = perf.thread_map(-1)
        evlist = perf.parse_events("cpu-clock,task-clock", cpus, thread_map)

        for ev in evlist:
            ev.read_format = perf.FORMAT_TOTAL_TIME_ENABLED | perf.FORMAT_TOTAL_TIME_RUNNING

        evlist.open()
        evlist.enable()

        count = 100000
        while count > 0:
            count -= 1

        evlist.disable()

        for evsel in evlist:
            for cpu in cpus:
                for thread in range(len(thread_map)):
                    counts = evsel.read(cpu, thread)
                    print(f"For {evsel} val: {counts.val} enable: {counts.ena} run: {counts.run}")

        evlist.close()

if __name__ == '__main__':
    main()
