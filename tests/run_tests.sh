#!/usr/bin/env bash
# Integration test for the user-space part (no kernel module needed).
# Starts fake_sensor (ramping temperature) + sensor_daemon, then drives the
# daemon through monitor_client and checks the replies and the log files.
set -u
cd "$(dirname "$0")/.."
BIN=build
PORT=${PORT:-5599}
TMP=$(mktemp -d)
FIFO=$TMP/sensor.fifo
LOGDIR=$TMP/logs
PASS=0; FAIL=0

cleanup() {
    kill "${DPID:-}" "${FPID:-}" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

check() {  # check <name> <regex> <actual>
    if [[ "$3" =~ $2 ]]; then echo "  PASS  $1"; PASS=$((PASS+1))
    else echo "  FAIL  $1"; echo "        expected /$2/, got: $3"; FAIL=$((FAIL+1)); fi
}
client() { $BIN/monitor_client --port "$PORT" "$@" 2>&1; }

make -s all || { echo "build failed"; exit 1; }

$BIN/fake_sensor "$FIFO" 100 ramp &  FPID=$!
sleep 0.3
$BIN/sensor_daemon --device "$FIFO" --port "$PORT" --log-dir "$LOGDIR" --threshold 35 2>/dev/null &
DPID=$!
sleep 1.5   # ramp crosses 35 C after ~6 samples

echo "Running tests..."
check "GET returns a reading"          '^OK seq=[0-9]+ temp=[0-9.]+ hum=50\.00 ts='  "$(client GET)"
check "STATUS reports ALERT state"     'state=ALERT .*alerts=1 '                     "$(client STATUS)"
check "SET_THRESHOLD accepted"         '^OK threshold=80\.00$'                       "$(client SET_THRESHOLD 80)"
sleep 0.5
check "state returns to NORMAL"        'state=NORMAL'                                "$(client STATUS)"
check "SET_THRESHOLD rejects garbage"  '^ERR'                                        "$(client SET_THRESHOLD abc)"
check "SET_INTERVAL fails on a FIFO"   '^ERR set interval failed'                    "$(client SET_INTERVAL 500)"
check "unknown command rejected"       '^ERR unknown command'                        "$(client BOGUS)"
check "lowercase command accepted"     '^OK seq='                                    "$(client get)"

kill -TERM "$DPID"; wait "$DPID" 2>/dev/null; RC=$?
check "daemon exits with status 0 on SIGTERM" '^0$' "$RC"
LINES=$(wc -l < "$LOGDIR/readings.csv")
check "readings.csv has header + rows" '^([6-9]|[1-9][0-9]+)$' "$LINES"
check "alerts.log contains ALERT"      'ALERT temp='  "$(cat "$LOGDIR/alerts.log")"

echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ]
