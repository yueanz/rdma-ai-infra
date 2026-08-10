#!/usr/bin/env python3
"""Turn a phase1 sweep CSV into the figures the README embeds.

Usage: python3 scripts/plot_phase1.py [results/phase1_sweep.csv] [outdir]

Two units meet in this data and they are not the same quantity: our benchmarks
report a full round trip, perftest halves it before printing (rtt_factor = 2 in
perftest_parameters.c). Everything here is normalised to one-way microseconds so
the curves can share an axis.
"""
import csv
import statistics as st
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

CSV = sys.argv[1] if len(sys.argv) > 1 else "results/phase1_sweep.csv"
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else "results"

# Categorical slots 1 and 2 of the reference palette, which is documented as
# validated all-pairs in both modes. Colour carries the operation; line style
# carries whose implementation it is, so identity is never colour alone.
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


def load():
    """(bench, size, depth) -> list of one-way microseconds / Gbps."""
    lat, bw = defaultdict(list), defaultdict(list)
    for r in csv.DictReader(open(CSV)):
        if r["conn"] != "verbs":
            continue
        depth = int(r["depth"] or 0)
        # ib_send_lat rounds an odd -r up to the next even value before it runs
        # ("WA for a bug when rx_depth is odd in SEND", perftest_parameters.c),
        # so plot its points at the depth it actually used, not the one asked
        # for. Confirmed on this link: -r 1 and -r 2 measure the same, as do
        # 3 and 4, and 5 and 6.
        if r["bench"] == "perftest_ib_send_lat" and depth % 2 == 1:
            depth += 1
        key = (r["bench"], int(r["size"]), depth)
        if r["median_us"]:
            # ours is a round trip, perftest already halved its own
            half = 1 if r["bench"].startswith("perftest") else 2
            lat[key].append(float(r["median_us"]) / half)
        elif r["gbps"]:
            bw[key].append(float(r["gbps"]))
    return lat, bw


def line(ax, xs, series, color, style, label):
    """Median across repetitions. No error bar: run-to-run spread is under 1%
    almost everywhere, and points were not all repeated the same number of
    times, so a min-max whisker would have shown sample size more than noise."""
    ax.plot(xs, [st.median(series[x]) for x in xs], color=color, linestyle=style,
            linewidth=2, marker="o", markersize=5, label=label, zorder=3)


def legend(ax, **kw):
    ax.legend(frameon=False, fontsize=8, handlelength=3.2, **kw)


def dress(ax):
    ax.grid(True, which="major", color=GRID, linewidth=0.8, linestyle="-", zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)


def fig_latency_vs_size(lat):
    """Absolute latency across the size range.

    How closely this tracks perftest is a matter of a few percent, which no log
    axis spanning 0.8-95 us can show — that comparison lives in the table the
    README prints below the figure, where it can be read exactly."""
    sizes = sorted({k[1] for k in lat if k[0] == "lat_send_recv" and k[2] == 64})

    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    for bench, color, style, label in (
        ("lat_send_recv", BLUE, "-", "send/recv — this implementation"),
        ("perftest_ib_send_lat", BLUE, "--", "send/recv — ib_send_lat"),
        ("lat_rdma_write", ORANGE, "-", "write — this implementation"),
        ("perftest_ib_write_lat", ORANGE, "--", "write — ib_write_lat"),
    ):
        series = {s: lat[(bench, s, 64)] for s in sizes if lat.get((bench, s, 64))}
        if series:
            line(ax, sorted(series), series, color, style, label)

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("message size (bytes)")
    ax.set_ylabel("one-way latency (µs)")
    ax.set_title("One-way latency vs message size", loc="left", fontsize=11)
    ax.set_xticks(sizes)
    ax.xaxis.set_major_formatter(FuncFormatter(
        lambda v, _: f"{int(v)}" if v < 1024 else f"{int(v/1024)}K" if v < 1048576 else "1M"))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    ax.set_xlim(sizes[0] / 1.6, sizes[-1] * 1.6)
    ax.set_ylim(0.45, 160)
    dress(ax)
    legend(ax, loc="upper left")

    fig.tight_layout()
    out = f"{OUTDIR}/phase1-latency-vs-size.png"
    fig.savefig(out, dpi=170)
    print("wrote", out)


def fig_latency_vs_rqdepth(lat):
    """How many receive work requests have to stay posted.

    The write line is flat because a one-sided write never touches a receive
    queue — the knob does not reach it, so those seven points are the same run
    repeated. That is worth plotting anyway: had the machine drifted during the
    sweep it would have moved too, so its flatness is what makes the drop in the
    other two a real effect rather than an artefact.

    The perftest line starts at 2 because its -r cannot be driven below that;
    see the rounding note in load()."""
    SIZE = 4096
    depths = sorted({k[2] for k in lat if k[0] == "lat_send_recv" and k[1] == SIZE and k[2]})

    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    for bench, color, style, label in (
        ("lat_send_recv", BLUE, "-", "send/recv — this implementation"),
        ("perftest_ib_send_lat", BLUE, "--", "send/recv — perftest ib_send_lat (odd -r rounds up, so 1 lands on 2)"),
        ("lat_rdma_write", ORANGE, "-", "one-sided write — this implementation, no receive queue to deepen"),
    ):
        series = {d: lat[(bench, SIZE, d)] for d in depths if lat.get((bench, SIZE, d))}
        if series:
            line(ax, sorted(series), series, color, style, label)

    ax.set_xscale("log", base=2)
    ax.set_xlabel("receive work requests kept posted")
    ax.set_ylabel("one-way latency (µs)")
    ax.set_title("send/recv latency vs how deep the receive queue is kept",
                 loc="left", fontsize=11)
    ax.set_xticks(depths)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{int(v)}"))
    ax.set_xlim(depths[0] / 1.4, depths[-1] * 1.4)
    ax.set_ylim(0, 4.3)
    dress(ax)
    ax.annotate("keeping 8 posted is enough;\nkeeping 1 costs twice that",
                xy=(8, 1.84), xytext=(11, 2.8), fontsize=8, color=INK_2,
                arrowprops=dict(arrowstyle="-", color=INK_2, linewidth=0.8))
    legend(ax, loc="upper right")

    fig.tight_layout()
    out = f"{OUTDIR}/phase1-latency-vs-rq-depth.png"
    fig.savefig(out, dpi=170)
    print("wrote", out)


def table_latency(lat):
    print("\nlatency vs size, one-way us (depth 64)")
    print(f"{'size':>9} {'send/recv':>10} {'ib_send_lat':>12} {'ratio':>7} "
          f"{'write':>8} {'ib_write_lat':>13} {'ratio':>7}")
    for s in sorted({k[1] for k in lat if k[0] == "lat_send_recv"}):
        m = lambda b: st.median(lat[(b, s, 64)]) if lat.get((b, s, 64)) else None
        a, b_, c, d = (m("lat_send_recv"), m("perftest_ib_send_lat"),
                       m("lat_rdma_write"), m("perftest_ib_write_lat"))
        if None in (a, b_, c, d):
            continue
        print(f"{s:>9} {a:10.2f} {b_:12.2f} {a/b_:6.2f}x {c:8.2f} {d:13.2f} {c/d:6.2f}x")


def table_bandwidth(bw):
    print("\nbandwidth vs size (depth 64)")
    print(f"{'size':>9} {'this impl':>11} {'ib_write_bw':>12} {'ratio':>7}")
    for s in sorted({k[1] for k in bw if k[0] == "bw_rdma_write"}):
        a = bw.get(("bw_rdma_write", s, 64))
        b = bw.get(("perftest_ib_write_bw", s, 64))
        if a and b:
            ma, mb = st.median(a), st.median(b)
            print(f"{s:>9} {ma:10.2f}G {mb:11.2f}G {ma/mb*100:6.1f}%")


if __name__ == "__main__":
    lat, bw = load()
    fig_latency_vs_size(lat)
    fig_latency_vs_rqdepth(lat)
    table_latency(lat)
    table_bandwidth(bw)
