#!/bin/bash
# Phase 4 sweep: one-sided write and read against a remote slab, across slot
# sizes. Emits results/phase4_sweep.csv. Assumes scripts/setup_netns.sh has run.
#
# Same conditions as the phase 1-3 sweeps: performance governor, irqbalance
# off, server and client pinned to different physical cores.
#
# One arm only. The point of this phase is that the server's CPU is not in the
# data path at all, which TCP cannot express -- it has no one-sided primitive,
# so a TCP arm would have to be the server actively receiving, and would be
# measuring a different thing rather than the same thing more slowly.
set -u

cd "$(cd "$(dirname "$0")/.." && pwd)"
SRV=./build/phase4_kv_cache/kv_server
CLI=./build/phase4_kv_cache/kv_bench
OUT=results/phase4_sweep.csv
# KV block sizes: a single layer's K/V for one request lands in this range.
SIZES=(4096 16384 65536 262144 1048576 4194304)
# Both directions are swept at each depth. One at a time is what a decode step
# does; the higher depths are there because a read issued alone cannot fill the
# link -- the queue pair carries one at a time until several are posted -- while
# a write can.
DEPTHS=(1 4 16)
REPS=15
NUM_SLOTS=8
SRV_CORE=2
CLI_CORE=3

[ -x "$SRV" ] && [ -x "$CLI" ] || { echo "build first: cmake --build build" >&2; exit 1; }
sudo -v || exit 1
sudo pkill -9 -f "kv_server|kv_bench" 2>/dev/null
mkdir -p results

# Saved so cleanup can undo a killed child leaving echo or ONLCR off, which
# makes the shell print a staircase and appear to ignore Enter.
TTY_SAVED=$(stty -g 2>/dev/null)
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$g" >/dev/null
done
sudo systemctl stop irqbalance 2>/dev/null

restore() {
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo "$GOV" | sudo tee "$g" >/dev/null
    done
    sudo systemctl start irqbalance 2>/dev/null
    sudo pkill -9 -f "kv_server|kv_bench" 2>/dev/null
    [ -n "$TTY_SAVED" ] && stty "$TTY_SAVED" 2>/dev/null
}
trap restore EXIT

# Fewer iterations once a single transfer costs hundreds of microseconds;
# every point still covers a comfortable stretch of wall time.
iters_for() { [ "$1" -le 262144 ] && echo 2000 || echo 500; }

"$CLI" --csv-header > "$OUT"

port=32000
for rep in $(seq 1 $REPS); do
    for size in "${SIZES[@]}"; do
      for depth in "${DEPTHS[@]}"; do
        iters=$(iters_for "$size")
        # </dev/null so the backgrounded sudo never reaches for the terminal;
        # a SIGKILL landing while it has echo off leaves the shell unusable.
        sudo taskset -c $SRV_CORE ip netns exec ns1 \
            "$SRV" $port $NUM_SLOTS "$size" </dev/null >/dev/null 2>&1 &
        srv=$!
        sleep 0.5
        rows=$(sudo timeout 120 taskset -c $CLI_CORE ip netns exec ns2 \
            "$CLI" 192.168.100.1 $port --iters "$iters" --depth "$depth" --csv \
            </dev/null 2>/dev/null | grep -E '^(write|read),')
        if [ -n "$rows" ]; then
            echo "$rows" >> "$OUT"
        else
            echo "FAILED: size=$size depth=$depth rep=$rep" >&2
        fi
        wait $srv 2>/dev/null
        sudo pkill -9 -f "kv_server|kv_bench" 2>/dev/null
        # RDMA listen also opens an OOB socket at data_port+1, and data is at
        # port+2, so each run consumes four.
        port=$((port + 4))
      done
    done
    echo "rep $rep/$REPS done"
done

echo "wrote $OUT ($(( $(wc -l < "$OUT") - 1 )) rows)"
