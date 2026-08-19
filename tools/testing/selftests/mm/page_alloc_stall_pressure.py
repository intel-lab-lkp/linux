#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
page_alloc_stall_pressure.py

Executes a multi-threaded userspace workload to consume and touch anonymous
memory. It serves as a workload generator to create system memory contention.

Mechanism:
1. Instantiates a primary memory hogger process to occupy a predetermined
   percentage of system memory, mimicking a low-watermark memory state.
2. Spawns multiple concurrent worker processes to intentionally overcommit the
   remaining memory, generating high-frequency page replacement and swapping
   activity.

Parameters:
 - hog_percent:      Percentage of available system memory to lock strictly
                     inside the primary memory hogger process. (Default: 50.0)
 - num_workers:      Number of concurrent worker processes mapping anonymous
                     memory. (Default: 64)
 - overcommit_ratio: Ratio of remaining free memory intentionally overcommitted
                     and mapped across all workers. (Default: 2.5)
 - memtoy_path:      Local filesystem path to the compiled memtoy binary.
                     (Source available at: https://github.com/kosaki/memtoy)
                     The binary is operated via stdin and must support commands
                     to allocate, map, and continuously touch anonymous memory
                     regions. (Default: "memtoy/memtoy")

Usage:
 python3 page_alloc_stall_pressure.py <hog_percent> <num_workers> \
          <overcommit_ratio> <memtoy_path>
"""

import os
import sys
import subprocess
import time
import threading

def get_mem_free_bytes():
    with open("/proc/meminfo") as f:
        for line in f:
            if line.startswith("MemFree:"):
                return int(line.split()[1]) * 1024
    return 0

def get_cgroup_root():
    # Check for both v2 and v1.
    if os.path.exists("/sys/fs/cgroup/cgroup.controllers"):
        return "/sys/fs/cgroup"
    if os.path.exists("/sys/fs/cgroup/memory"):
        return "/sys/fs/cgroup/memory"
    if os.path.exists("/dev/cgroup/memory"):
        return "/dev/cgroup/memory"

    raise FileNotFoundError("Neither Cgroup v1 nor v2 controllers found.")

def create_memcg(cg_name):
    cg_root = get_cgroup_root()
    path = os.path.join(cg_root, cg_name)
    try:
        os.makedirs(path, exist_ok=True)
    except Exception as e:
        print(f"Failed to create cgroup {path}: {e}")

def remove_memcg(cg_name):
    cg_root = get_cgroup_root()
    path = os.path.join(cg_root, cg_name)
    try:
        os.rmdir(path)
    except Exception as e:
        print(f"Failed to remove cgroup {path}: {e}")

def run_mem_hogger(cg_name, size_bytes, memtoy_path):
    cg_root = get_cgroup_root()
    try:
        full_cg_path = os.path.join(cg_root, cg_name)
        def assign_memcg():
            pid = os.getpid()
            with open(os.path.join(full_cg_path, "cgroup.procs"), "w") as f:
                f.write(str(pid))
            # Protect it from the OOM killing
            with open(f"/proc/{pid}/oom_score_adj", "w") as f:
                f.write("-1000")

        p = subprocess.Popen([memtoy_path],
                             preexec_fn=assign_memcg,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, universal_newlines=True)

        size_mb = int(size_bytes / 1024 / 1024)

        p.stdin.write(f"anon region {size_mb}m\n")
        p.stdin.write("map region\n")
        p.stdin.write("lock region\n") # Do not swap out the hogged memory
        p.stdin.write("touch region write 1\n")
        p.stdin.flush()

        for line in p.stdout:
            if "touched" in line:
                print(f"Mem Hogger (PID {p.pid}) allocated {size_mb} MB RAM.")
                break

        return p
    except Exception as e:
        print(f"Failed to run Mem Hogger: {e}")

class WorkerThread(threading.Thread):
    def __init__(self, name, cg_name, size_bytes, memtoy_path):
        super().__init__(name=name)
        self.cg_name = cg_name
        self.size_bytes = size_bytes
        self.process = None
        self.daemon = True
        self.should_stop = False
        self.memtoy_path = memtoy_path

    def run(self):
        full_cg_path = os.path.join(get_cgroup_root(), self.cg_name)
        def assign_memcg():
            with open(os.path.join(full_cg_path, "cgroup.procs"), "w") as f:
                f.write(str(os.getpid()))

        # Allocating a region of memory with size_bytes.
        while not self.should_stop:
            try:
                self.process = subprocess.Popen([self.memtoy_path],
                                                preexec_fn=assign_memcg,
                                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                                universal_newlines=True)
                size_mb = int(self.size_bytes / 1024 / 1024)

                self.process.stdin.write(f"anon region {size_mb}m\n")
                self.process.stdin.write("map region\n")
                self.process.stdin.write("touch region write 1\n")
                self.process.stdin.flush()

                print(f"Worker {self.name} (PID {self.process.pid}) allocated {size_mb} MB RAM.")

                while True:
                    try:
                        # Access the newly allocated memory continuously.
                        # If it get killed during stdin write, it is ok.
                        self.process.stdin.write("touch region read\n")
                        self.process.stdin.flush()
                    except (BrokenPipeError, ValueError):
                        break

                    for line in self.process.stdout:
                        if "touched" in line:
                            break

                    if self.process.poll() is not None:
                        break

            except Exception as e:
                print(f"Worker {self.name} process terminated: {e}. Respawning.")
                if self.process:
                    try:
                        self.process.terminate()
                    except Exception:
                        pass
                self.process = None
                time.sleep(0.1) # Don't respawn too fast

def main(hog_percent, num_workers, overcommit_ratio, memtoy_path):
    free_mem_bytes = get_mem_free_bytes()
    hogger_bytes = int(free_mem_bytes * (hog_percent / 100))
    remain_bytes = free_mem_bytes - hogger_bytes

    hogger_memcg_name = "mem_hogger"

    print(f"Allocating {hogger_bytes} for the memory hogger.")

    create_memcg(hogger_memcg_name)
    hogger_process = run_mem_hogger(hogger_memcg_name, hogger_bytes, memtoy_path)

    worker_size_bytes = int(remain_bytes / num_workers * overcommit_ratio)
    print(f"Spawning {num_workers} with {worker_size_bytes} memory allocation each.")

    threads = []
    for i in range(num_workers):
        worker_cg_name = f"worker_cg_{i}"
        create_memcg(worker_cg_name)
        t = WorkerThread(f"Worker_{i}", worker_cg_name, worker_size_bytes, memtoy_path)
        threads.append(t)
        t.start()
        time.sleep(0.05)

    try:
        print("All memory loads are created. Will run for 10mins, or Ctrl-C to exit.")
        start_time = time.time()
        while time.time() - start_time < 600:
            time.sleep(10)
    except KeyboardInterrupt:
        print("Got Ctrl-C. Exiting.")
    finally:
        print("Cleaning the processes and cgroups...")

        if hogger_process:
            try:
                hogger_process.terminate()
                if hogger_process.stdin: hogger_process.stdin.close()
                if hogger_process.stdout: hogger_process.stdout.close()
                hogger_process.wait()
            except Exception:
                pass

        for t in threads:
            t.should_stop = True
            if t.process:
                try:
                    t.process.terminate()
                    if t.process.stdin: t.process.stdin.close()
                    if t.process.stdout: t.process.stdout.close()
                    t.process.wait()
                except Exception:
                    pass

        # let the kernel settle down
        time.sleep(1)

        remove_memcg(hogger_memcg_name)
        for i in range(num_workers):
            remove_memcg(f"worker_cg_{i}")

        print("Cleanup done.")

        return 0

if __name__ == "__main__":
    hog_percent = float(sys.argv[1]) if len(sys.argv) > 1 else 50.0
    num_workers = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    overcommit_ratio = float(sys.argv[3]) if len(sys.argv) > 3 else 2.5
    memtoy_path = sys.argv[4] if len(sys.argv) > 4 else "memtoy/memtoy"
    sys.exit(main(hog_percent, num_workers, overcommit_ratio, memtoy_path))
