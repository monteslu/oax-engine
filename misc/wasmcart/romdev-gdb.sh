#!/usr/bin/env bash
# romdev-gdb.sh: (re)start the shared romdev server under gdb, so a native
# crash (in the GL driver, a native addon) leaves a backtrace in the log
# instead of just "died from SIGSEGV". Check for a human playtest first
# (stop the shared server first if one is running).
#   misc/wasmcart/romdev-gdb.sh [log file]   (default /tmp/rom-dev-mcp.log)
set -euo pipefail
LOG="${1:-/tmp/rom-dev-mcp.log}"
R="${ROMDEV_DIR:?set ROMDEV_DIR to the romdevtools package (its src/mcp/server.js)}"
pkill -f '[r]omdevtools/src/mcp/server.js$' || true   # the shared server only; private ones end in --port N
sleep 2
cd "$R"
ROMDEV_REEXEChild=1 setsid nohup gdb -batch \
  -ex 'handle SIGPIPE nostop noprint pass' -ex 'handle SIGUSR1 nostop noprint pass' \
  -ex 'handle SIGUSR2 nostop noprint pass' -ex 'handle SIGCHLD nostop noprint pass' \
  -ex run -ex 'echo \n==== CRASH BACKTRACE ====\n' -ex 'bt 60' -ex 'thread apply all bt 25' \
  --args "$(command -v node)" ${ROMDEV_NODE_ARGS:-} --experimental-vm-modules "$R/src/mcp/server.js" > "$LOG" 2>&1 < /dev/null &
for i in $(seq 1 30); do
  ss -tln 2>/dev/null | grep -q ':7331 ' && { echo "romdev up under gdb (log $LOG)"; exit 0; }
  sleep 1
done
echo "romdev did not come up; see $LOG" >&2
exit 1
