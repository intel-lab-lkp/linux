#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# -*- python -*-
# -*- coding: utf-8 -*-

import perf

def main():
        cpus = perf.cpu_map()
        thread_map = perf.thread_map(-1)
        evlist = perf.evlist(cpus, thread_map)

        evsel1 = perf.evsel(type = perf.TYPE_SOFTWARE,
                 config = perf.COUNT_SW_CPU_CLOCK,
                 read_format = perf.FORMAT_TOTAL_TIME_ENABLED | perf.FORMAT_TOTAL_TIME_RUNNING,
                 disabled=1)
        evlist.add(evsel1)

        evsel2 = perf.evsel(type = perf.TYPE_SOFTWARE,
                 config = perf.COUNT_SW_TASK_CLOCK,
                 read_format = perf.FORMAT_TOTAL_TIME_ENABLED | perf.FORMAT_TOTAL_TIME_RUNNING,
                 disabled=1)
        evlist.add(evsel2)

        evlist.open()
        evlist.enable()

        count = 100000
        while count > 0:
            count -= 1

        evlist.disable()
        evsel = evlist.next(None)
        while evsel != None:
            counts = evsel.read(0, 0)
            print(counts.val, counts.ena, counts.run)
            evsel = evlist.next(evsel)
        evlist.close()

if __name__ == '__main__':
    main()
