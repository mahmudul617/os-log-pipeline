#!/usr/bin/env bash
# observe.sh - Run the pipeline slowly and show the process hierarchy with
# ps and pstree (for screenshots / the experimental investigation).
#
# Usage: bash scripts/observe.sh
set -u
cd "$(dirname "$0")/.."
make -s all
[ -f data/medium.log ] || python3 scripts/gen_logs.py 10000 data/medium.log --seed 2 > /dev/null

echo "Starting pipeline in the background (filter slowed down to 1 ms/record)..."
./bin/log_pipeline -d 1000 -o results/observe_report.txt data/medium.log 2> results/observe_lifecycle.log &
SUP=$!
sleep 1

echo
echo "===== This shell (PID $$) and the supervisor (PID $SUP) ====="
echo
echo "\$ ps -o pid,ppid,stat,etime,comm --ppid $SUP -p $SUP"
ps -o pid,ppid,stat,etime,comm --ppid "$SUP" -p "$SUP"
echo
echo "\$ pstree -p $SUP"
pstree -p "$SUP"
echo
echo "\$ ls -l /proc/<pid>/fd   (which pipe each stage holds)"
for pid in $(pgrep -P "$SUP"); do
    echo "--- $(cat /proc/$pid/comm) (pid $pid)"
    ls -l /proc/$pid/fd 2>/dev/null | awk 'NR>1 {print "   fd " $9 " -> " $11}'
done

echo
echo "Waiting for the pipeline to finish..."
wait "$SUP"
echo "Exit status: $?"
echo
echo "Life-cycle log (results/observe_lifecycle.log):"
cat results/observe_lifecycle.log
