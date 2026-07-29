#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
cpuidle: verify latency_req QoS ceilings restrict idle-state selection.

Constrains exit latency via each of the three QoS inputs and checks that
cpuidle states whose exit latency exceeds the ceiling do not gain usage:

  1) /dev/cpu_dma_latency
  2) /dev/cpu_wakeup_latency
  3) /sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us

Also runs unconstrained (no QoS) baselines at the start and end so a
broken latency_req cache that falsely returns 0 cannot hide behind the
constrained cases alone.
"""

from __future__ import annotations

import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpuidle_lib as cl
import ksft


DMA_LAT_DEV = "/dev/cpu_dma_latency"
WAKEUP_LAT_DEV = "/dev/cpu_wakeup_latency"


def pick_ceilings(states: Dict[int, cl.IdleState]) -> List[int]:
	nonzero = sorted({s.latency_us for s in states.values() if s.latency_us > 0})
	out: List[int] = []
	if nonzero:
		out.append(nonzero[0])
	if len(nonzero) >= 2:
		mid = (nonzero[0] + nonzero[1]) // 2
		out.append(mid if mid > nonzero[0] else max(1, nonzero[1] - 1))
	if len(nonzero) >= 3:
		out.append((nonzero[1] + nonzero[2]) // 2)
	seen = set()
	uniq = []
	for c in out:
		if c not in seen and c >= 1:
			seen.add(c)
			uniq.append(c)
	return uniq or [1]


def allowed_states(states: Dict[int, cl.IdleState], ceiling: int) -> List[int]:
	return [i for i, s in states.items() if s.latency_us <= ceiling]


def forbidden_states(states: Dict[int, cl.IdleState], ceiling: int) -> List[int]:
	return [i for i, s in states.items() if s.latency_us > ceiling]


def forbidden_violations(states: Dict[int, cl.IdleState], udelta: Dict[int, int],
			 ceiling: int) -> List[str]:
	bad = []
	for i in forbidden_states(states, ceiling):
		if udelta[i] > 0:
			s = states[i]
			bad.append(
				f"state{i}({s.name},lat={s.latency_us}) usage+={udelta[i]}"
			)
	return bad


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
	"""Restore pm_qos_resume_latency_us exactly as read (n/a vs 0 differ)."""

	def __init__(self, cpu: int, latency_us: int):
		self.path = (f"{cl.CPUIDLE_BASE}/cpu{cpu}/power/"
			     f"pm_qos_resume_latency_us")
		self.latency_us = latency_us
		self.prev: Optional[str] = None

	def __enter__(self):
		self.prev = open(self.path).read().strip()
		with open(self.path, "w") as f:
			f.write(f"{int(self.latency_us)}\n")
		return self

	def __exit__(self, *args):
		if self.prev is None:
			return
		with open(self.path, "w") as f:
			f.write(f"{self.prev}\n")


class NullGuard:
	"""No-op guard: leave all QoS constraints untouched."""

	def __enter__(self):
		return self

	def __exit__(self, *args):
		return False


def run_no_qos_case(desc: str, states: Dict[int, cl.IdleState]) -> None:
	"""Unconstrained idle: expect activity in at least one non-zero-latency state."""
	ksft.print_msg(f"=== {desc} ===")
	ksft.print_msg("ceiling=(none)")

	with NullGuard():
		ud = cl.measure_idle()

	total = sum(ud.values())
	deep = [i for i, s in states.items()
		if s.latency_us > 0 and ud[i] > 0]

	cl.print_usage_table(states, ud)
	ksft.print_msg(
		f"total_usage+={total} "
		f"nonzero_latency_entered={cl.fmt_state_list(states, deep)}"
	)

	if total <= 0:
		ksft.test_result_fail(f"{desc}: too little idle activity ({total})")
		return
	if not deep:
		ksft.test_result_fail(
			f"{desc}: no state with latency>0 gained usage "
			"(latency_req may be stuck at 0)"
		)
		return
	ksft.test_result_pass(desc)


def run_case(desc: str, states: Dict[int, cl.IdleState], ceiling: int,
	     guard) -> None:
	allow = allowed_states(states, ceiling)
	forbid = forbidden_states(states, ceiling)

	ksft.print_msg(f"=== {desc} ===")
	ksft.print_msg(f"ceiling={ceiling}us")
	ksft.print_msg(f"allowed:   {cl.fmt_state_list(states, allow)}")
	ksft.print_msg(f"forbidden: {cl.fmt_state_list(states, forbid)}")

	with guard:
		ud = cl.measure_idle()

	total = sum(ud.values())
	violations = forbidden_violations(states, ud, ceiling)

	cl.print_usage_table(states, ud, set(forbid))
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


def build_plan(states: Dict[int, cl.IdleState],
	       ceilings: List[int]) -> List[Tuple[str, int, object]]:
	"""Return list of (description, ceiling, context-manager factory args)."""
	cases: List[Tuple[str, int, object]] = []

	have_dma = os.path.exists(DMA_LAT_DEV)
	have_wakeup = os.path.exists(WAKEUP_LAT_DEV)
	resume_path = (f"{cl.CPUIDLE_BASE}/cpu{cl.CPU}/power/"
		       f"pm_qos_resume_latency_us")
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

	states = cl.require_root_and_cpuidle(min_states=1)
	cl.log_states(states)
	cl.warn_preexisting_qos()
	cl.warn_preexisting_disable(states)

	ceilings = pick_ceilings(states)
	ksft.print_msg(f"ceilings_us={ceilings}")
	cases = build_plan(states, ceilings)
	ksft.set_plan(len(cases) + 2)

	run_no_qos_case("no_qos baseline (start)", states)

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
			guard = ResumeLatencyGuard(cl.CPU, arg)
		run_case(desc, states, ceiling, guard)

	run_no_qos_case("no_qos baseline (end)", states)

	ksft.finished()


if __name__ == "__main__":
	main()
