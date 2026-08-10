#!/usr/bin/env python3
"""Turn the phase3 sweep CSV into the README figure and tables.

Bus bandwidth rather than time, because busbw = (bytes/time) * 2(N-1)/N is a
transform of the same measurement and only one of the two has any shape to it:
plotted against size the times are three near-straight lines on log-log, while
the bandwidth peaks at 4 MB and falls away. The times are tabulated below
instead. busbw is also what NCCL's own benchmarks report, so it is the number
that can be held against something outside this repo.

Usage: python3 scripts/plot_phase3.py [results/phase3_sweep.csv] [outdir]
"""
import csv
import statistics as st
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

CSV = sys.argv[1] if len(sys.argv) > 1 else "results/phase3_sweep.csv"
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else "results"

# Same palette as the phase 1 and 2 figures; colour carries the backend.
BLUE, ORANGE = "#2a78d6", "#eb6834"
INK, INK_2, GRID, SURFACE = "#0b0b0b", "#52514e", "#dedcd6", "#fcfcfb"

plt.rcParams.update({
    "font.size": 9,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK,
    "axes.titlecolor": INK,
    "text.color": INK,
    "xtick.color": INK_2,
    "ytick.color": INK_2,
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
})

# Sizes that carry an axis label; the rest are ticks only.
LABELLED = {1 << 16, 1 << 18, 1 << 20, 1 << 22, 1 << 23, 1 << 24}

ARMS = (
    ("rdma", BLUE, "-", "RDMA backend"),
    ("tcp", ORANGE, "-", "TCP backend, latency-tuned"),
    ("tcp_untuned", ORANGE, "--", "TCP backend, default settings"),
)


def load():
    """(backend, bytes) -> (list of us, list of busbw Gbps)."""
    us, bw = defaultdict(list), defaultdict(list)
    for r in csv.DictReader(open(CSV)):
        k = (r["conn"], int(r["bytes"]))
        us[k].append(float(r["median_us"]))
        bw[k].append(float(r["busbw_gbps"]))
    return us, bw


def med(d, *k):
    return st.median(d[k]) if d.get(k) else None


def fmt_bytes(v, _pos=None):
    for unit, div in (("M", 1 << 20), ("K", 1 << 10)):
        if v >= div:
            n = v / div
            return f"{n:.0f}{unit}" if n == int(n) else f"{n:.1f}{unit}"
    return f"{int(v)}"


def figure(us, bw):
    sizes = sorted({k[1] for k in us})
    fig, ax = plt.subplots(figsize=(7.6, 4.4))

    for be, color, style, label in ARMS:
        ax.plot(sizes, [med(bw, be, s) for s in sizes], color=color,
                linestyle=style, linewidth=2, marker="o", markersize=5,
                label=label, zorder=3)

    ax.set_xscale("log", base=2)
    # Every size gets a tick, but only some get a label -- the points past
    # 4 MB are deliberately close together and the labels collide otherwise.
    ax.set_xticks(sizes)
    ax.set_xticklabels([fmt_bytes(s) if s in LABELLED else "" for s in sizes])
    ax.set_xlabel("buffer size (bytes)")
    ax.set_ylabel("bus bandwidth (Gbps)")
    ax.grid(True, color=GRID, linewidth=0.6, zorder=0)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)

    ax.set_ylim(0, 108)
    ax.axhline(100, color=INK_2, linewidth=0.8, linestyle=":", zorder=1)
    ax.annotate("100 GbE link", xy=(sizes[0], 100), xytext=(0, -12),
                textcoords="offset points", fontsize=8, color=INK_2)

    # The gap in time at each end, against whichever TCP configuration
    # measured better there. Bandwidth is the reciprocal, so the same ratio.
    for s in (sizes[0], sizes[-1]):
        r, t = med(bw, "rdma", s), max(med(bw, "tcp", s), med(bw, "tcp_untuned", s))
        ax.annotate(f"{r/t:.1f}×", xy=(s, (r + t) / 2), fontsize=9,
                    color=INK_2, ha="center", va="center")

    # Upper right is the only empty quadrant: the link marker sits top
    # left and both backends run along the bottom half.
    ax.legend(loc="upper right", frameon=False, fontsize=8.5)
    ax.set_title("Ring all-reduce, two ranks, one interface over two backends",
                 fontsize=10.5, loc="left")
    fig.tight_layout()
    out = f"{OUTDIR}/phase3-allreduce.png"
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def tables(us, bw):
    sizes = sorted({k[1] for k in us})
    print("\nall-reduce time (us). gap is against the better TCP configuration.")
    print(f"{'buffer':>9} {'rdma':>9} {'tcp lat':>9} {'tcp dflt':>9} {'gap':>6}")
    for s in sizes:
        r, t, u = (med(us, a, s) for a in ("rdma", "tcp", "tcp_untuned"))
        print(f"{fmt_bytes(s):>9} {r:>9.1f} {t:>9.1f} {u:>9.1f} {min(t, u)/r:>5.1f}x")

    print("\nbus bandwidth (Gbps) against a 100 GbE link")
    print(f"{'buffer':>9} {'rdma':>9} {'tcp lat':>9} {'tcp dflt':>9}")
    for s in sizes:
        r, t, u = (med(bw, a, s) for a in ("rdma", "tcp", "tcp_untuned"))
        print(f"{fmt_bytes(s):>9} {r:>9.1f} {t:>9.1f} {u:>9.1f}")

    print("\nrun-to-run spread of the median, IQR as % of median")
    for be, _, _, label in ARMS:
        iqr = []
        for s in sizes:
            v = us[(be, s)]
            q = st.quantiles(v, n=4)
            iqr.append((q[2] - q[0]) / st.median(v) * 100)
        print(f"  {be:12s} median {st.median(iqr):5.1f}%   worst {max(iqr):5.1f}%")


if __name__ == "__main__":
    u, b = load()
    figure(u, b)
    tables(u, b)
