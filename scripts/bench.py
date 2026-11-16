#!/usr/bin/env python3
"""
bench.py - Compare execution time of the multi-process pipeline and the
single-process version for different input sizes.

Usage (from the project root, after `make`):
    python3 scripts/bench.py                 # default sizes, 5 runs each
    python3 scripts/bench.py --sizes 1000 10000 100000 --runs 3

Produces:
    results/environment.txt   hardware / software used (put this in the report)
    results/timing_*.csv      raw numbers for the three experiments
    results/timing.md         tables ready to paste into the report
    results/timing.png        graphs
Experiments:
    1. time vs input size (1K .. 2M lines)
    2. speedup vs simulated CPU work per record (-w), shows when the
       pipeline starts to beat the single process
    3. effect of the kernel pipe buffer size (-b)
Each run's report is compared between both versions, so a speed result is
only recorded if the outputs are identical.
"""
import argparse
import os
import platform
import resource
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPE = os.path.join(ROOT, "bin", "log_pipeline")
SINGLE = os.path.join(ROOT, "bin", "log_single")
DATA = os.path.join(ROOT, "data", "bench")
RES = os.path.join(ROOT, "results")


def run(cmd):
    """Run cmd, return (wall seconds, cpu seconds of all child processes, stdout)."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    wall = time.perf_counter() - t0
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if p.returncode != 0:
        sys.exit(f"command failed: {' '.join(cmd)}\n{p.stderr.decode()}")
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    return wall, cpu, p.stdout


def environment():
    def sh(cmd):
        try:
            return subprocess.run(cmd, shell=True, stdout=subprocess.PIPE,
                                  text=True).stdout.strip()
        except Exception:
            return "?"
    cpu = sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip()
    lines = [
        f"Date            : {time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"CPU model       : {cpu}",
        f"CPU cores (nproc): {os.cpu_count()}",
        f"Memory          : {sh('free -h | awk \"/Mem:/ {print $2}\"')}",
        f"OS              : {sh('. /etc/os-release && echo $PRETTY_NAME')}",
        f"Kernel          : {platform.release()}",
        f"Compiler        : {sh('gcc --version | head -1')}",
        f"Python          : {platform.python_version()}",
        f"Virtualized     : {sh('systemd-detect-virt 2>/dev/null || echo unknown')}",
    ]
    return "\n".join(lines)


def measure(path, runs, single_args=(), pipe_args=()):
    """Time both versions on one file. Returns a dict of medians."""
    single = [SINGLE, *single_args, path]
    pipe = [PIPE, "-q", *pipe_args, path]
    _, _, out_s = run(single)          # warm-up (loads file into page cache)
    _, _, out_p = run(pipe)
    if out_s != out_p:
        sys.exit(f"reports differ for {path} {pipe_args}! not recording timings")
    s_wall, s_cpu, p_wall, p_cpu = [], [], [], []
    for _ in range(runs):
        w, c, _ = run(single); s_wall.append(w); s_cpu.append(c)
        w, c, _ = run(pipe); p_wall.append(w); p_cpu.append(c)
    sd = (lambda v: statistics.stdev(v) * 1000) if runs > 1 else (lambda v: 0.0)
    r = {
        "single_ms": statistics.median(s_wall) * 1000, "single_sd": sd(s_wall),
        "pipe_ms": statistics.median(p_wall) * 1000, "pipe_sd": sd(p_wall),
        "single_cpu_ms": statistics.median(s_cpu) * 1000,
        "pipe_cpu_ms": statistics.median(p_cpu) * 1000,
    }
    r["speedup"] = r["single_ms"] / r["pipe_ms"]
    return r


def dataset(n):
    path = os.path.join(DATA, f"log_{n}.log")
    if not os.path.exists(path):
        subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "gen_logs.py"),
                        str(n), path, "--seed", "2026", "--malformed", "0.01"],
                       check=True, stdout=subprocess.DEVNULL)
    return path


def write_csv(name, rows):
    cols = list(rows[0].keys())
    with open(os.path.join(RES, name), "w") as f:
        f.write(",".join(cols) + "\n")
        for r in rows:
            f.write(",".join(f"{r[c]:.3f}" if isinstance(r[c], float) else str(r[c])
                             for c in cols) + "\n")


def md_row(first, r):
    return (f"| {first} | {r['single_ms']:.1f} ± {r['single_sd']:.1f} | "
            f"{r['pipe_ms']:.1f} ± {r['pipe_sd']:.1f} | {r['speedup']:.2f}x | "
            f"{r['single_cpu_ms']:.0f} | {r['pipe_cpu_ms']:.0f} |\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", type=int, nargs="+",
                    default=[1_000, 10_000, 100_000, 1_000_000, 2_000_000])
    ap.add_argument("--work", type=int, nargs="+", default=[0, 2, 5, 10, 20, 50, 100],
                    help="values of -w for experiment 2")
    ap.add_argument("--work-lines", type=int, default=100_000)
    ap.add_argument("--runs", type=int, default=5)
    args = ap.parse_args()

    for b in (PIPE, SINGLE):
        if not os.path.exists(b):
            sys.exit("binaries not found - run `make` first")
    os.makedirs(DATA, exist_ok=True)
    os.makedirs(RES, exist_ok=True)

    env = environment()
    print(env, "\n")
    with open(os.path.join(RES, "environment.txt"), "w") as f:
        f.write(env + "\n")

    head = ("| {} | Single (ms) | Pipeline (ms) | Speedup | CPU single (ms) | CPU pipeline (ms) |\n"
            "|---:|---:|---:|---:|---:|---:|\n")
    md = open(os.path.join(RES, "timing.md"), "w")
    md.write(f"Environment: {os.cpu_count()} CPU cores. Median of {args.runs} runs "
             f"(± = standard deviation). CPU = user+sys time of all processes.\n\n")

    # ---------- Experiment 1: input size ----------
    print("Experiment 1: input size (no extra work)")
    rows = []
    for n in args.sizes:
        path = dataset(n)
        r = {"lines": n, "size_mb": os.path.getsize(path) / 1e6, **measure(path, args.runs)}
        rows.append(r)
        print(f"  {n:>9} lines ({r['size_mb']:6.1f} MB): single {r['single_ms']:8.1f} ms | "
              f"pipeline {r['pipe_ms']:8.1f} ms | speedup {r['speedup']:.2f}x")
    write_csv("timing_size.csv", rows)
    md.write("### Experiment 1: execution time vs input size\n\n" + head.format("Lines"))
    for r in rows:
        md.write(md_row(f"{r['lines']:,} ({r['size_mb']:.1f} MB)", r))

    # ---------- Experiment 2: per-record work ----------
    print(f"\nExperiment 2: simulated work per record per stage ({args.work_lines:,} lines)")
    path = dataset(args.work_lines)
    wrows = []
    for w in args.work:
        a = ("-w", str(w))
        r = {"work": w, **measure(path, args.runs, a, a)}
        wrows.append(r)
        print(f"  -w {w:<4}: single {r['single_ms']:8.1f} ms | pipeline {r['pipe_ms']:8.1f} ms "
              f"| speedup {r['speedup']:.2f}x")
    write_csv("timing_work.csv", wrows)
    md.write(f"\n### Experiment 2: speedup vs work per record ({args.work_lines:,} lines)\n\n"
             + head.format("Work (-w)"))
    for r in wrows:
        md.write(md_row(str(r["work"]), r))

    # ---------- Experiment 3: pipe buffer size ----------
    big = dataset(max(args.sizes))
    print(f"\nExperiment 3: kernel pipe buffer size ({max(args.sizes):,} lines)")
    brows = []
    for b, label in ((0, "64 KB (default)"), (262144, "256 KB"), (1048576, "1 MB")):
        r = {"pipe_buffer": label, **measure(big, args.runs, (), ("-b", str(b)))}
        brows.append(r)
        print(f"  {label:16}: pipeline {r['pipe_ms']:8.1f} ms")
    write_csv("timing_pipebuf.csv", brows)
    md.write(f"\n### Experiment 3: pipe buffer size ({max(args.sizes):,} lines)\n\n"
             + head.format("Pipe buffer"))
    for r in brows:
        md.write(md_row(r["pipe_buffer"], r))
    md.close()

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed - skipping graph (sudo apt install python3-matplotlib)")
        return

    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11.5, 4.3))
    x = [r["lines"] for r in rows]
    a1.plot(x, [r["single_ms"] for r in rows], "o-", color="#2a6fdb", label="Single process")
    a1.plot(x, [r["pipe_ms"] for r in rows], "s-", color="#e0712b", label="3-process pipeline")
    a1.set_xscale("log"); a1.set_yscale("log")
    a1.set_xlabel("Input size (lines)"); a1.set_ylabel("Wall-clock time (ms)")
    a1.set_title("1) Time vs input size (no extra work)")
    a1.grid(True, which="both", alpha=0.3); a1.legend()

    xw = [r["work"] for r in wrows]
    a2.plot(xw, [r["speedup"] for r in wrows], "o-", color="#2a9d5c")
    a2.axhline(1.0, color="gray", linestyle="--", linewidth=1)
    a2.axhline(min(3, os.cpu_count()), color="#999", linestyle=":", linewidth=1,
               label=f"ideal = min(3 stages, {os.cpu_count()} cores)")
    a2.set_xlabel("Simulated work per record per stage (-w)")
    a2.set_ylabel("Speedup (single / pipeline)")
    a2.set_title(f"2) Speedup vs work per record ({args.work_lines:,} lines)")
    a2.grid(True, alpha=0.3); a2.legend(loc="lower right")

    fig.suptitle(f"Pipeline vs single process ({os.cpu_count()} CPU cores)")
    fig.tight_layout()
    fig.savefig(os.path.join(RES, "timing.png"), dpi=150)
    print("\nSaved results/environment.txt, results/timing.md, results/timing_*.csv, results/timing.png")


if __name__ == "__main__":
    main()
