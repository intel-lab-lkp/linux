#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# -*- python -*-
# -*- coding: utf-8 -*-

import perf
import time

def main():
    cpus    = perf.cpu_map()
    threads = perf.thread_map(-1)

    evlist = perf.parse_events('cpu-cycles', cpus, threads)

    tgt = perf.target(uses_mmap = True, default_per_cpu = True)
    opts = perf.record_opts(freq=1000, target=tgt, sample_time=True,
                            sample_cpu=True, no_buffering=True, no_inherit=True)
    for ev in evlist:
        ev.tracking = False
        ev.read_format = 0
        ev.sample_type = perf.SAMPLE_IP|perf.SAMPLE_TID|perf.SAMPLE_CPU|perf.SAMPLE_PERIOD
    evlist.config(opts)

    evlist.open()
    evlist.mmap()

    evlist.enable()
    time.sleep(2)
    evlist.disable()

    done = False
    while done is False:
        for cpu in cpus:
            event = evlist.read_on_cpu(cpu)
            if event is None:
                done = True
                break

            if not isinstance(event, perf.sample_event):
                continue

            print(f"cpu: {event.sample_cpu:<3} pid: {event.sample_pid:<6} "
                    f"tid: {event.sample_tid:<6} ip: {hex(event.sample_ip):<20} "
                    f"period: {event.sample_period:<20}")

    evlist.close()

if __name__ == '__main__':
    main()
