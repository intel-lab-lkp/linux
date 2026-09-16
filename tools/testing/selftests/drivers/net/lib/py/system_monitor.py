# SPDX-License-Identifier: GPL-2.0

"""Performance measurement helpers for network driver selftests."""

import shlex
import subprocess
import threading
import time

from lib.py import cmd, ksft_metric, ksft_metric_aggregate, \
    ksft_metric_reporting, ksft_pr


class SystemMonitor:
    """Emit CPU and NIC metrics for the local and remote test endpoints."""

    _CPU_FIELDS = ("user", "nice", "sys", "idle", "iowait", "irq", "sirq",
                   "steal")
    _CPU_TIME_METRICS = {
        "cpu.time.usr": ("user", "nice"),
        "cpu.time.sys": ("sys",),
        "cpu.time.sirq": ("sirq",),
    }
    _NIC_METRICS = {
        "rx_dropped": "nic.rx.dropped",
        "rx_missed_errors": "nic.rx.missed_errors",
        "tx_dropped": "nic.tx.dropped",
    }
    _SNAPSHOT_END = "__KSFT_SYSTEM_STAT_END__"
    _NIC_STATS = "__KSFT_NIC_STATS__"
    _CLOCK_TICKS = 100
    _SAMPLE_INTERVAL = 1

    def __init__(self, env):
        self.env = env
        self._remote_sampler = None

    @classmethod
    def _parse_cpu(cls, text):
        stats = {}
        for line in text.splitlines():
            fields = line.split()
            if not fields or not fields[0].startswith("cpu") or \
                    not fields[0][3:].isdigit():
                continue
            stats[int(fields[0][3:])] = {
                name: int(value)
                for name, value in zip(cls._CPU_FIELDS, fields[1:])
            }
        return stats

    @classmethod
    def _parse_nic(cls, text):
        fields = next(line.split() for line in text.splitlines()
                      if line.startswith(cls._NIC_STATS))
        return {name: int(value)
                for name, value in zip(cls._NIC_METRICS, fields[1:])}

    @classmethod
    def _read_nic(cls, ifname):
        stats = {}
        for name in cls._NIC_METRICS:
            path = f"/sys/class/net/{ifname}/statistics/{name}"
            with open(path, encoding="utf-8") as stat_file:
                stats[name] = int(stat_file.read())
        return stats

    @classmethod
    def _cpu_deltas(cls, before, after):
        return {
            cpu: {
                field: after[cpu][field] - before[cpu][field]
                for field in cls._CPU_FIELDS
            }
            for cpu in before.keys() & after.keys()
        }

    @staticmethod
    def _per_cpu_array(values):
        if not values:
            return None
        result = [None] * (max(values) + 1)
        for cpu, value in values.items():
            result[cpu] = value
        return result

    def _start_remote_sampler(self):
        nic_reads = ""
        for name in self._NIC_METRICS:
            path = shlex.quote(
                f"/sys/class/net/{self.env.remote_ifname}/statistics/{name}")
            nic_reads += (f"read value < {path} || exit; "
                          f"printf ' %s' \"$value\"; ")
        command = (
            "while :; do cat /proc/stat || exit; "
            f"printf '{self._NIC_STATS}'; {nic_reads}"
            f"printf '\\n{self._SNAPSHOT_END}\\n'; sleep 1 || exit; done"
        )
        self._remote_sampler = cmd(command, host=self.env.remote,
                                   background=True)

    def _read_remote(self):
        lines = []
        while True:
            line = self._remote_sampler.proc.stdout.readline()
            if not line:
                status = self._remote_sampler.proc.poll()
                raise RuntimeError(
                    f"Remote system sampler stopped unexpectedly ({status})")
            line = line.decode("utf-8", "replace").rstrip("\n")
            if line == self._SNAPSHOT_END:
                return "\n".join(lines)
            lines.append(line)

    def _stop_remote_sampler(self):
        sampler = self._remote_sampler
        self._remote_sampler = None
        if sampler is None:
            return
        try:
            sampler.process(terminate=True, fail=False, timeout=5)
        except subprocess.TimeoutExpired:
            sampler.proc.kill()
            sampler.process(terminate=False, fail=False, timeout=5)

    def _snapshot(self):
        remote = self._read_remote()
        with open("/proc/stat", encoding="utf-8") as stat_file:
            local_cpu = self._parse_cpu(stat_file.read())
        return {
            "local": {
                "cpu": local_cpu,
                "nic": self._read_nic(self.env.ifname),
            },
            "remote": {
                "cpu": self._parse_cpu(remote),
                "nic": self._parse_nic(remote),
            },
        }

    def _record_sample(self, snapshots):
        for host, current in snapshots.items():
            baseline = self._baseline[host]
            previous = self._previous[host]
            elapsed = self._cpu_deltas(baseline["cpu"], current["cpu"])

            for metric, fields in self._CPU_TIME_METRICS.items():
                values = self._per_cpu_array({
                    cpu: sum(delta[field] for field in fields) /
                    self._CLOCK_TICKS
                    for cpu, delta in elapsed.items()
                })
                if values is not None:
                    ksft_metric(metric, values, shape="per-cpu", host=host)

            interval = self._cpu_deltas(previous["cpu"], current["cpu"])
            utilization = {}
            for cpu, delta in interval.items():
                total = sum(delta.values())
                if total > 0:
                    busy = total - delta["idle"] - delta["iowait"]
                    utilization[cpu] = round(100 * busy / total, 4)
            values = self._per_cpu_array(utilization)
            if values is not None:
                ksft_metric("cpu.utilization", values, shape="per-cpu",
                            host=host)

            for counter, metric in self._NIC_METRICS.items():
                value = current["nic"][counter] - baseline["nic"][counter]
                ksft_metric(metric, value, shape="scalar", host=host)

        self._previous = snapshots

    def _register_aggregations(self):
        for metric in self._CPU_TIME_METRICS:
            ksft_metric_aggregate(
                metric, summarize="p90", transform="rate",
                aggregation="busy-core-equivalents",
                display_range={"min": 0, "max": 100},
                display_scale=100,
                display_label="Percent of one CPU")
        ksft_metric_aggregate(
            "cpu.utilization", summarize="p90",
            aggregation="busy-core-equivalents",
            display_range={"min": 0, "max": 100},
            display_label="Percent of one CPU")
        for metric in self._NIC_METRICS.values():
            ksft_metric_aggregate(
                metric, summarize="distribution", transform="rate",
                display_range={"min": 0})

    def _sample_loop(self):
        next_sample = time.monotonic() + self._SAMPLE_INTERVAL
        try:
            while not self._stop_event.wait(
                    max(0, next_sample - time.monotonic())):
                self._record_sample(self._snapshot())
                next_sample += self._SAMPLE_INTERVAL
                while next_sample <= time.monotonic():
                    next_sample += self._SAMPLE_INTERVAL
        except Exception as error:
            self._sample_error = error

    def __enter__(self):
        ksft_metric_reporting(True)
        try:
            self._register_aggregations()
            self._start_remote_sampler()
            self._baseline = self._snapshot()
            self._previous = self._baseline
            for host in self._baseline:
                for metric in self._NIC_METRICS.values():
                    ksft_metric(metric, 0, shape="scalar", host=host)

            self._stop_event = threading.Event()
            self._sample_error = None
            self._thread = threading.Thread(target=self._sample_loop,
                                            daemon=True)
            self._thread.start()
            return self
        except Exception:
            ksft_metric_reporting(False)
            self._stop_remote_sampler()
            raise

    def __exit__(self, exc_type, _exc_value, _exc_tb):
        ksft_metric_reporting(False)
        try:
            self._stop_event.set()
            self._thread.join()
            if self._sample_error is not None:
                raise self._sample_error
            self._record_sample(self._snapshot())
        except Exception as error:
            if exc_type is None:
                raise
            ksft_pr(f"System metric collection failed: {error}")
        finally:
            self._stop_remote_sampler()
        return False
