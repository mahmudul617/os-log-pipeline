#!/usr/bin/env python3
"""
gen_logs.py - Generate a synthetic application log for testing/benchmarking.

Usage:
    python3 scripts/gen_logs.py LINES OUTFILE [--seed N] [--malformed RATE]

Each valid line looks like:
    2026-11-15 10:23:45 ERROR Database connection failed (db-2)

With --malformed RATE (e.g. 0.01 = 1%), some lines are deliberately broken
(blank, bad timestamp, unknown level, missing message, too long) so the
error handling can be tested. The same seed always gives the same file,
so experiments are repeatable.
"""
import argparse
import datetime
import random

LEVELS = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]
WEIGHTS = [20, 50, 15, 12, 3]  # percent

MESSAGES = {
    "DEBUG": ["Cache lookup key=user:{n}", "Entering handler /api/v1/items/{n}",
              "SQL query took {n} us"],
    "INFO": ["User {n} logged in", "Request GET /home served in {n} ms",
             "Scheduled job #{n} completed", "Payment {n} accepted"],
    "WARNING": ["Disk usage at {p}%", "Slow response from auth service ({n} ms)",
                "Retrying connection to cache (attempt {a})"],
    "ERROR": ["Database connection failed (db-{a})", "Timeout calling payment API",
              "Failed to write file /var/data/{n}.tmp", "Null user id in session {n}"],
    "CRITICAL": ["Out of memory in worker {a}", "Primary database unreachable",
                 "Service crashed with signal 11"],
}


def valid_line(rng, ts):
    level = rng.choices(LEVELS, WEIGHTS)[0]
    msg = rng.choice(MESSAGES[level]).format(
        n=rng.randint(1, 99999), p=rng.randint(70, 99), a=rng.randint(1, 5))
    return f"{ts:%Y-%m-%d %H:%M:%S} {level} {msg}"


def malformed_line(rng, ts):
    kind = rng.randrange(6)
    if kind == 0:
        return ""                                            # empty line
    if kind == 1:
        return f"{ts:%Y/%m/%d %H:%M:%S} ERROR wrong date format"  # bad timestamp
    if kind == 2:
        return f"{ts:%Y-%m-%d %H:%M:%S} FATALITY unknown level"  # bad level
    if kind == 3:
        return f"{ts:%Y-%m-%d %H:%M:%S} ERROR"                # no message
    if kind == 4:
        return f"{ts:%Y-%m-%d %H:%M:%S} INFO " + "x" * 1500   # too long
    return "garbage line without any structure"               # bad timestamp


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lines", type=int)
    ap.add_argument("outfile")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--malformed", type=float, default=0.0,
                    help="fraction of malformed lines, e.g. 0.01")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    ts = datetime.datetime(2026, 11, 1, 0, 0, 0)
    with open(args.outfile, "w") as f:
        for _ in range(args.lines):
            ts += datetime.timedelta(seconds=rng.randint(0, 3))
            if rng.random() < args.malformed:
                f.write(malformed_line(rng, ts) + "\n")
            else:
                f.write(valid_line(rng, ts) + "\n")
    print(f"wrote {args.lines} lines to {args.outfile}")


if __name__ == "__main__":
    main()
