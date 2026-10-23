#!/usr/bin/env python3
"""
Plot SafeServe benchmark results.

Reads the CSV written by tools/bench.sh (default results/bench.csv) and
draws, per workload mix, goodput and p99 latency against concurrency for each
strategy, plus the share of requests that failed (503, errors, timeouts).
Repetitions of the same cell are summarised by their median, with min-max
error bars.

Nothing here is hard-coded: every number comes from the CSV.

    python3 tools/plot.py [results/bench.csv] [results/bench.png]
    python3 tools/plot.py --markdown [results/bench.csv]   # summary table only
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

plt = None


def need_matplotlib():
    global plt
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as pyplot
    except ImportError:
        sys.exit("matplotlib is required: python3 -m pip install matplotlib")
    plt = pyplot

ORDER = ["naive", "lockorder", "detect", "banker", "banker-atomic"]
COLORS = {"naive": "#c0392b", "lockorder": "#27ae60", "detect": "#e67e22",
          "banker": "#2e86c1", "banker-atomic": "#8e44ad"}
MIX_ORDER = ["static", "balanced", "write"]


def load(path):
    cells = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            key = (row["label"], row["mix"], int(row["clients"]))
            cells[key].append(row)
    return cells


def summarize(rows, field, transform=float):
    vals = [transform(r[field]) for r in rows if r[field] not in ("", "nan")]
    if not vals:
        return float("nan"), 0.0, 0.0
    med = statistics.median(vals)
    return med, med - min(vals), max(vals) - med


def failure_share(r):
    total = int(r["requests"])
    bad = int(r["rejected_503"]) + int(r["other_errors"]) + int(r["timeouts"]) + int(r["conn_errors"])
    return 100.0 * bad / total if total else 100.0


def markdown_table(cells):
    """Median over repetitions of each (strategy, mix, clients) cell."""
    labels = [l for l in ORDER if any(k[0] == l for k in cells)]
    mixes = [m for m in MIX_ORDER if any(k[1] == m for k in cells)]
    print("| Mix | Clients | Strategy | Goodput (2xx/s) | p50 ms | p99 ms | Failed % | Timeouts |")
    print("|---|---:|---|---:|---:|---:|---:|---:|")
    for mix in mixes:
        for clients in sorted({k[2] for k in cells if k[1] == mix}):
            for label in labels:
                rows = cells.get((label, mix, clients))
                if not rows:
                    continue
                med = lambda f: summarize(rows, f)[0]
                fail = statistics.median([failure_share(r) for r in rows])
                tos = statistics.median([int(r["timeouts"]) for r in rows])
                fmt = lambda v: "-" if v != v else f"{v:.0f}"
                print(f"| {mix} | {clients} | {label} | {med('goodput_rps'):.1f} | {fmt(med('p50_ms'))} "
                      f"| {fmt(med('p99_ms'))} | {fail:.0f} | {tos:.0f} |")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--markdown":
        src = sys.argv[2] if len(sys.argv) > 2 else "results/bench.csv"
        markdown_table(load(src))
        return
    src = sys.argv[1] if len(sys.argv) > 1 else "results/bench.csv"
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + ".png"
    if not os.path.exists(src):
        sys.exit(f"{src} not found - run `make bench` first")

    need_matplotlib()
    cells = load(src)
    labels = [l for l in ORDER if any(k[0] == l for k in cells)]
    labels += sorted({k[0] for k in cells} - set(labels))
    mixes = [m for m in MIX_ORDER if any(k[1] == m for k in cells)]

    metrics = [
        ("goodput_rps", "Goodput (2xx / s)", float, False),
        ("p99_ms", "p99 latency (ms, log)", float, True),
        (None, "Failed requests (%)", failure_share, False),
    ]
    fig, axes = plt.subplots(len(metrics), len(mixes), figsize=(5 * len(mixes), 3.6 * len(metrics)),
                             squeeze=False)

    for col, mix in enumerate(mixes):
        for row, (field, title, fn, logy) in enumerate(metrics):
            ax = axes[row][col]
            for label in labels:
                xs = sorted({k[2] for k in cells if k[0] == label and k[1] == mix})
                if not xs:
                    continue
                ys, lo, hi = [], [], []
                for x in xs:
                    rows = cells[(label, mix, x)]
                    if field is None:
                        vals = [fn(r) for r in rows]
                        med = statistics.median(vals)
                        ys.append(med); lo.append(med - min(vals)); hi.append(max(vals) - med)
                    else:
                        m, l, h = summarize(rows, field, fn)
                        ys.append(m); lo.append(l); hi.append(h)
                ax.errorbar(xs, ys, yerr=[lo, hi], marker="o", capsize=3, label=label,
                            color=COLORS.get(label))
            ax.set_xscale("log")
            if logy:
                ax.set_yscale("log")
            ax.set_xlabel("concurrent clients")
            ax.set_title(f"{title} - {mix} mix", fontsize=10)
            ax.grid(True, which="both", linestyle=":", alpha=0.6)
    axes[0][0].legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
