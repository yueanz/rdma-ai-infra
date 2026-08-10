#!/usr/bin/env python3
"""Turn the phase2 sweep CSV into the README figure and tables.

The two TCP lines differ in two settings, not one: interrupt moderation and
whether recv() polls or sleeps. Tracing the interrupt-to-wakeup path showed it
is identical (4-6 us) either way, so of the ~25 us between them at 4 KB the
moderation delay is the bulk and blocking-vs-polling is about 7 us. The labels
therefore name the configuration rather than claiming a mechanism.

Usage: python3 scripts/plot_phase2.py [results/phase2_sweep.csv] [outdir]

Latency is reported one-way (the benchmark times a round trip; halved here),
matching the convention set by the phase 1 figures. write_ack rows are a
different quantity — write posted until its ACK arrives — and stay a table.
"""
import csv
import statistics as st
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

CSV = sys.argv[1] if len(sys.argv) > 1 else "results/phase2_sweep.csv"
PHASE1 = "results/phase1_sweep.csv"
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else "results"

# Same categorical slots as the phase 1 figures. Colour carries the backend.
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
    """(bench, backend, size) -> list of microseconds.

    sendrecv is a ping-pong and is halved to one-way, matching phase 1.
    write_ack is not a round trip through the peer's CPU — it is a write
    posted until the fabric acknowledges it — so it is left as measured."""
    lat = defaultdict(list)
    for r in csv.DictReader(open(CSV)):
        v = float(r["median_us"])
        if r["bench"] == "sendrecv":
            v /= 2
        lat[(r["bench"], r["conn"], int(r["size"]))].append(v)
    return lat


def med(lat, *k):
    return st.median(lat[k]) if lat.get(k) else None


def fig_rdma_vs_tcp(lat):
    sizes = sorted({k[2] for k in lat if k[0] == "sendrecv"})

    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    for be, color, style, label in (
        ("rdma", BLUE, "-", "RDMA backend"),
        ("tcp", ORANGE, "-", "TCP backend, latency-tuned"),
        ("tcp_untuned", ORANGE, "--", "TCP backend, default settings"),
    ):
        ys = [med(lat, "sendrecv", be, s) for s in sizes]
        ax.plot(sizes, ys, color=color, linestyle=style, linewidth=2, marker="o",
                markersize=5, label=label, zorder=3)

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("message size (bytes)")
    ax.set_ylabel("one-way latency (µs)")
    ax.set_title("Same Transport interface, two backends", loc="left", fontsize=11)
    ax.set_xticks(sizes)
    ax.xaxis.set_major_formatter(FuncFormatter(
        lambda v, _: f"{int(v)}" if v < 1024 else f"{int(v/1024)}K" if v < 1048576 else "1M"))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    ax.set_xlim(sizes[0] / 1.6, sizes[-1] * 1.6)
    ax.grid(True, which="major", color=GRID, linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)

    # The gap is the story: name it at both ends.
    # Quote the gap against whichever TCP configuration did better at that
    # size — the latency tuning is a loss at 1 MB, and taking the tuned number
    # there would overstate the gap.
    for s in (sizes[0], sizes[-1]):
        r = med(lat, "sendrecv", "rdma", s)
        t = min(med(lat, "sendrecv", "tcp", s), med(lat, "sendrecv", "tcp_untuned", s))
        ax.annotate(f"{t/r:.1f}×", xy=(s, (r * t) ** 0.5), fontsize=9,
                    color=INK_2, ha="center", va="center")
    ax.legend(frameon=False, fontsize=8, handlelength=3.2, loc="upper left")

    fig.tight_layout()
    out = f"{OUTDIR}/phase2-rdma-vs-tcp.png"
    fig.savefig(out, dpi=170)
    print("wrote", out)


def tables(lat):
    sizes = sorted({k[2] for k in lat if k[0] == "sendrecv"})

    print("\nsend/recv one-way us. gap is against the better TCP configuration.")
    print(f"{'size':>9} {'rdma':>8} {'tcp lat':>8} {'tcp coal':>9} {'gap':>6} {'write+ack':>10}")
    for s in sizes:
        r = med(lat, "sendrecv", "rdma", s)
        t = med(lat, "sendrecv", "tcp", s)
        u = med(lat, "sendrecv", "tcp_untuned", s)
        w = med(lat, "write_ack", "rdma", s)
        print(f"{s:>9} {r:>8.2f} {t:>8.2f} {u:>9.2f} {min(t,u)/r:>5.1f}x {w:>10.2f}")

    p1 = defaultdict(list)
    for r in csv.DictReader(open(PHASE1)):
        if (r["bench"] == "lat_send_recv" and r["conn"] == "verbs"
                and int(r["depth"]) == 64 and r["median_us"].strip()):
            p1[int(r["size"])].append(float(r["median_us"]) / 2)

    # phase 1 only swept depth at 4 KB, so every size here compares its
    # depth-64 row against phase 2's depth-8 loop. At 4 KB those two differ by
    # 0.4%, so this overstates the abstraction cost slightly rather than
    # flattering it.
    print("\nabstraction cost: Transport layer vs phase 1 bare verbs (one-way us)")
    print(f"{'size':>9} {'bare':>8} {'transport':>10} {'cost':>7}")
    for s in sizes:
        if p1.get(s):
            a, b = st.median(p1[s]), med(lat, "sendrecv", "rdma", s)
            print(f"{s:>9} {a:>8.2f} {b:>10.2f} {(b-a)/a*100:>+6.1f}%")


if __name__ == "__main__":
    lat = load()
    fig_rdma_vs_tcp(lat)
    tables(lat)
