#!/bin/bash
# Phase 2 sweep: RDMA vs TCP through the Transport interface, across sizes.
# Emits results/phase2_sweep.csv. Assumes scripts/setup_netns.sh has run.
#
# Conditions match the phase 1 sweep: performance governor, irqbalance off,
# server and client pinned to different physical cores.
#
# TCP is tuned for latency so the comparison is against a TCP that was given
# every chance rather than a default one: NODELAY (in the code), interrupt
# coalescing turned down, and busy polling so recv() spins on the NIC queue
# instead of sleeping for an interrupt — the same thing the RDMA side does in
# poll_cq. Every one of those trades CPU for latency, which is why none is a
# system default.
#
# The sweep measures TCP under both configurations so the effect is in the
# data rather than asserted. conn=tcp is the latency configuration;
# conn=tcp_untuned is adaptive interrupt coalescing with no busy polling —
# the conventional throughput-oriented setting, not a claim about what this
# box shipped with. RDMA touches neither interrupts nor the scheduler on its
# data path, so it is measured once.
set -u

cd "$(cd "$(dirname "$0")/.." && pwd)"
BIN=./build/phase2_transport/backend_compare
OUT=results/phase2_sweep.csv
SIZES=(64 256 1024 4096 16384 65536 262144 1048576)
# 15 rather than 5: the RDMA arm settles within a handful of runs (0.4% between
# its fastest and slowest run, median over sizes), while the TCP arms move 15%
# and 22% and the untuned one is bimodal at 16 KB. A 5-run median there is only
# good to roughly +-8%.
REPS=15
# Iterations per run, by message size: enough wall time at every point without
# spending minutes on the large ones. Note this does not stabilise the untuned
# TCP arm — at 4 KB and 16 KB it varies about 2.4x run to run at both 2000 and
# 20000 iterations, so its median is not a firm point estimate there. No
# conclusion below rests on that arm; the gap is quoted against whichever TCP
# configuration measured better.
iters_for() { [ "$1" -le 16384 ] && echo 20000 || echo 5000; }
SRV_CORE=2
CLI_CORE=3
NS1_DEV=$(sudo ip netns exec ns1 ip -br link | awk '$1!="lo"{print $1; exit}')
NS2_DEV=$(sudo ip netns exec ns2 ip -br link | awk '$1!="lo"{print $1; exit}')

[ -x "$BIN" ] || { echo "build first: cmake --build build" >&2; exit 1; }
sudo -v || exit 1
sudo pkill -9 backend_compare 2>/dev/null
mkdir -p results

TTY_SAVED=$(stty -g 2>/dev/null)
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$g" >/dev/null
done
sudo systemctl stop irqbalance 2>/dev/null
# busy_read/busy_poll are global sysctls, not per-netns: set on the host, and
# read when a socket is created. Saved here so restore() can put them back.
BUSY_READ=$(cat /proc/sys/net/core/busy_read)
BUSY_POLL=$(cat /proc/sys/net/core/busy_poll)

tcp_tuning_on() {
    sudo ip netns exec ns1 ethtool -C "$NS1_DEV" adaptive-rx off rx-usecs 0 rx-frames 1 2>/dev/null
    sudo ip netns exec ns2 ethtool -C "$NS2_DEV" adaptive-rx off rx-usecs 0 rx-frames 1 2>/dev/null
    echo 50 | sudo tee /proc/sys/net/core/busy_read >/dev/null
    echo 50 | sudo tee /proc/sys/net/core/busy_poll >/dev/null
}

# The throughput-oriented configuration: let the NIC batch interrupts and let
# recv() sleep. NODELAY stays on — it lives in the code, not in a sysctl, and
# turning it off would measure Nagle rather than this comparison.
tcp_tuning_off() {
    sudo ip netns exec ns1 ethtool -C "$NS1_DEV" adaptive-rx on 2>/dev/null
    sudo ip netns exec ns2 ethtool -C "$NS2_DEV" adaptive-rx on 2>/dev/null
    echo 0 | sudo tee /proc/sys/net/core/busy_read >/dev/null
    echo 0 | sudo tee /proc/sys/net/core/busy_poll >/dev/null
}

restore() {
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo "$GOV" | sudo tee "$g" >/dev/null
    done
    sudo systemctl start irqbalance 2>/dev/null
    echo "$BUSY_READ" | sudo tee /proc/sys/net/core/busy_read >/dev/null
    echo "$BUSY_POLL" | sudo tee /proc/sys/net/core/busy_poll >/dev/null
    sudo ip netns exec ns1 ethtool -C "$NS1_DEV" adaptive-rx on 2>/dev/null
    sudo ip netns exec ns2 ethtool -C "$NS2_DEV" adaptive-rx on 2>/dev/null
    # Restore the exact terminal settings we started with, in case a killed
    # child left echo or ONLCR off.
    [ -n "$TTY_SAVED" ] && stty "$TTY_SAVED" 2>/dev/null
}
trap restore EXIT

# Bounded server cleanup: grace period, then SIGKILL (phase 1's reap).
reap() {
    local pid=$1 n=0
    while kill -0 "$pid" 2>/dev/null && [ $n -lt 30 ]; do sleep 0.1; n=$((n+1)); done
    if kill -0 "$pid" 2>/dev/null; then
        sudo kill -9 "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        return 1
    fi
    wait "$pid" 2>/dev/null
    return 0
}

"$BIN" rdma --csv-header > "$OUT"

port=21000
for rep in $(seq 1 $REPS); do
    for arm in rdma tcp tcp_untuned; do
        be=$arm
        if [ "$arm" = tcp_untuned ]; then be=tcp; tcp_tuning_off; else tcp_tuning_on; fi
        for size in "${SIZES[@]}"; do
            iters=$(iters_for "$size")
            # </dev/null so the backgrounded sudo can never reach for the
            # terminal to prompt. Without it, SIGKILL below can land while
            # sudo has echo and ONLCR turned off, and the shell is left
            # printing a staircase and swallowing Enter.
            sudo taskset -c $SRV_CORE ip netns exec ns1 \
                "$BIN" $be --port $port --size "$size" --iters $iters --csv \
                </dev/null >/dev/null 2>&1 &
            srv=$!
            sleep 0.4
            row=$(sudo timeout 120 taskset -c $CLI_CORE ip netns exec ns2 \
                "$BIN" $be 192.168.100.1 --port $port --size "$size" --iters $iters --csv 2>/dev/null)
            if [ -n "$row" ]; then
                # The binary only knows it is TCP; the arm name carries which
                # system configuration it ran under.
                echo "${row//,tcp,/,$arm,}" >> "$OUT"
            else
                echo "FAILED: $arm size=$size rep=$rep" >&2
            fi
            reap $srv || echo "server killed: $arm size=$size rep=$rep" >&2
            port=$((port + 4))   # sendrecv uses port, write uses port+2/+3
        done
    done
    echo "rep $rep/$REPS done"
done

echo "wrote $OUT ($(( $(wc -l < "$OUT") - 1 )) rows)"
