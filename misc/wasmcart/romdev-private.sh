#!/usr/bin/env bash
# romdev-private.sh: start a private romdev server on its own port (its own
# process, so its own GL context), under gdb so a native crash leaves a
# backtrace. For parallel test agents that must not share one GL context.
#   misc/wasmcart/romdev-private.sh <port> [log]   then ROMDEV_URL=http://127.0.0.1:<port>
#   misc/wasmcart/romdev-private.sh stop <port>
set -euo pipefail
R="${ROMDEV_DIR:?set ROMDEV_DIR to the romdevtools package (its src/mcp/server.js)}"
if [ "${1:-}" = stop ]; then
  pkill -f "[r]omdevtools/src/mcp/server.js --port $2\$" || true
  exit 0
fi
PORT="$1"
LOG="${2:-$HOME/.cache/romdev-private-$PORT.log}"
mkdir -p "$(dirname "$LOG")"
pkill -f "[r]omdevtools/src/mcp/server.js --port $PORT\$" || true
sleep 1
cd "$R"
ROMDEV_REEXEChild=1 setsid nohup gdb -batch \
  -ex 'handle SIGPIPE nostop noprint pass' -ex 'handle SIGUSR1 nostop noprint pass' \
  -ex 'handle SIGUSR2 nostop noprint pass' -ex 'handle SIGCHLD nostop noprint pass' \
  -ex run -ex 'echo \n==== CRASH BACKTRACE ====\n' -ex 'bt 60' \
  --args "$(command -v node)" ${ROMDEV_NODE_ARGS:-} --experimental-vm-modules "$R/src/mcp/server.js" --port "$PORT" > "$LOG" 2>&1 < /dev/null &
for i in $(seq 1 40); do
  ss -tln 2>/dev/null | grep -q ":$PORT " && { echo "romdev :$PORT up (log $LOG)"; exit 0; }
  sleep 1
done
echo "romdev :$PORT did not come up; see $LOG" >&2
exit 1
