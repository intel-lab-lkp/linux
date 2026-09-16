# SPDX-License-Identifier: GPL-2.0

"""Run and parse kperf network performance measurements."""

import re
import shlex
import subprocess
import uuid
from dataclasses import dataclass

from lib.py import cmd, ksft_metric, ksft_metric_aggregate, \
    ksft_metric_reporting_enabled, ksft_pr, wait_port_listen


KPERF_PORT = 18323

_SECTION_RE = re.compile(r"==\s+(Source|Target)\b")
_RATE_RE = re.compile(r"\b(Tx|Rx)\s*([0-9]+(?:\.[0-9]+)?)\s+Gbps\b")
_TCP_HEADER = "TCP retrans reord rtt rttvar d_ce snd_wnd cwnd"
_TCP_ROW_RE = re.compile(r"^\s*(?:[^:\s]+:\s*)?(\d+)(?:\s+\d+){6}\s*$")


@dataclass
class KperfResult:
    source_throughput: float
    target_throughput: float
    source_retransmits: int
    target_retransmits: int


class KperfRunner:
    """Manage kperf servers and run measurements between test endpoints."""

    def __init__(self, env):
        self.env = env
        self._pid_file = f"/tmp/ksft-kperf-{uuid.uuid4().hex[:8]}.pid"
        self._started = []
        env.require_cmd("kperf-client", local=True)
        env.require_cmd("kperf-server", local=True, remote=True)

    def _server(self, remote, kill=False):
        host = self.env.remote if remote else None
        address = self.env.remote_addr if remote else self.env.addr
        args = ["kperf-server", "--pid-file", self._pid_file]
        args += ["--kill"] if kill else ["--addr", address]
        return cmd(shlex.join(args) if remote else args, host=host,
                   fail=not kill)

    def __enter__(self):
        try:
            for remote in (False, True):
                self._server(remote)
                self._started.append(remote)
            wait_port_listen(KPERF_PORT)
            wait_port_listen(KPERF_PORT, host=self.env.remote)
        except Exception:
            self.close()
            raise
        return self

    def close(self):
        while self._started:
            self._server(self._started.pop(), kill=True)

    def __exit__(self, _exc_type, _exc_value, _exc_tb):
        self.close()

    def run(self, client_args=()):
        """Run kperf and report results when a SystemMonitor is active."""
        argv = ["kperf-client", "--src", self.env.remote_addr,
                "--dst", self.env.addr]
        argv += [str(arg) for arg in client_args]
        command = cmd(argv, background=True)
        try:
            command.process(terminate=False, fail=False, timeout=300)
        except subprocess.TimeoutExpired as error:
            command.proc.kill()
            command.process(terminate=False, fail=False)
            raise RuntimeError(f"kperf client timed out\n{command!r}") \
                from error
        if command.ret:
            raise RuntimeError(
                f"kperf client exited with status {command.ret}\n{command!r}")

        try:
            result = self.parse(command.stdout + "\n" + command.stderr)
        except ValueError:
            ksft_pr(command)
            raise

        if ksft_metric_reporting_enabled():
            ksft_metric_aggregate(
                "throughput", summarize="distribution",
                regression={"compare": "p50", "better": "higher",
                            "relative_tolerance": 0.05})
            ksft_metric_aggregate("tcp.retransmits", summarize="max")
            for host, throughput, retransmits in (
                    ("remote", result.source_throughput,
                     result.source_retransmits),
                    ("local", result.target_throughput,
                     result.target_retransmits)):
                ksft_metric("throughput", throughput, shape="scalar",
                            host=host)
                ksft_metric("tcp.retransmits", retransmits, shape="scalar",
                            host=host)
        return result

    @staticmethod
    def parse(output):
        """Parse human-readable kperf client output."""
        throughput = {"Source": [], "Target": []}
        retransmits = {"Source": [], "Target": []}
        section = None
        in_tcp = False

        for line in output.splitlines():
            match = _SECTION_RE.search(line)
            if match:
                section = match.group(1)
                in_tcp = False
                continue
            if section is None:
                continue

            match = _RATE_RE.search(line)
            if match:
                direction, value = match.groups()
                if (section, direction) in (("Source", "Tx"),
                                             ("Target", "Rx")):
                    throughput[section].append(float(value))
                continue

            if _TCP_HEADER in line:
                in_tcp = True
                continue
            if in_tcp:
                match = _TCP_ROW_RE.match(line)
                if match:
                    retransmits[section].append(int(match.group(1)))

        values = (*throughput.values(), *retransmits.values())
        if not all(values):
            raise ValueError("incomplete kperf result")

        return KperfResult(sum(throughput["Source"]),
                           sum(throughput["Target"]),
                           sum(retransmits["Source"]),
                           sum(retransmits["Target"]))
