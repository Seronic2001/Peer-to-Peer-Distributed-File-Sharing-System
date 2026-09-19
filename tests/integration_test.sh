#!/usr/bin/env bash
# ===========================================================================
# End-to-end integration test for the P2P file sharing system.
#
# Topology: 2 trackers (primary + backup), several clients driven through
# stdin FIFOs so commands can be injected at precise moments.
#
# PORT PLAN (avoid collisions with the trackers' replication listeners,
# which occupy client_port + 1000 = 6101/6102):
#   tracker client ports:  5101, 5102
#   tracker replication:   6101, 6102 (derived, must stay clear)
#   client peer ports:     7101-7106
#
# Scenarios:
#   S1  Single-seeder transfer (rarest-first): byte-identical results.
#   S2  Multi-peer download: third client pulls from two seeders at once.
#   S3  Tracker failover: kill primary; backup promotes and serves clients.
#   S4  Corrupt-source resilience: seeder's bytes change mid-download;
#       hash verification must reject bad pieces.
#
# Usage: bash tests/integration_test.sh [--keep]
# ===========================================================================
set -u

KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="${TMPDIR:-/tmp}/p2p_itest"
LOGS="$RUN/logs"
mkdir -p "$LOGS"

PASS=0
FAIL=0
FAILED_SCENARIOS=()

pass() { echo "[PASS] $*"; PASS=$((PASS + 1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); FAILED_SCENARIOS+=("$*"); }

# ------------------------------------------------------------- processes --

declare -a ALL_PIDS=()
declare -A CLIENT_PID=()

kill_all() {
  for pid in "${ALL_PIDS[@]:-}"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
  done
  # -x matches the exact process name; -f would also match this shell's own
  # command line and kill the test harness itself.
  pkill -x tracker 2>/dev/null
  pkill -x client  2>/dev/null
}

cleanup() {
  kill_all
  if [ "$KEEP" -eq 0 ]; then
    rm -rf "$RUN"
  else
    echo "(logs kept in $LOGS)"
  fi
}
trap cleanup EXIT

pkill -x tracker 2>/dev/null
pkill -x client  2>/dev/null
sleep 0.5

# --------------------------------------------------------------- helpers --

wait_for_port() {
  local port="$1" tries="${2:-50}"
  # Each attempt is bounded by `timeout` so a wedged probe can never hang
  # the harness; failures just retry.
  for _ in $(seq "$tries"); do
    if timeout 2 bash -c "(exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null"; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

wait_for_line() { # wait_for_line <file> <pattern> [tries]
  local file="$1" pattern="$2" tries="${3:-150}"
  for _ in $(seq "$tries"); do
    if grep -qE "$pattern" "$file" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

# Send a command to a client's stdin FIFO. Fails fast (instead of blocking
# forever) if the client process is gone and nothing will ever read the FIFO.
send() { # send <client-name> <command>
  local name="$1" cmd="$2"
  if [ -z "${CLIENT_PID[$name]:-}" ] || ! kill -0 "${CLIENT_PID[$name]}" 2>/dev/null; then
    fail "send to '$name' aborted: client process is dead (cmd: $cmd)"
    return 1
  fi
  echo "$cmd" >>"$RUN/$name.cmdlog"
  timeout 5 bash -c 'echo "$1" > "$2"' _ "$cmd" "$RUN/$name.in" || {
    fail "send to '$name' timed out (cmd: $cmd)"
    return 1
  }
}

start_trackers() {
  "$ROOT/tracker/tracker" "$RUN/tracker_info.txt" 1 >"$LOGS/tracker1.log" 2>&1 &
  ALL_PIDS+=($!)
  wait_for_port 5101 || { echo "tracker1 did not start"; tail -5 "$LOGS/tracker1.log"; exit 1; }
  "$ROOT/tracker/tracker" "$RUN/tracker_info.txt" 2 >"$LOGS/tracker2.log" 2>&1 &
  ALL_PIDS+=($!)
  wait_for_port 5102 || { echo "tracker2 did not start"; tail -5 "$LOGS/tracker2.log"; exit 1; }
  sleep 1  # let tracker2 elect as backup
}

# start_client <name> <peer_port>
start_client() {
  local name="$1" port="$2"
  mkfifo "$RUN/$name.in" 2>/dev/null
  # Hold the write end open so the client never sees EOF on stdin.
  (exec 9>"$RUN/$name.in"; sleep 3600) &
  ALL_PIDS+=($!)
  "$ROOT/client/client" "127.0.0.1:$port" "$RUN/tracker_info.txt" \
    <"$RUN/$name.in" >"$RUN/$name.out" 2>&1 &
  CLIENT_PID[$name]=$!
  ALL_PIDS+=(${CLIENT_PID[$name]})
}

register_users() {
  local u
  for u in "$@"; do
    send "$u" "create_user $u pw_$u" || return 1
    send "$u" "login $u pw_$u" || return 1
  done
  sleep 1
}

check_login() { # check_login <name>
  if grep -q "Login successful" "$RUN/$1.out" 2>/dev/null; then
    return 0
  fi
  fail "$1 login failed"; tail -3 "$RUN/$1.out" 2>/dev/null
  return 1
}

# ==================================================================== run ==

echo "==> Building project..."
make -C "$ROOT" >/dev/null 2>&1 || { echo "Build failed"; exit 1; }

echo "==> Topology: 2 trackers + clients, scenario-driven"

cat >"$RUN/tracker_info.txt" <<'EOF'
127.0.0.1:5101
127.0.0.1:5102
EOF

# ------------------------------------------------------------------ S1 ----
echo ""
echo "==> S1: single-seeder download (rarest-first)"

start_trackers

start_client alice 7101
start_client bob   7102
start_client carol 7103
register_users alice bob carol || true
check_login alice; check_login bob; check_login carol

send alice "create_group g1"
sleep 0.5
send bob   "join_group g1"
send carol "join_group g1"
sleep 0.5
send alice "accept_request g1 bob"
send alice "accept_request g1 carol"
sleep 0.5

# ~1.5 MB random file -> 3 pieces.
dd if=/dev/urandom of="$RUN/orig1.bin" bs=1M count=1 2>/dev/null
dd if=/dev/urandom of="$RUN/orig1.bin" bs=512K count=1 seek=2 conv=notrunc 2>/dev/null

send alice "upload_file g1 $RUN/orig1.bin"
if wait_for_line "$RUN/alice.out" "File shared successfully" 50; then
  pass "S1: upload accepted by tracker"
else
  fail "S1: upload_file not confirmed"; tail -3 "$RUN/alice.out"
fi

mkdir -p "$RUN/dl_bob1" "$RUN/dl_carol1"
send bob   "download_file g1 orig1.bin $RUN/dl_bob1/orig1.bin rarest"
send carol "download_file g1 orig1.bin $RUN/dl_carol1/orig1.bin rarest"

if wait_for_line "$RUN/bob.out" "Download of 'orig1.bin' complete" 100; then
  if cmp -s "$RUN/orig1.bin" "$RUN/dl_bob1/orig1.bin"; then
    pass "S1: bob downloaded byte-identical file"
  else
    fail "S1: bob's copy differs from the original"
  fi
else
  fail "S1: bob download did not complete"; tail -5 "$RUN/bob.out"
fi

if wait_for_line "$RUN/carol.out" "Download of 'orig1.bin' complete" 100; then
  if cmp -s "$RUN/orig1.bin" "$RUN/dl_carol1/orig1.bin"; then
    pass "S1: carol downloaded byte-identical file"
  else
    fail "S1: carol's copy differs from the original"
  fi
else
  fail "S1: carol download did not complete"; tail -5 "$RUN/carol.out"
fi

# ------------------------------------------------------------------ S2 ----
echo ""
echo "==> S2: multi-peer download from two seeders"

# 3 MB file -> 6 pieces. Alice uploads; bob downloads (becomes 2nd seeder);
# carol then downloads with both alice and bob as sources.
dd if=/dev/urandom of="$RUN/orig2.bin" bs=1M count=3 2>/dev/null
send alice "upload_file g1 $RUN/orig2.bin"
wait_for_line "$RUN/alice.out" "File shared successfully" 50 >/dev/null

mkdir -p "$RUN/dl_bob2"
send bob "download_file g1 orig2.bin $RUN/dl_bob2/orig2.bin rarest"
if wait_for_line "$RUN/bob.out" "Download of 'orig2.bin' complete" 150; then
  pass "S2: bob acquired the file (second seeder ready)"
else
  fail "S2: bob failed to acquire orig2.bin"; tail -5 "$RUN/bob.out"
fi

mkdir -p "$RUN/dl_carol2"
send carol "download_file g1 orig2.bin $RUN/dl_carol2/orig2.bin rarest"
if wait_for_line "$RUN/carol.out" "Download of 'orig2.bin' complete" 150; then
  if cmp -s "$RUN/orig2.bin" "$RUN/dl_carol2/orig2.bin"; then
    pass "S2: carol downloaded from 2 seeders, byte-identical"
  else
    fail "S2: carol's copy differs from the original"
  fi
else
  fail "S2: carol multi-peer download did not complete"; tail -5 "$RUN/carol.out"
fi

# ------------------------------------------------------------------ S3 ----
echo ""
echo "==> S3: tracker failover"

# Kill the primary (tracker 1, port 5101). The backup must promote and
# accept new clients.
kill "${ALL_PIDS[0]}" 2>/dev/null
sleep 3

start_client dave 7104
sleep 1
send dave "create_user dave pw_dave"
sleep 0.5
send dave "login dave pw_dave"
sleep 1

if grep -q "Successfully connected to PRIMARY tracker at 127.0.0.1:5102" "$RUN/dave.out"; then
  pass "S3: new client reached the promoted backup on port 5102"
else
  fail "S3: new client did not land on the promoted backup"; tail -5 "$RUN/dave.out"
fi

if grep -q "Login successful" "$RUN/dave.out" 2>/dev/null; then
  pass "S3: login works against the promoted tracker"
else
  fail "S3: login against promoted tracker failed"; tail -5 "$RUN/dave.out"
fi

# ------------------------------------------------------------------ S4 ----
echo ""
echo "==> S4: corrupt-source resilience"

# Fresh topology (S3 killed the primary).
kill_all
sleep 1
start_trackers

start_client eve   7105
start_client frank 7106
register_users eve frank || true
check_login eve; check_login frank

send eve "create_group g2"
sleep 0.5
send frank "join_group g2"
sleep 0.5
send eve "accept_request g2 frank"
sleep 0.5

# 4 MB file -> 8 pieces; enough that frank is still downloading when the
# corruption lands.
dd if=/dev/urandom of="$RUN/orig4.bin" bs=1M count=4 2>/dev/null
cp "$RUN/orig4.bin" "$RUN/orig4.pristine"

send eve "upload_file g2 $RUN/orig4.bin"
wait_for_line "$RUN/eve.out" "File shared successfully" 50 >/dev/null

mkdir -p "$RUN/dl_frank"
send frank "download_file g2 orig4.bin $RUN/dl_frank/orig4.bin rarest"
sleep 0.8   # let the first pieces arrive
# Corrupt the seeder's on-disk copy mid-download: eve's seeder re-reads from
# disk per request, so later pieces are served with wrong bytes.
dd if=/dev/urandom of="$RUN/orig4.bin" bs=1 count=64 seek=$((3*512*1024)) conv=notrunc 2>/dev/null

if wait_for_line "$RUN/frank.out" "Download of 'orig4.bin' complete" 200; then
  # Completed: final verification ran against pristine hashes, so success
  # means corrupted pieces were rejected and retried until correct.
  if cmp -s "$RUN/orig4.pristine" "$RUN/dl_frank/orig4.bin"; then
    pass "S4: completed byte-identical despite mid-download corruption"
  else
    fail "S4: declared complete but bytes differ from pristine source"
  fi
elif wait_for_line "$RUN/frank.out" "Hash mismatch for piece" 30; then
  pass "S4: corrupted piece detected and rejected by hash verification"
else
  fail "S4: no completion and no hash-mismatch event observed"; tail -8 "$RUN/frank.out"
fi

# ============================================================== summary ==

echo ""
echo "==> Integration summary: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
  echo "    Failed: ${FAILED_SCENARIOS[*]}"
  echo "    Logs:   $LOGS"
  exit 1
fi
echo "    All scenarios passed."
exit 0
