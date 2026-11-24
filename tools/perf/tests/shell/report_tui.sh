#!/bin/bash
# perf report and annotate TUI tests
# SPDX-License-Identifier: GPL-2.0

set -e

# shellcheck source=lib/setup_python.sh
. "$(dirname "$0")/lib/setup_python.sh"

if ! perf check feature -q libslang; then
	echo "[Skip] No libslang support"
	exit 2
fi

err=0
tmpdir=$(mktemp -d /tmp/__perf_test_tui.XXXXXX)
perfdata="${tmpdir}/perf.data"
tui_keys="${tmpdir}/tui_keys.py"

cleanup() {
	rm -rf "${tmpdir}"

	trap - EXIT TERM INT
}

trap_cleanup() {
	echo "Unexpected signal in ${FUNCNAME[1]}"
	cleanup
	exit 1
}
trap trap_cleanup EXIT TERM INT

# The TUI needs a terminal, so run it on a pty and feed it the keys a
# user would press.
cat > "${tui_keys}" <<'PY'
import fcntl, os, pty, select, struct, sys, termios, time

outfile = sys.argv[1]
keys = sys.argv[2].encode()
cmd = sys.argv[3:]

pid, fd = pty.fork()
if pid == 0:
	os.environ["TERM"] = "xterm"
	os.execvp(cmd[0], cmd)

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

start = time.time()
next_key = start + 1.5
deadline = start + 20
with open(outfile, "wb") as out:
	while True:
		now = time.time()
		if now > deadline:
			os.kill(pid, 9)
			sys.exit(2)
		if keys and now >= next_key:
			os.write(fd, keys[:1])
			keys = keys[1:]
			next_key = now + 0.7
		r, _, _ = select.select([fd], [], [], 0.1)
		if r:
			try:
				chunk = os.read(fd, 65536)
			except OSError:
				break
			if not chunk:
				break
			out.write(chunk)
			out.flush()

_, status = os.waitpid(pid, 0)
sys.exit(os.waitstatus_to_exitcode(status))
PY

run_tui() {
	local desc="$1" keys="$2" expect="$3"

	shift 3
	echo "TUI ${desc}"
	if ! "${PYTHON}" "${tui_keys}" "${tmpdir}/${desc}.out" "${keys}" "$@"; then
		echo "TUI ${desc} [Failed: TUI exited with an error]"
		err=1
		return
	fi
	if ! grep -qa "${expect}" "${tmpdir}/${desc}.out"; then
		echo "TUI ${desc} [Failed: missing '${expect}']"
		err=1
		return
	fi
	echo "TUI ${desc} [Success]"
}

echo "TUI record"
if ! perf record -q -o "${perfdata}" -- perf test -w noploop 1; then
	echo "TUI record [Failed: perf record failed]"
	cleanup
	exit 1
fi

run_tui "report" "q" "Overhead" perf report --tui -i "${perfdata}"
run_tui "report help" "?qq" "Help" perf report --tui -i "${perfdata}"
run_tui "report dso zoom" "dq" "Overhead" perf report --tui -i "${perfdata}"
run_tui "report thread zoom" "tq" "Overhead" perf report --tui -i "${perfdata}"
run_tui "annotate" "q" "Percent" perf annotate --tui -i "${perfdata}"
# noploop is built without debug info, so 's' opens the "no source code"
# warning and the first 'q' only dismisses it; the second one quits.
run_tui "annotate source" "sqq" "Percent" perf annotate --tui -i "${perfdata}"

cleanup
exit $err
