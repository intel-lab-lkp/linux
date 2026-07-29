# SPDX-License-Identifier: GPL-2.0
"""Shared helpers for cpuidle kselftests."""

from __future__ import annotations

import glob
import os
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Set

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
				"..", "kselftest"))
import ksft


CPUIDLE_BASE = "/sys/devices/system/cpu"
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
	disable_path: str
	disable: int


def cpuidle_dir(cpu: int = CPU) -> str:
	return f"{CPUIDLE_BASE}/cpu{cpu}/cpuidle"


def read_states(cpu: int = CPU) -> Dict[int, IdleState]:
	base = cpuidle_dir(cpu)
	states: Dict[int, IdleState] = {}
	paths = sorted(
		glob.glob(f"{base}/state*"),
		key=lambda p: int(os.path.basename(p).replace("state", "")),
	)
	for path in paths:
		idx = int(os.path.basename(path).replace("state", ""))
		disable_path = f"{path}/disable"
		disable = 0
		if os.path.exists(disable_path):
			disable = int(open(disable_path).read())
		else:
			disable_path = ""
		states[idx] = IdleState(
			index=idx,
			name=open(f"{path}/name").read().strip(),
			latency_us=int(open(f"{path}/latency").read()),
			residency_us=int(open(f"{path}/residency").read()),
			usage=int(open(f"{path}/usage").read()),
			time_us=int(open(f"{path}/time").read()),
			disable_path=disable_path,
			disable=disable,
		)
	return states


def usage_delta(before: Dict[int, IdleState],
		after: Dict[int, IdleState]) -> Dict[int, int]:
	return {i: after[i].usage - before[i].usage for i in before}


def idle_on_cpu(seconds: float = IDLE_SEC) -> None:
	end = time.monotonic() + seconds
	burn_end = time.monotonic() + min(0.1, seconds / 10)
	while time.monotonic() < burn_end:
		pass
	while time.monotonic() < end:
		time.sleep(0.05)


def measure_idle(cpu: int = CPU,
		 seconds: float = IDLE_SEC) -> Dict[int, int]:
	"""Idle briefly and return per-state usage deltas."""
	time.sleep(0.05)
	before = read_states(cpu)
	idle_on_cpu(seconds)
	after = read_states(cpu)
	return usage_delta(before, after)


def fmt_state_list(states: Dict[int, IdleState], idxs: List[int]) -> str:
	if not idxs:
		return "(none)"
	return ", ".join(
		f"state{i}:{states[i].name}(lat={states[i].latency_us})"
		for i in idxs
	)


def print_usage_table(states: Dict[int, IdleState], udelta: Dict[int, int],
		      forbidden: Optional[Set[int]] = None) -> None:
	ksft.print_msg(
		f"{'idx':>3} {'name':<12} {'lat':>6} {'d_usage':>8} {'expect':>8}"
	)
	for i in sorted(states):
		s = states[i]
		if forbidden is None:
			expect = "allow"
		else:
			expect = "forbid" if i in forbidden else "allow"
		ksft.print_msg(
			f"{i:3d} {s.name:<12} {s.latency_us:6d} {udelta[i]:8d} {expect:>8}"
		)


def current_governor() -> str:
	path = f"{CPUIDLE_BASE}/cpuidle/current_governor"
	if not os.path.exists(path):
		return "?"
	return open(path).read().strip()


def require_root_and_cpuidle(min_states: int = 1) -> Dict[int, IdleState]:
	"""Skip and finish unless root with enough cpuidle states; else return them."""
	if os.geteuid() != 0:
		ksft.set_plan(1)
		ksft.test_result_skip("must run as root")
		ksft.finished()

	base = cpuidle_dir()
	if not os.path.isdir(base):
		ksft.set_plan(1)
		ksft.test_result_skip(f"no cpuidle sysfs at {base}")
		ksft.finished()

	states = read_states()
	if len(states) < min_states:
		ksft.set_plan(1)
		ksft.test_result_skip(
			f"need at least {min_states} cpuidle state(s), got {len(states)}"
		)
		ksft.finished()

	return states


def log_states(states: Dict[int, IdleState], extra: bool = False) -> None:
	ksft.print_msg(f"governor={current_governor()} cpu={CPU}")
	for i in sorted(states):
		s = states[i]
		msg = (f"state{i}: {s.name} latency={s.latency_us}us "
		       f"residency={s.residency_us}us")
		if extra:
			msg += f" disable={s.disable}"
		ksft.print_msg(msg)


DMA_LAT_DEV = "/dev/cpu_dma_latency"
WAKEUP_LAT_DEV = "/dev/cpu_wakeup_latency"
# Matches PM_QOS_CPU_LATENCY_DEFAULT_VALUE (2000 * USEC_PER_SEC).
CPU_DMA_LATENCY_DEFAULT = 2000 * 1000000
# Matches PM_QOS_RESUME_LATENCY_NO_CONSTRAINT / S32_MAX.
CPU_WAKEUP_LATENCY_DEFAULT = 0x7FFFFFFF


def _read_qos_dev_limit(path: str) -> Optional[int]:
	"""Open QoS miscdev, read aggregate limit (s32), close. None if unavailable."""
	try:
		fd = os.open(path, os.O_RDONLY)
	except OSError:
		return None
	try:
		raw = os.read(fd, 4)
	finally:
		os.close(fd)
	if len(raw) != 4:
		return None
	return int.from_bytes(raw, byteorder=sys.byteorder, signed=True)


def warn_preexisting_qos(cpu: int = CPU) -> None:
	"""Remind about preexisting global / per-CPU latency QoS settings."""
	notes: List[str] = []

	dma = _read_qos_dev_limit(DMA_LAT_DEV)
	if dma is not None and dma != CPU_DMA_LATENCY_DEFAULT:
		notes.append(f"/dev/cpu_dma_latency aggregate={dma}us (not default)")

	wake = _read_qos_dev_limit(WAKEUP_LAT_DEV)
	if wake is not None and wake != CPU_WAKEUP_LATENCY_DEFAULT:
		notes.append(
			f"/dev/cpu_wakeup_latency aggregate={wake}us (not default)"
		)

	resume_path = f"{CPUIDLE_BASE}/cpu{cpu}/power/pm_qos_resume_latency_us"
	if os.path.exists(resume_path):
		resume = open(resume_path).read().strip()
		# "0" means no constraint; "n/a" and positive N are constraints.
		if resume != "0":
			notes.append(f"{resume_path}={resume!r}")

	if not notes:
		ksft.print_msg("preexisting QoS: none (unconstrained)")
		return

	ksft.print_msg("WARNING: preexisting QoS may affect results:")
	for n in notes:
		ksft.print_msg(f"  - {n}")
	ksft.print_msg("  clear QoS holders / set resume to 0 if needed")


def warn_preexisting_disable(states: Dict[int, IdleState]) -> None:
	"""Remind about preexisting user-disabled idle states."""
	disabled = [
		f"state{i}:{s.name}"
		for i, s in sorted(states.items())
		if s.disable_path and s.disable != 0
	]
	if not disabled:
		ksft.print_msg("preexisting disable: none")
		return

	ksft.print_msg("WARNING: preexisting user-disabled states may affect results:")
	for name in disabled:
		ksft.print_msg(f"  - {name}")
	ksft.print_msg("  write 0 to stateX/disable to re-enable if needed")
