# SPDX-License-Identifier: GPL-2.0
"""
Live event session helper using perf.evlist.

This module provides a LiveSession class that allows running a callback
for each event collected live from the system, similar to perf.session
but without requiring a perf.data file.
"""

import perf


class LiveSession:
    """Represents a live event collection session."""

    def __init__(self, event_string: str, sample_callback):
        self.event_string = event_string
        self.sample_callback = sample_callback
        # Create a cpu map for all online CPUs
        self.cpus = perf.cpu_map()
        # Parse events and set maps
        self.evlist = perf.parse_events(self.event_string, self.cpus)
        self.evlist.config()

    def run(self):
        """Run the live session."""
        self.evlist.open()
        try:
            self.evlist.mmap()
            self.evlist.enable()

            while True:
                # Poll for events with 100ms timeout
                try:
                    self.evlist.poll(100)
                except InterruptedError:
                    continue
                for cpu in self.cpus:
                    while True:
                        try:
                            event = self.evlist.read_on_cpu(cpu)
                            if event is None:
                                break
                            if event.type == perf.RECORD_SAMPLE:
                                self.sample_callback(event)
                        except Exception as e:
                            import sys
                            print(f"Error processing event on CPU {cpu}: {e}", file=sys.stderr)
                            break
        except KeyboardInterrupt:
            pass
        finally:
            self.evlist.close()
