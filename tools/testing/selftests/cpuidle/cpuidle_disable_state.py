#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
cpuidle: verify user disable of an idle state blocks selection.

Writes 1 to /sys/devices/system/cpu/cpuN/cpuidle/stateX/disable and checks
that stateX does not gain usage while other states still idle.
"""

from __future__ import annotations

import os
import sys
from typing import Dict, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpuidle_lib as cl
import ksft


def pick_disable_target(states: Dict[int, cl.IdleState]) -> Optional[int]:
	"""Prefer deepest user-enabled state; keep at least one shallower enabled."""
	ordered = sorted(states)
	if len(ordered) < 2:
		return None
	for idx in reversed(ordered):
		if idx == ordered[0]:
			continue
		s = states[idx]
		if s.disable_path and s.disable == 0:
			return idx
	return None


class DisableStateGuard:
	def __init__(self, path: str):
		self.path = path
		self.prev: Optional[str] = None

	def __enter__(self):
		self.prev = open(self.path).read().strip()
		with open(self.path, "w") as f:
			f.write("1\n")
		return self

	def __exit__(self, *args):
		if self.prev is None:
			return
		with open(self.path, "w") as f:
			f.write(f"{self.prev}\n")


def run_disable_case(desc: str, states: Dict[int, cl.IdleState],
		     target: int) -> None:
	s = states[target]
	ksft.print_msg(f"=== {desc} ===")
	ksft.print_msg(f"disable state{target}:{s.name} (lat={s.latency_us})")

	with DisableStateGuard(s.disable_path):
		ud = cl.measure_idle()

	total = sum(ud.values())
	cl.print_usage_table(states, ud, {target})
	ksft.print_msg(f"total_usage+={total} disabled_usage+={ud[target]}")

	if total <= 0:
		ksft.test_result_fail(f"{desc}: too little idle activity ({total})")
		return
	if ud[target] > 0:
		ksft.test_result_fail(
			f"{desc}: state{target}({s.name}) usage+={ud[target]} "
			"while user-disabled"
		)
		return
	ksft.test_result_pass(desc)


def run_reenable_case(desc: str, states: Dict[int, cl.IdleState],
		      target: int) -> None:
	"""After restore, confirm disable sysfs is cleared and idle still works."""
	s = states[target]
	ksft.print_msg(f"=== {desc} ===")
	ksft.print_msg(f"re-enabled state{target}:{s.name}")

	ud = cl.measure_idle()
	total = sum(ud.values())
	cl.print_usage_table(states, ud)
	ksft.print_msg(f"total_usage+={total} target_usage+={ud[target]}")

	if total <= 0:
		ksft.test_result_fail(f"{desc}: too little idle activity ({total})")
		return
	cur = int(open(s.disable_path).read())
	if cur != 0:
		ksft.test_result_fail(f"{desc}: disable sysfs still {cur}")
		return
	ksft.test_result_pass(desc)


def main() -> None:
	ksft.print_header()

	states = cl.require_root_and_cpuidle(min_states=2)
	# Need disable sysfs on states we might target.
	states = {i: s for i, s in states.items() if s.disable_path}
	if len(states) < 2:
		ksft.set_plan(1)
		ksft.test_result_skip("need at least 2 cpuidle states with disable")
		ksft.finished()

	cl.log_states(states, extra=True)
	cl.warn_preexisting_qos()

	target = pick_disable_target(states)
	if target is None:
		ksft.set_plan(1)
		ksft.test_result_skip("no user-enabled deep state to disable")
		ksft.finished()

	ksft.set_plan(2)
	run_disable_case(
		f"disable state{target} blocks selection", states, target
	)
	states = cl.read_states()
	run_reenable_case(
		f"restore state{target} disable", states, target
	)

	ksft.finished()


if __name__ == "__main__":
	main()
