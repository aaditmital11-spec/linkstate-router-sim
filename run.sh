#!/usr/bin/env bash
#
# run.sh - build the simulator, start routers 1 to 4, and watch their logs.
#
# Each router writes to logs/r<id>.log. Press Ctrl+C to stop: the trap sends
# SIGTERM to every router, which makes each one print its FINAL routing table
# before exiting, then waits for them all to finish.

# -e  stop on the first failing command
# -u  treat use of an unset variable as an error
# -o pipefail  a pipeline fails if any stage fails, not just the last one
set -euo pipefail

TOPOLOGY="topology.txt"
ROUTERS=(1 2 3 4)
LOG_DIR="logs"

# Run from the directory holding this script, so relative paths work no matter
# where it was invoked from.
cd "$(dirname "$0")"

make

mkdir -p "$LOG_DIR"

# PIDs of the routers we start, so the trap knows what to clean up.
pids=()

# PID of the log-following tail, empty until it is started.
tail_pid=""

# Called on Ctrl+C (SIGINT) and on SIGTERM.
shutdown() {
    echo
    echo "stopping routers..."

    # Stop following the logs first, so the final tables are not interleaved
    # with tail's output.
    if [ -n "$tail_pid" ]; then
        kill -TERM "$tail_pid" 2>/dev/null || true
    fi

    for pid in "${pids[@]}"; do
        # SIGTERM, not SIGKILL: the router catches it and prints its FINAL
        # table on the way out. A process that already exited makes kill fail,
        # which is not an error here.
        kill -TERM "$pid" 2>/dev/null || true
    done

    # Wait for each router to flush its final table and exit.
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    echo "all routers stopped. logs are in $LOG_DIR/"
    exit 0
}
trap shutdown INT TERM

echo "starting ${#ROUTERS[@]} routers using $TOPOLOGY"

for id in "${ROUTERS[@]}"; do
    ./router "$id" "$TOPOLOGY" > "$LOG_DIR/r$id.log" 2>&1 &
    pids+=("$!")
    echo "  router $id  pid $!  port $((5000 + id))  log $LOG_DIR/r$id.log"
done

echo
echo "following logs, press Ctrl+C to stop"
echo

#
# tail runs in the background and we block in "wait" rather than running tail
# in the foreground. This matters: bash defers a trap until the current
# foreground command finishes, so a foreground "tail -F" would swallow Ctrl+C
# forever and the trap would never run. The "wait" builtin, by contrast, is
# interrupted by a signal, which lets shutdown() execute immediately.
#
# -F keeps following even if a log is rotated or recreated.
tail -F "$LOG_DIR"/r*.log &
tail_pid=$!

wait "$tail_pid" || true

# Reached only if tail exited by itself; tidy up the routers all the same.
shutdown
