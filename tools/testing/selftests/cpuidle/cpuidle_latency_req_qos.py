#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
cpuidle: verify latency_req QoS ceilings restrict idle-state selection.

Constrains exit latency via each of the three QoS inputs and checks that
cpuidle states whose exit latency exceeds the ceiling do not gain usage:

  1) /dev/cpu_dma_latency
  2) /dev/cpu_wakeup_latency
  3) /sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us
"""

from __future__ import annotations

import glob
import os
import struct
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

# Source tree: tools/testing/selftests/cpuidle/../kselftest
# Install tree: kselftest_install/cpuidle/../kselftest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
				"..", "kselftest"))
import ksft


CPUIDLE_BASE = "/sys/devices/system/cpu"
DMA_LAT_DEV = "/dev/cpu_dma_latency"
WAKEUP_LAT_DEV = "/dev/cpu_wakeup_latency"

# Keep windows short so the whole collection fits under settings timeout.
IDLE_SEC = 2.0
CPU = 0


@dataclass
class IdleState:
	index: int
	name: str
	latency_us: int
	residency_us: int
	usage: int
	time_us: int


def read_states(cpu: int) -> Dict[int, IdleState]:
	base = f"{CPUIDLE_BASE}/cpu{cpu}/cpuidle"
	states: Dict[int, IdleState] = {}
	paths = sorted(
		glob.glob(f"{base}/state*"),
		key=lambda p: int(os.path.basename(p).replace("state", "")),
	)
	for path in paths:
		idx = int(os.path.basename(path).replace("state", ""))
		states[idx] = IdleState(
			index=idx,
			name=open(f"{path}/name").read().strip(),
			latency_us=int(open(f"{path}/latency").read()),
			residency_us=int(open(f"{path}/residency").read()),
			usage=int(open(f"{path}/usage").read()),
			time_us=int(open(f"{path}/time").read()),
		)
	return states


def usage_delta(before: Dict[int, IdleState],
		after: Dict[int, IdleState]) -> Dict[int, int]:
	return {i: after[i].usage - before[i].usage for i in before}


def pick_ceilings(states: Dict[int, IdleState]) -> List[int]:
	nonzero = sorted({s.latency_us for s in states.values() if s.latency_us > 0})
	out: List[int] = []
	if nonzero:
		out.append(nonzero[0])
	if len(nonzero) >= 2:
		mid = (nonzero[0] + nonzero[1]) // 2
		out.append(mid if mid > nonzero[0] else max(1, nonzero[1] - 1))
	if len(nonzero) >= 3:
		out.append((nonzero[1] + nonzero[2]) // 2)
	# Dedup, keep order
	seen = set()
	uniq = []
	for c in out:
		if c not in seen and c >= 1:
			seen.add(c)
			uniq.append(c)
	return uniq or [1]


def idle_on_cpu(seconds: float) -> None:
	end = time.monotonic() + seconds
	burn_end = time.monotonic() + min(0.1, seconds / 10)
	while time.monotonic() < burn_end:
		pass
	while time.monotonic() < end:
		time.sleep(0.05)


def allowed_states(states: Dict[int, IdleState], ceiling: int) -> List[int]:
	return [i for i, s in states.items() if s.latency_us <= ceiling]


def forbidden_states(states: Dict[int, IdleState], ceiling: int) -> List[int]:
	return [i for i, s in states.items() if s.latency_us > ceiling]


def fmt_state_list(states: Dict[int, IdleState], idxs: List[int]) -> str:
	if not idxs:
		return "(none)"
	return ", ".join(
		f"state{i}:{states[i].name}(lat={states[i].latency_us})"
		for i in idxs
	)


def forbidden_violations(states: Dict[int, IdleState], udelta: Dict[int, int],
			 ceiling: int) -> List[str]:
	"""States that must not be entered but still gained usage."""
	bad = []
	for i in forbidden_states(states, ceiling):
		if udelta[i] > 0:
			s = states[i]
			bad.append(
				f"state{i}({s.name},lat={s.latency_us}) usage+={udelta[i]}"
			)
	return bad


def print_usage_table(states: Dict[int, IdleState], udelta: Dict[int, int],
		      ceiling: int) -> None:
	ksft.print_msg(
		f"{'idx':>3} {'name':<12} {'lat':>6} {'d_usage':>8} {'expect':>8}"
	)
	for i in sorted(states):
		s = states[i]
		expect = "allow" if s.latency_us <= ceiling else "forbid"
		ksft.print_msg(
			f"{i:3d} {s.name:<12} {s.latency_us:6d} {udelta[i]:8d} {expect:>8}"
		)


class DmaLatencyGuard:
	def __init__(self, latency_us: int):
		self.latency_us = latency_us
		self.fd = -1

	def __enter__(self):
		self.fd = os.open(DMA_LAT_DEV, os.O_RDWR)
		os.write(self.fd, struct.pack("i", int(self.latency_us)))
		return self

	def __exit__(self, *args):
		if self.fd >= 0:
			os.close(self.fd)
			self.fd = -1


class WakeupLatencyGuard:
	def __init__(self, latency_us: int):
		self.latency_us = latency_us
		self.fd = -1

	def __enter__(self):
		self.fd = os.open(WAKEUP_LAT_DEV, os.O_RDWR)
		os.write(self.fd, struct.pack("i", int(self.latency_us)))
		return self

	def __exit__(self, *args):
		if self.fd >= 0:
			os.close(self.fd)
			self.fd = -1


class ResumeLatencyGuard:
	def __init__(self, cpu: int, latency_us: int):
		self.path = f"{CPUIDLE_BASE}/cpu{cpu}/power/pm_qos_resume_latency_us"
		self.latency_us = latency_us
		self.prev: Optional[str] = None

	def __enter__(self):
		self.prev = open(self.path).read().strip()
		with open(self.path, "w") as f:
			f.write(f"{int(self.latency_us)}\n")
		return self

	def __exit__(self, *args):
		restore = "0" if self.prev in ("0", "n/a", None) else self.prev
		with open(self.path, "w") as f:
			f.write(f"{restore}\n")


def run_case(desc: str, states: Dict[int, IdleState], ceiling: int,
	     guard) -> None:
	allow = allowed_states(states, ceiling)
	forbid = forbidden_states(states, ceiling)

	ksft.print_msg(f"=== {desc} ===")
	ksft.print_msg(f"ceiling={ceiling}us")
	ksft.print_msg(f"allowed:   {fmt_state_list(states, allow)}")
	ksft.print_msg(f"forbidden: {fmt_state_list(states, forbid)}")

	with guard:
		time.sleep(0.05)
		before = read_states(CPU)
		idle_on_cpu(IDLE_SEC)
		after = read_states(CPU)

	ud = usage_delta(before, after)
	total = sum(ud.values())
	violations = forbidden_violations(states, ud, ceiling)

	print_usage_table(states, ud, ceiling)
	ksft.print_msg(
		f"total_usage+={total} "
		f"violations={violations if violations else 'none'}"
	)

	if total <= 0:
		ksft.test_result_fail(f"{desc}: too little idle activity ({total})")
		return
	if violations:
		ksft.test_result_fail(f"{desc}: {'; '.join(violations)}")
		return
	ksft.test_result_pass(desc)


def build_plan(states: Dict[int, IdleState],
	       ceilings: List[int]) -> List[Tuple[str, int, object]]:
	"""Return list of (description, ceiling, context-manager factory args)."""
	cases: List[Tuple[str, int, object]] = []

	have_dma = os.path.exists(DMA_LAT_DEV)
	have_wakeup = os.path.exists(WAKEUP_LAT_DEV)
	resume_path = f"{CPUIDLE_BASE}/cpu{CPU}/power/pm_qos_resume_latency_us"
	have_resume = os.path.exists(resume_path)

	for ceiling in ceilings:
		if have_dma:
			cases.append(
				(f"cpu_dma_latency ceiling={ceiling}", ceiling,
				 ("dma", ceiling))
			)
		else:
			cases.append(
				(f"cpu_dma_latency ceiling={ceiling}", ceiling,
				 ("skip", "missing /dev/cpu_dma_latency"))
			)

		if have_wakeup:
			cases.append(
				(f"cpu_wakeup_latency ceiling={ceiling}", ceiling,
				 ("wakeup", ceiling))
			)
		else:
			cases.append(
				(f"cpu_wakeup_latency ceiling={ceiling}", ceiling,
				 ("skip", "missing /dev/cpu_wakeup_latency"))
			)

		if have_resume:
			cases.append(
				(f"pm_qos_resume_latency_us ceiling={ceiling}",
				 ceiling, ("resume", ceiling))
			)
		else:
			cases.append(
				(f"pm_qos_resume_latency_us ceiling={ceiling}",
				 ceiling, ("skip", f"missing {resume_path}"))
			)

	return cases


def main() -> None:
	ksft.print_header()

	if os.geteuid() != 0:
		ksft.set_plan(1)
		ksft.test_result_skip("must run as root")
		ksft.finished()

	cpuidle_dir = f"{CPUIDLE_BASE}/cpu{CPU}/cpuidle"
	if not os.path.isdir(cpuidle_dir):
		ksft.set_plan(1)
		ksft.test_result_skip(f"no cpuidle sysfs at {cpuidle_dir}")
		ksft.finished()

	states = read_states(CPU)
	if not states:
		ksft.set_plan(1)
		ksft.test_result_skip("no cpuidle states")
		ksft.finished()

	gov_path = f"{CPUIDLE_BASE}/cpuidle/current_governor"
	gov = open(gov_path).read().strip() if os.path.exists(gov_path) else "?"
	ksft.print_msg(f"governor={gov} cpu={CPU}")
	for i in sorted(states):
		s = states[i]
		ksft.print_msg(
			f"state{i}: {s.name} latency={s.latency_us}us "
			f"residency={s.residency_us}us"
		)

	ceilings = pick_ceilings(states)
	ksft.print_msg(f"ceilings_us={ceilings}")
	cases = build_plan(states, ceilings)
	ksft.set_plan(len(cases))

	for desc, ceiling, kind in cases:
		tag, arg = kind[0], kind[1]
		if tag == "skip":
			ksft.test_result_skip(f"{desc}: {arg}")
			continue
		if tag == "dma":
			guard = DmaLatencyGuard(arg)
		elif tag == "wakeup":
			guard = WakeupLatencyGuard(arg)
		else:
			guard = ResumeLatencyGuard(CPU, arg)
		run_case(desc, states, ceiling, guard)

	ksft.finished()


if __name__ == "__main__":
	main()
