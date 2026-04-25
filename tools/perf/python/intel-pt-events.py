#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Print Intel PT Events including Power Events and PTWRITE.
Ported from tools/perf/scripts/python/intel-pt-events.py
"""

import argparse
import contextlib
from ctypes import addressof, create_string_buffer
import io
import os
import struct
import sys
from typing import Any, Optional
import perf

# Try to import LibXED from legacy directory if available in PYTHONPATH
try:
    from libxed import LibXED  # type: ignore
except ImportError:
    LibXED = None  # type: ignore


class IntelPTAnalyzer:
    """Analyzes Intel PT events and prints details."""

    def __init__(self, cfg: argparse.Namespace):
        self.args = cfg
        self.session: Optional[perf.session] = None
        self.insn = False
        self.src = False
        self.source_file_name: Optional[str] = None
        self.line_number: int = 0
        self.dso: Optional[str] = None
        self.stash_dict: dict[int, list[str]] = {}
        self.output: Any = None
        self.output_pos: int = 0
        self.cpu: int = -1
        self.time: int = 0
        self.switch_str: dict[int, str] = {}

        if cfg.insn_trace:
            print("Intel PT Instruction Trace")
            self.insn = True
        elif cfg.src_trace:
            print("Intel PT Source Trace")
            self.insn = True
            self.src = True
        else:
            print("Intel PT Branch Trace, Power Events, Event Trace and PTWRITE")

        self.disassembler: Any = None
        if self.insn and LibXED is not None:
            try:
                self.disassembler = LibXED()
            except Exception as e:
                print(f"Failed to initialize LibXED: {e}")
                self.disassembler = None

    def print_ptwrite(self, raw_buf: bytes) -> None:
        """Print PTWRITE data."""
        data = struct.unpack_from("<IQ", raw_buf)
        flags = data[0]
        payload = data[1]
        exact_ip = flags & 1
        try:
            s = payload.to_bytes(8, "little").decode("ascii").rstrip("\x00")
            if not s.isprintable():
                s = ""
        except (UnicodeDecodeError, ValueError):
            s = ""
        print(f"IP: {exact_ip} payload: {payload:#x} {s}", end=' ')

    def print_cbr(self, raw_buf: bytes) -> None:
        """Print CBR data."""
        if len(raw_buf) < 12:
            return
        data = struct.unpack_from("<BBBBII", raw_buf)
        cbr = data[0]
        f = (data[4] + 500) // 1000
        if data[2] == 0:
            return
        p = ((cbr * 1000 // data[2]) + 5) // 10
        print(f"{cbr:3u}  freq: {f:4u} MHz  ({p:3u}%)", end=' ')

    def print_mwait(self, raw_buf: bytes) -> None:
        """Print MWAIT data."""
        data = struct.unpack_from("<IQ", raw_buf)
        payload = data[1]
        hints = payload & 0xff
        extensions = (payload >> 32) & 0x3
        print(f"hints: {hints:#x} extensions: {extensions:#x}", end=' ')

    def print_pwre(self, raw_buf: bytes) -> None:
        """Print PWRE data."""
        data = struct.unpack_from("<IQ", raw_buf)
        payload = data[1]
        hw = (payload >> 7) & 1
        cstate = (payload >> 12) & 0xf
        subcstate = (payload >> 8) & 0xf
        print(f"hw: {hw} cstate: {cstate} sub-cstate: {subcstate}", end=' ')

    def print_exstop(self, raw_buf: bytes) -> None:
        """Print EXSTOP data."""
        data = struct.unpack_from("<I", raw_buf)
        flags = data[0]
        exact_ip = flags & 1
        print(f"IP: {exact_ip}", end=' ')

    def print_pwrx(self, raw_buf: bytes) -> None:
        """Print PWRX data."""
        data = struct.unpack_from("<IQ", raw_buf)
        payload = data[1]
        deepest_cstate = payload & 0xf
        last_cstate = (payload >> 4) & 0xf
        wake_reason = (payload >> 8) & 0xf
        print(f"deepest cstate: {deepest_cstate} last cstate: {last_cstate} "
              f"wake reason: {wake_reason:#x}", end=' ')

    def print_psb(self, raw_buf: bytes) -> None:
        """Print PSB data."""
        data = struct.unpack_from("<IQ", raw_buf)
        offset = data[1]
        print(f"offset: {offset:#x}", end=' ')

    def print_evt(self, raw_buf: bytes) -> None:
        """Print EVT data."""
        glb_cfe = ["", "INTR", "IRET", "SMI", "RSM", "SIPI", "INIT", "VMENTRY", "VMEXIT",
                   "VMEXIT_INTR", "SHUTDOWN", "", "UINT", "UIRET"] + [""] * 18
        glb_evd = ["", "PFA", "VMXQ", "VMXR"] + [""] * 60

        data = struct.unpack_from("<BBH", raw_buf)
        typ = data[0] & 0x1f
        ip_flag = (data[0] & 0x80) >> 7
        vector = data[1]
        evd_cnt = data[2]
        s = glb_cfe[typ]
        if s:
            print(f" cfe: {s} IP: {ip_flag} vector: {vector}", end=' ')
        else:
            print(f" cfe: {typ} IP: {ip_flag} vector: {vector}", end=' ')
        pos = 4
        for _ in range(evd_cnt):
            if len(raw_buf) < pos + 16:
                break
            data = struct.unpack_from("<QQ", raw_buf, pos)
            et = data[0] & 0x3f
            s = glb_evd[et]
            if s:
                print(f"{s}: {data[1]:#x}", end=' ')
            else:
                print(f"EVD_{et}: {data[1]:#x}", end=' ')
            pos += 16

    def print_iflag(self, raw_buf: bytes) -> None:
        """Print IFLAG data."""
        data = struct.unpack_from("<IQ", raw_buf)
        iflag = data[0] & 1
        old_iflag = iflag ^ 1
        via_branch = data[0] & 2
        s = "via" if via_branch else "non"
        print(f"IFLAG: {old_iflag}->{iflag} {s} branch", end=' ')

    def common_start_str(self, comm: str, sample: perf.sample_event) -> str:
        """Return common start string for display."""
        ts = sample.sample_time
        cpu = sample.sample_cpu
        pid = sample.sample_pid
        tid = sample.tid
        machine_pid = getattr(sample, "machine_pid", 0)
        if machine_pid:
            vcpu = getattr(sample, "vcpu", -1)
            return (f"VM:{machine_pid:5d} VCPU:{vcpu:03d} {comm:>16s} {pid:5u}/{tid:<5u} "
                    f"[{cpu:03u}] {ts // 1000000000:9u}.{ts % 1000000000:09u}  ")
        return (f"{comm:>16s} {pid:5u}/{tid:<5u} [{cpu:03u}] "
                f"{ts // 1000000000:9u}.{ts % 1000000000:09u}  ")

    def print_common_start(self, comm: str, sample: perf.sample_event, name: str) -> None:
        """Print common start info."""
        flags_disp = getattr(sample, "flags_disp", "")
        print(self.common_start_str(comm, sample) + f"{name:>8s}  {flags_disp:>21s}", end=' ')

    def print_instructions_start(self, comm: str, sample: perf.sample_event) -> None:
        """Print instructions start info."""
        flags = getattr(sample, "flags_disp", "")
        if "x" in flags:
            print(self.common_start_str(comm, sample) + "x", end=' ')
        else:
            print(self.common_start_str(comm, sample), end='  ')

    def disassem(self, insn: bytes, ip: int) -> tuple[int, str]:
        """Disassemble instruction using LibXED."""
        inst = self.disassembler.instruction()
        self.disassembler.set_mode(inst, 0)  # Assume 64-bit
        buf = create_string_buffer(insn, 64)
        return self.disassembler.disassemble_one(inst, addressof(buf), len(insn), ip)

    def print_common_ip(self, sample: perf.sample_event, symbol: str, dso: str) -> None:
        """Print IP and symbol info."""
        ip = sample.sample_ip
        offs = f"+{sample.symoff:#x}" if hasattr(sample, "symoff") else ""
        cyc_cnt = getattr(sample, "cyc_cnt", 0)
        if cyc_cnt:
            insn_cnt = getattr(sample, "insn_cnt", 0)
            ipc_str = f"  IPC: {insn_cnt / cyc_cnt:#.2f} ({insn_cnt}/{cyc_cnt})"
        else:
            ipc_str = ""

        if self.insn and self.disassembler is not None:
            try:
                insn = sample.insn()
            except AttributeError:
                insn = None
            if insn:
                cnt, text = self.disassem(insn, ip)
                byte_str = (f"{ip:x}").rjust(16)
                for k in range(cnt):
                    byte_str += f" {insn[k]:02x}"
                print(f"{byte_str:-40s}  {text:-30s}", end=' ')
            print(f"{symbol}{offs} ({dso})", end=' ')
        else:
            print(f"{ip:16x} {symbol}{offs} ({dso})", end=' ')

        addr_correlates_sym = getattr(sample, "addr_correlates_sym", False)
        if addr_correlates_sym:
            addr = sample.addr
            addr_dso = getattr(sample, "addr_dso", "[unknown]")
            addr_symbol = getattr(sample, "addr_symbol", "[unknown]")
            addr_offs = f"+{sample.addr_symoff:#x}" if hasattr(sample, "addr_symoff") else ""
            print(f"=> {addr:x} {addr_symbol}{addr_offs} ({addr_dso}){ipc_str}")
        else:
            print(ipc_str)

    def print_srccode(self, comm: str, sample: perf.sample_event,
                      symbol: str, dso: str, with_insn: bool) -> None:
        """Print source code info."""
        ip = sample.sample_ip
        if symbol == "[unknown]":
            start_str = self.common_start_str(comm, sample) + (f"{ip:x}").rjust(16).ljust(40)
        else:
            offs = f"+{sample.symoff:#x}" if hasattr(sample, "symoff") else ""
            start_str = self.common_start_str(comm, sample) + (symbol + offs).ljust(40)

        if with_insn and self.insn and self.disassembler is not None:
            try:
                insn = sample.insn()
            except AttributeError:
                insn = None
            if insn:
                _, text = self.disassem(insn, ip)
                start_str += text.ljust(30)

        try:
            source_file_name, line_number, source_line = sample.srccode()
        except (AttributeError, ValueError):
            source_file_name, line_number, source_line = None, 0, None

        if source_file_name:
            if self.line_number == line_number and self.source_file_name == source_file_name:
                src_str = ""
            else:
                if len(source_file_name) > 40:
                    src_file = ("..." + source_file_name[-37:]) + " "
                else:
                    src_file = source_file_name.ljust(41)
                if source_line is None:
                    src_str = src_file + str(line_number).rjust(4) + " <source not found>"
                else:
                    src_str = src_file + str(line_number).rjust(4) + " " + source_line
            self.dso = None
        elif dso == self.dso:
            src_str = ""
        else:
            src_str = dso
            self.dso = dso

        self.line_number = line_number
        self.source_file_name = source_file_name
        print(start_str, src_str)

    def do_process_event(self, sample: perf.sample_event) -> None:
        """Process event and print info."""
        comm = "Unknown"
        if hasattr(self, 'session') and self.session:
            try:
                comm = self.session.find_thread(sample.sample_pid).comm()
            except Exception:
                pass
        name = getattr(sample.evsel, 'name', str(sample.evsel))
        if name.startswith("evsel("):
            name = name[6:-1]
        dso = getattr(sample, "dso", "[unknown]")
        symbol = getattr(sample, "symbol", "[unknown]")

        cpu = sample.sample_cpu
        if cpu in self.switch_str:
            print(self.switch_str[cpu])
            del self.switch_str[cpu]

        try:
            raw_buf = sample.raw_buf
        except AttributeError:
            raw_buf = b""

        if name.startswith("instructions"):
            if self.src:
                self.print_srccode(comm, sample, symbol, dso, True)
            else:
                self.print_instructions_start(comm, sample)
                self.print_common_ip(sample, symbol, dso)
        elif name.startswith("branches"):
            if self.src:
                self.print_srccode(comm, sample, symbol, dso, False)
            else:
                self.print_common_start(comm, sample, name)
                self.print_common_ip(sample, symbol, dso)
        elif name == "ptwrite":
            self.print_common_start(comm, sample, name)
            self.print_ptwrite(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "cbr":
            self.print_common_start(comm, sample, name)
            self.print_cbr(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "mwait":
            self.print_common_start(comm, sample, name)
            self.print_mwait(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "pwre":
            self.print_common_start(comm, sample, name)
            self.print_pwre(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "exstop":
            self.print_common_start(comm, sample, name)
            self.print_exstop(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "pwrx":
            self.print_common_start(comm, sample, name)
            self.print_pwrx(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "psb":
            self.print_common_start(comm, sample, name)
            self.print_psb(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "evt":
            self.print_common_start(comm, sample, name)
            self.print_evt(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        elif name == "iflag":
            self.print_common_start(comm, sample, name)
            self.print_iflag(raw_buf)
            self.print_common_ip(sample, symbol, dso)
        else:
            self.print_common_start(comm, sample, name)
            self.print_common_ip(sample, symbol, dso)

    def interleave_events(self, sample: perf.sample_event) -> None:
        """Interleave output to avoid garbled lines from different CPUs."""
        self.cpu = sample.sample_cpu
        ts = sample.sample_time

        if self.time != ts:
            self.time = ts
            self.flush_stashed_output()

        self.output_pos = 0
        with contextlib.redirect_stdout(io.StringIO()) as self.output:
            self.do_process_event(sample)

        self.stash_output()

    def stash_output(self) -> None:
        """Stash output for later flushing."""
        output_str = self.output.getvalue()[self.output_pos:]
        n = len(output_str)
        if n:
            self.output_pos += n
            if self.cpu not in self.stash_dict:
                self.stash_dict[self.cpu] = []
            self.stash_dict[self.cpu].append(output_str)
            if len(self.stash_dict[self.cpu]) > 1000:
                self.flush_stashed_output()

    def flush_stashed_output(self) -> None:
        """Flush stashed output."""
        while self.stash_dict:
            cpus = list(self.stash_dict.keys())
            for cpu in cpus:
                items = self.stash_dict[cpu]
                countdown = self.args.interleave
                while len(items) and countdown:
                    sys.stdout.write(items[0])
                    del items[0]
                    countdown -= 1
                if not items:
                    del self.stash_dict[cpu]

    def process_event(self, sample: perf.sample_event) -> None:
        """Wrapper to handle interleaving and exceptions."""
        try:
            if self.args.interleave:
                self.interleave_events(sample)
            else:
                self.do_process_event(sample)
        except BrokenPipeError:
            # Stop python printing broken pipe errors and traceback
            sys.stdout = open(os.devnull, 'w', encoding='utf-8')
            sys.exit(1)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    ap.add_argument("--insn-trace", action='store_true')
    ap.add_argument("--src-trace", action='store_true')
    ap.add_argument("--all-switch-events", action='store_true')
    ap.add_argument("--interleave", type=int, nargs='?', const=4, default=0)
    args = ap.parse_args()

    analyzer = IntelPTAnalyzer(args)

    try:
        session = perf.session(perf.data(args.input), sample=analyzer.process_event)
        analyzer.session = session
        session.process_events()
        if args.interleave:
            analyzer.flush_stashed_output()
        print("End")
    except KeyboardInterrupt:
        if args.interleave:
            analyzer.flush_stashed_output()
        print("End")
    except Exception as e:
        print(f"Error processing events: {e}")
