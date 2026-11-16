#!/usr/bin/env bash
# run_tests.sh - automated tests for the log-processing pipeline.
#
# Covers: normal cases, boundary cases (empty file, CRLF, no final newline,
# very long lines), malformed input, failure scenarios (a stage crashing,
# a stage killed with kill -9 from outside, repeated failure, missing file,
# bad option, SIGTERM to the supervisor) and pipeline == single-process.
#
# Usage:  bash tests/run_tests.sh        (run from the project root)

set -u
cd "$(dirname "$0")/.."

PIPE=./bin/log_pipeline
SINGLE=./bin/log_single
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

# value of a report field, e.g. field report.txt "ERROR"
field() { grep -E "^ *$2 *:" "$1" | head -1 | awk -F': ' '{print $2}'; }

echo "== Building =="
make -s all || { echo "build failed"; exit 1; }

# ---------------------------------------------------------------------
echo "== 1. Normal case: hand-checked sample file (data/sample.log) =="
$PIPE -q data/sample.log > "$TMP/p.txt"; rc=$?
check "pipeline exits with status 0"            "[ $rc -eq 0 ]"
check "20 input lines"                          "[ \"$(field $TMP/p.txt 'Input lines')\" = 20 ]"
check "4 ERROR records"                         "[ \"$(field $TMP/p.txt ERROR)\" = 4 ]"
check "2 CRITICAL records"                      "[ \"$(field $TMP/p.txt CRITICAL)\" = 2 ]"
check "6 INFO records"                          "[ \"$(field $TMP/p.txt INFO)\" = 6 ]"
check "4 malformed lines (one of each kind)"    "[ \"$(field $TMP/p.txt 'Malformed records')\" = 4 ]"
check "6 selected events (ERROR+CRITICAL)"      "[ \"$(field $TMP/p.txt 'Selected events')\" = 6 ]"
check "message containing '|' is handled"       "[ \"$(field $TMP/p.txt 'Consistency check')\" = 'OK (every input line accounted for)' ]"
$SINGLE data/sample.log > "$TMP/s.txt"
check "pipeline report == single-process report" "cmp -s $TMP/p.txt $TMP/s.txt"

# ---------------------------------------------------------------------
echo "== 2. Boundary cases =="
: > "$TMP/empty.log"
$PIPE -q "$TMP/empty.log" > "$TMP/e.txt"; rc=$?
check "empty file: exit 0"                      "[ $rc -eq 0 ]"
check "empty file: 0 input lines"               "[ \"$(field $TMP/e.txt 'Input lines')\" = 0 ]"

printf '2026-11-15 10:00:00 ERROR one\r\n2026-11-15 10:00:01 INFO two\r\n2026-11-15 10:00:02 ERROR no newline at end' > "$TMP/crlf.log"
$PIPE -q "$TMP/crlf.log" > "$TMP/c.txt"
check "CRLF line endings + missing final newline: 3 valid" "[ \"$(field $TMP/c.txt 'Valid records')\" = 3 ]"

{ printf '2026-11-15 10:00:00 INFO '; head -c 5000 /dev/zero | tr '\0' 'x'; echo; } > "$TMP/long.log"
$PIPE -q "$TMP/long.log" > "$TMP/l.txt"
check "5000-char line is reported as too_long"  "[ \"$(field $TMP/l.txt too_long)\" = 1 ]"

printf '2026-11-15 10:00:00 ERROR only line\n' > "$TMP/one.log"
$PIPE -q "$TMP/one.log" > "$TMP/o.txt"
check "single-line file"                        "[ \"$(field $TMP/o.txt 'Selected events')\" = 1 ]"

# ---------------------------------------------------------------------
echo "== 3. Malformed log entries =="
cat > "$TMP/bad.log" <<'EOF'

2026/11/15 10:00:00 ERROR slash date
2026-13-40 10:00:00 ERROR impossible date
2026-11-15 25:61:00 ERROR impossible time
not a log line at all
2026-11-15 10:00:00 FATAL unknown level
2026-11-15 10:00:00error no space
2026-11-15 10:00:00
2026-11-15 10:00:00 ERROR
2026-11-15 10:00:00 ERROR
2026-11-15 10:00:00 ERROR this one is fine
EOF
$PIPE -q -l ALL "$TMP/bad.log" > "$TMP/b.txt"; rc=$?
check "malformed input does not crash (exit 0)" "[ $rc -eq 0 ]"
check "1 empty_line"                            "[ \"$(field $TMP/b.txt empty_line)\" = 1 ]"
check "4 bad_timestamp"                         "[ \"$(field $TMP/b.txt bad_timestamp)\" = 4 ]"
check "3 bad_level"                             "[ \"$(field $TMP/b.txt bad_level)\" = 3 ]"
check "2 missing_message"                       "[ \"$(field $TMP/b.txt missing_message)\" = 2 ]"
check "exactly 1 valid record"                  "[ \"$(field $TMP/b.txt 'Valid records')\" = 1 ]"
$SINGLE -l ALL "$TMP/bad.log" > "$TMP/bs.txt"
check "pipeline == single on malformed file"    "cmp -s $TMP/b.txt $TMP/bs.txt"

# ---------------------------------------------------------------------
echo "== 4. Larger generated file (100,000 lines, 2% malformed) =="
python3 scripts/gen_logs.py 100000 "$TMP/gen.log" --malformed 0.02 --seed 99 > /dev/null
for opts in "" "-l ALL" "-l WARNING" "-l ERROR,CRITICAL -k database"; do
    $PIPE -q $opts "$TMP/gen.log" > "$TMP/gp.txt"
    $SINGLE $opts "$TMP/gen.log" > "$TMP/gs.txt"
    check "pipeline == single (options: ${opts:-default})" "cmp -s $TMP/gp.txt $TMP/gs.txt"
done
# Independent check with grep (does not use our parser at all)
expected=$(grep -cE '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} ERROR +[^ ]' "$TMP/gen.log")
$PIPE -q -l ALL "$TMP/gen.log" > "$TMP/gp.txt"
check "ERROR count matches grep ($expected)"    "[ \"$(field $TMP/gp.txt ERROR)\" = \"$expected\" ]"

# ---------------------------------------------------------------------
echo "== 5. Failure scenarios: stage crash + restart (no loss, no duplicates) =="
$SINGLE -l ALL "$TMP/gen.log" > "$TMP/ref.txt"
for st in reader filter summarizer; do
    timeout 30 $PIPE -l ALL -x "$st:54321" "$TMP/gen.log" > "$TMP/r.txt" 2> "$TMP/r.err"; rc=$?
    check "$st crash: recovered, exit 0"                  "[ $rc -eq 0 ]"
    check "$st crash: was really killed by SIGKILL"      "grep -q 'TERMINATED BY SIGNAL 9' $TMP/r.err"
    check "$st crash: pipeline restarted once"           "grep -q '(1 restart)' $TMP/r.err"
    check "$st crash: report identical to single-process" "cmp -s $TMP/r.txt $TMP/ref.txt"
    check "$st crash: treated as crash, not as bad data"  "! grep -q 'protocol error' $TMP/r.err"
done

echo "== 5b. Crash in the middle of a large stream (partial record left in the pipe) =="
python3 scripts/gen_logs.py 300000 "$TMP/big.log" --malformed 0.02 --seed 5 > /dev/null
$SINGLE -l ALL "$TMP/big.log" > "$TMP/bigref.txt"
for st in reader filter summarizer; do
    timeout 60 $PIPE -l ALL -x "$st:150000" "$TMP/big.log" > "$TMP/r.txt" 2> "$TMP/r.err"; rc=$?
    check "$st crash at 150000: recovered, identical report, no protocol error" \
          "[ $rc -eq 0 ] && cmp -s $TMP/r.txt $TMP/bigref.txt && ! grep -q 'protocol error' $TMP/r.err"
done

echo "== 6. Failure scenario: filter killed from OUTSIDE with kill -9 =="
$PIPE -l ALL -d 50 "$TMP/gen.log" > "$TMP/k.txt" 2> "$TMP/k.err" &
sup=$!
sleep 1.5
fpid=$(pgrep -P "$sup" -x lp-filter)
if [ -n "$fpid" ]; then
    kill -9 "$fpid"
    wait "$sup"; rc=$?
    check "killed filter (pid $fpid): recovered, exit 0" "[ $rc -eq 0 ]"
    check "report identical to single-process"           "cmp -s $TMP/k.txt $TMP/ref.txt"
else
    wait "$sup"
    fail "could not find lp-filter child process"
fi

echo "== 7. Failure scenario: stage keeps failing -> gives up, never hangs =="
timeout 20 $PIPE -q -r 2 -x filter:10:always "$TMP/gen.log" > /dev/null 2> "$TMP/g.err"; rc=$?
check "gives up with non-zero status (not 124 = timeout/hang)" "[ $rc -ne 0 ] && [ $rc -ne 124 ]"
check "log says 'giving up after 2 restarts'"   "grep -q 'giving up after 2 restarts' $TMP/g.err"

echo "== 8. Failure scenario: SIGTERM to supervisor stops all children =="
$PIPE -q -d 200 "$TMP/gen.log" > /dev/null 2>&1 &
sup=$!
sleep 1
kids_before=$(pgrep -P "$sup" | wc -l)
kill -TERM "$sup"
wait "$sup"; rc=$?
sleep 0.2
check "3 children were running"                  "[ $kids_before -eq 3 ]"
check "supervisor exited with 128+15"            "[ $rc -eq 143 ]"
check "no lp-* processes left behind"            "! pgrep -x lp-filter >/dev/null && ! pgrep -x lp-reader >/dev/null"

echo "== 9. Usage errors =="
$PIPE -q /no/such/file.log > /dev/null 2>&1; rc=$?
check "missing input file -> exit 1"             "[ $rc -eq 1 ]"
$PIPE -l NOTALEVEL data/sample.log > /dev/null 2>&1; rc=$?
check "invalid level list -> exit 1"             "[ $rc -eq 1 ]"
$PIPE -q -o "$TMP/out.txt" data/sample.log; rc=$?
check "-o writes report to a file"               "[ $rc -eq 0 ] && cmp -s $TMP/out.txt $TMP/s.txt"

echo "== 10. No checkpoint files left in /tmp =="
check "no stale lp-*.ckpt files"                 "[ -z \"\$(ls ${TMPDIR:-/tmp}/lp-*.ckpt 2>/dev/null)\" ]"

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
