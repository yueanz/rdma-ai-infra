#!/usr/bin/env python3
"""Turn the phase4 sweep CSV into the README figure and tables.

Two stacked panels. Time first, at one operation outstanding, which is what a
decode step waits for and what reads the same way as the phase 1 and 2 figures.
Rate second, at one and at sixteen, because depth is where the two directions
part company: a lone write already fills the link, a lone read does not, and
concurrency recovers only the small reads.

Usage: python3 scripts/plot_phase4.py [results/phase4_sweep.csv] [outdir]
"""
import csv
import statistics as st
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

CSV = sys.argv[1] if len(sys.argv) > 1 else "results/phase4_sweep.csv"
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else "results"

# Same palette as the earlier figures. Here colour carries the direction
# rather than the backend — there is only one backend in this phase.
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

OPS = (("write", BLUE), ("read", ORANGE))
SHOWN_DEPTHS = (1, 16)


def load():
    """(op, size, depth) -> samples. Above depth 1 the per-operation time is
    a batch divided by its size, so it is inverse throughput rather than a
    latency; only depth 1 goes on the time panel."""
    us, bw = defaultdict(list), defaultdict(list)
    for r in csv.DictReader(open(CSV)):
        k = (r["bench"], int(r["size"]), int(r["depth"]))
        us[k].append(float(r["median_us"]))
        bw[k].append(float(r["gbps"]))
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
    fig, (ax_t, ax_b) = plt.subplots(2, 1, figsize=(7.6, 7.4), sharex=True)

    for op, color in OPS:
        ax_t.plot(sizes, [med(us, op, s, 1) for s in sizes], color=color,
                  linewidth=2, marker="o", markersize=5,
                  label=f"{op}, one at a time", zorder=3)
        for d in SHOWN_DEPTHS:
            ax_b.plot(sizes, [med(bw, op, s, d) for s in sizes], color=color,
                      linestyle="-" if d == 1 else "--", linewidth=2,
                      marker="o", markersize=5,
                      label=f"{op}, {d} outstanding" if d != 1
                            else f"{op}, one at a time", zorder=3)

    for ax in (ax_t, ax_b):
        ax.set_xscale("log", base=2)
        ax.set_xticks(sizes)
        ax.set_xticklabels([fmt_bytes(s) for s in sizes])
        ax.grid(True, color=GRID, linewidth=0.6, zorder=0)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)

    ax_t.set_yscale("log")
    ax_t.set_ylabel("time per operation (µs)")
    ax_t.set_title("One-sided access to a remote slot, server CPU uninvolved",
                   fontsize=10.5, loc="left")
    ax_t.legend(loc="upper left", frameon=False, fontsize=8.5)

    ax_b.set_ylim(0, 108)
    ax_b.set_ylabel("rate (Gbps)")
    ax_b.set_xlabel("slot size (bytes)")
    ax_b.axhline(100, color=INK_2, linewidth=0.8, linestyle=":", zorder=1)
    ax_b.annotate("100 GbE link", xy=(sizes[0], 100), xytext=(0, -12),
                  textcoords="offset points", fontsize=8, color=INK_2)
    # Lower right: the link marker sits top left and every curve rises
    # from the left, so that corner is the only clear one.
    ax_b.legend(loc="lower right", frameon=False, fontsize=8)

    fig.tight_layout()
    out = f"{OUTDIR}/phase4-oneside.png"
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def tables(us, bw):
    sizes = sorted({k[1] for k in us})
    print("\none operation at a time: time and the rate it implies")
    print(f"{'slot':>8} {'write us':>9} {'read us':>9} {'read/write':>11} "
          f"{'w Gbps':>8} {'r Gbps':>8}")
    for s in sizes:
        w, r = med(us, "write", s, 1), med(us, "read", s, 1)
        print(f"{fmt_bytes(s):>8} {w:>9.2f} {r:>9.2f} {r/w:>10.2f}x "
              f"{med(bw, 'write', s, 1):>8.1f} {med(bw, 'read', s, 1):>8.1f}")

    depths = sorted({k[2] for k in bw})
    print("\nrate (Gbps) against operations outstanding")
    print(f"{'slot':>8} " + " ".join(f"{'w d'+str(d):>8}" for d in depths)
          + " " + " ".join(f"{'r d'+str(d):>8}" for d in depths))
    for s in sizes:
        print(f"{fmt_bytes(s):>8} "
              + " ".join(f"{med(bw,'write',s,d):>8.1f}" for d in depths) + " "
              + " ".join(f"{med(bw,'read',s,d):>8.1f}" for d in depths))

    print("\nrun-to-run spread at depth 1, IQR as % of median")
    for op, _ in OPS:
        iqr = []
        for s in sizes:
            v = us[(op, s, 1)]
            q = st.quantiles(v, n=4)
            iqr.append((q[2] - q[0]) / st.median(v) * 100)
        print(f"  {op:6s} median {st.median(iqr):5.2f}%   worst {max(iqr):5.2f}%")


if __name__ == "__main__":
    u, b = load()
    figure(u, b)
    tables(u, b)
