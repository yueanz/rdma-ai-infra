#!/bin/bash
# Phase 3 sweep: ring all-reduce over RDMA and TCP, across buffer sizes.
# Emits results/phase3_sweep.csv. Assumes scripts/setup_netns.sh has run.
#
# Same conditions and the same three arms as the phase 2 sweep, so the two
# sets of numbers can be read against each other: performance governor,
# irqbalance off, the two ranks pinned to different physical cores, and TCP
# measured both tuned for latency and at its default settings.
#
# The two ranks are two processes on this one host, rank 0 in ns1 and rank 1
# in ns2, wired through the pair of NICs. A world of two is the whole ring, so
# reduce-scatter and all-gather are one step each.
set -u

cd "$(cd "$(dirname "$0")/.." && pwd)"
BIN=./build/phase3_collective/allreduce_bench
OUT=results/phase3_sweep.csv
# float32 counts: 64 KB, 256 KB, 1 MB, 4 MB, 16 MB of buffer.
COUNTS=(16384 65536 262144 1048576 4194304)
REPS=15
ITERS=200
R0_CORE=2
R1_CORE=3
NS1_DEV=$(sudo ip netns exec ns1 ip -br link | awk '$1!="lo"{print $1; exit}')
NS2_DEV=$(sudo ip netns exec ns2 ip -br link | awk '$1!="lo"{print $1; exit}')

[ -x "$BIN" ] || { echo "build first: cmake --build build" >&2; exit 1; }
sudo -v || exit 1
sudo pkill -9 allreduce_bench 2>/dev/null
mkdir -p results

# Saved so cleanup can undo a killed child leaving echo or ONLCR off, which
# makes the shell print a staircase and appear to ignore Enter.
TTY_SAVED=$(stty -g 2>/dev/null)
GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
BUSY_READ=$(cat /proc/sys/net/core/busy_read)
BUSY_POLL=$(cat /proc/sys/net/core/busy_poll)

for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$g" >/dev/null
done
sudo systemctl stop irqbalance 2>/dev/null

tcp_tuning_on() {
    sudo ip netns exec ns1 ethtool -C "$NS1_DEV" adaptive-rx off rx-usecs 0 rx-frames 1 2>/dev/null
    sudo ip netns exec ns2 ethtool -C "$NS2_DEV" adaptive-rx off rx-usecs 0 rx-frames 1 2>/dev/null
    echo 50 | sudo tee /proc/sys/net/core/busy_read >/dev/null
    echo 50 | sudo tee /proc/sys/net/core/busy_poll >/dev/null
}
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
    sudo pkill -9 allreduce_bench 2>/dev/null
    [ -n "$TTY_SAVED" ] && stty "$TTY_SAVED" 2>/dev/null
}
trap restore EXIT

"$BIN" --csv-header > "$OUT"

port=27000
for rep in $(seq 1 $REPS); do
    for arm in rdma tcp tcp_untuned; do
        flag=""
        if [ "$arm" = rdma ]; then flag="--rdma"; tcp_tuning_on
        elif [ "$arm" = tcp ]; then tcp_tuning_on
        else tcp_tuning_off
        fi
        for count in "${COUNTS[@]}"; do
            # </dev/null so the backgrounded sudo never reaches for the
            # terminal; a SIGKILL landing while it has echo off leaves the
            # shell unusable.
            sudo taskset -c $R1_CORE ip netns exec ns2 \
                "$BIN" 1 2 $port 192.168.100.1 192.168.100.2 \
                --count "$count" --iters $ITERS $flag --csv \
                </dev/null >/dev/null 2>&1 &
            r1=$!
            sleep 0.4
            row=$(sudo timeout 180 taskset -c $R0_CORE ip netns exec ns1 \
                "$BIN" 0 2 $port 192.168.100.1 192.168.100.2 \
                --count "$count" --iters $ITERS $flag --csv \
                </dev/null 2>/dev/null)
            # Only the data row: a stray progress line on stdout would
            # otherwise land in the middle of the CSV.
            row=$(printf '%s\n' "$row" | grep '^allreduce,' || true)
            if [ -n "$row" ]; then
                # The binary only knows rdma from tcp; the arm name carries
                # which system configuration it ran under.
                echo "${row//,tcp,/,$arm,}" >> "$OUT"
            else
                echo "FAILED: $arm count=$count rep=$rep" >&2
            fi
            wait $r1 2>/dev/null
            sudo pkill -9 allreduce_bench 2>/dev/null
            port=$((port + 2))
        done
    done
    echo "rep $rep/$REPS done"
done

echo "wrote $OUT ($(( $(wc -l < "$OUT") - 1 )) rows)"
