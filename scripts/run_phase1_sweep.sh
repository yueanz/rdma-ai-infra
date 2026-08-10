#!/bin/bash
# Sweep the phase1 benchmarks and, at the same points, the perftest tool they
# correspond to. The baseline is the point of the exercise: a curve that tracks
# ib_write_lat / ib_write_bw is evidence the from-scratch implementation is
# correct and competitive, which the numbers alone cannot show.
#
# Assumes setup_netns.sh has already placed the two NICs in ns1 / ns2.
# Usage: sudo bash scripts/run_phase1_sweep.sh [output.csv]
#   Defaults to results/phase1_sweep.csv, the dataset the figures are drawn
#   from and the one file of this kind that is committed. Pass a name to keep
#   a run out of the way — those stay ignored.
#
# Knobs (env): REPS CONN_REPS ITERS TIMEOUT SERVER_CPU CLIENT_CPU
#   REPS=1 CONN_REPS=2 bash scripts/run_phase1_sweep.sh   # quick smoke run
set -uo pipefail

NS1=ns1
NS2=ns2
IP1=192.168.100.1
BIN=${BIN:-build/phase1_verbs}
CSV=${1:-results/phase1_sweep.csv}

REPS=${REPS:-5}
CONN_REPS=${CONN_REPS:-10}   # cm-vs-verbs is a null result; repeat it instead of sweeping it
ITERS=${ITERS:-1000}          # latency: one sample per iteration
BW_BYTES=${BW_BYTES:-268435456}   # bandwidth: iterations scaled to move this much
TIMEOUT=${TIMEOUT:-60}

# Both processes busy-spin, so they must not land on two hyperthreads of the
# same physical core. On this box the siblings are (0,6) (1,7) ... (5,11).
SERVER_CPU=${SERVER_CPU:-2}
CLIENT_CPU=${CLIENT_CPU:-3}

LAT_SIZES=${LAT_SIZES:-"64 256 1024 4096 16384 65536 262144 1048576"}
BW_SIZES=${BW_SIZES:-"1024 4096 16384 65536 262144 1048576"}
DEPTHS=${DEPTHS:-"1 2 4 8 16 32 64"}
RQ_DEPTHS=${RQ_DEPTHS:-"1 2 4 8 16 32 64"}
LAT_DEPTH=${LAT_DEPTH:-64}       # receive queue depth for the latency sweeps
# Small messages are message-rate bound, not link bound, so a shallow window
# leaves both tools short of line rate and the curve measures the window
# instead of the size. Keep it deep enough that neither is the bottleneck.
BW_SIZE_DEPTH=${BW_SIZE_DEPTH:-64}
REF_SIZE=${REF_SIZE:-65536}      # fixed size for the depth sweep
LAT_SIZE=${LAT_SIZE:-4096}       # fixed size for the receive-queue-depth sweep
CONN_SIZE=${CONN_SIZE:-4096}     # fixed size for the cm-vs-verbs comparison

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root (sudo bash $0)" >&2
    exit 1
fi
if [ ! -x "$BIN/lat_send_recv" ]; then
    echo "benchmarks not built — run: cmake -B build && cmake --build build -j" >&2
    exit 1
fi

# Stopping irqbalance and pinning the governor were measured together on this
# box (RDMA send/recv, 5 runs a point): leaving both at their defaults costs
# +4% at 64 B, +11% at 1 KB, +3.5% at 16 KB, and nothing by 256 KB, and widens
# the run-to-run spread at 16 KB from 0.3% to 2.7%. The two were not varied
# separately, so which one earns which share is unmeasured.
# Saved so cleanup can undo a killed child leaving echo or ONLCR off, which
# makes the shell print a staircase and appear to ignore Enter.
TTY_SAVED=$(stty -g 2>/dev/null)

IRQBALANCE_WAS_ACTIVE=0
if systemctl is-active --quiet irqbalance 2>/dev/null; then
    IRQBALANCE_WAS_ACTIVE=1
    systemctl stop irqbalance
fi

# The mechanism for the governor half: small messages are message-rate bound,
# so the cost is the CPU posting work requests, and a scaling governor parks a
# core that is busy-waiting rather than obviously busy. Large transfers are
# link bound and stop caring somewhere between 16 KB and 256 KB.
GOV_SAVED=""
if [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    GOV_SAVED=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$g" 2>/dev/null
    done
    echo "cpu governor: $GOV_SAVED -> performance"
fi

cleanup() {
    [ -n "$TTY_SAVED" ] && stty "$TTY_SAVED" 2>/dev/null
    pkill -f "$BIN/" 2>/dev/null
    pkill -f "ib_(send|write)_(lat|bw)" 2>/dev/null
    [ "$IRQBALANCE_WAS_ACTIVE" -eq 1 ] && systemctl start irqbalance
    if [ -n "$GOV_SAVED" ]; then
        for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            echo "$GOV_SAVED" > "$g" 2>/dev/null
        done
    fi
    return 0
}
trap cleanup EXIT

# ---- discover what each namespace holds -------------------------------------
# Mirrors find_roce_v2_gid(): prefer the IPv4-mapped RoCE v2 entry, so perftest
# is pinned to the same GID the implementation picks. Without this the two would
# be addressing over different IP versions and the comparison would be void.
gid_index() {
    local ns=$1 dev=$2
    ip netns exec "$ns" bash -c '
        dev=$1; fallback=""
        for g in /sys/class/infiniband/$dev/ports/1/gids/*; do
            [ -e "$g" ] || continue
            gid=$(cat "$g"); idx=$(basename "$g")
            [ "$gid" = "0000:0000:0000:0000:0000:0000:0000:0000" ] && continue
            pd=$(dirname "$(dirname "$g")")
            [ "$(cat "$pd/gid_attrs/types/$idx" 2>/dev/null)" = "RoCE v2" ] || continue
            case "$gid" in
                0000:0000:0000:0000:0000:ffff:*) echo "$idx"; exit 0 ;;
                fe80:*) : ;;
                *) [ -z "$fallback" ] && fallback=$idx ;;
            esac
        done
        echo "${fallback:-0}"' _ "$dev"
}

NS1_DEV=$(ip netns exec "$NS1" rdma dev show | awk -F': ' '{print $2; exit}')
NS2_DEV=$(ip netns exec "$NS2" rdma dev show | awk -F': ' '{print $2; exit}')
NS1_GID=$(gid_index "$NS1" "$NS1_DEV")
NS2_GID=$(gid_index "$NS2" "$NS2_DEV")
echo "$NS1: $NS1_DEV gid=$NS1_GID    $NS2: $NS2_DEV gid=$NS2_GID"

# The C side owns the column list; `rep` is the script's own, since the
# benchmark has no idea it is being run repeatedly.
mkdir -p "$(dirname "$CSV")"
{ "$BIN/lat_send_recv" --csv-header | tr -d '\n'; echo ",rep"; } > "$CSV"

PORT=13000
next_port() {
    # port and port+1 are both used, and a just-closed listener can linger in
    # TIME_WAIT, so never reuse one inside a sweep.
    PORT=$((PORT + 10))
    [ $PORT -gt 60000 ] && PORT=13000
    echo $PORT
}

# Only the client is wrapped in `timeout`. A failed run can leave the server
# blocked forever in accept(), so give it a moment to exit on its own and then
# make sure it does — an unbounded wait here stalls the whole sweep.
reap() {
    local pid=$1 i
    for i in $(seq 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

# A fixed iteration count makes the small-size bandwidth points meaningless:
# 1 KB x 1000 moves 1 MB, which at ~90 Gbps is under half a millisecond — all
# pipeline fill, no steady state. Scale so every point moves the same volume.
bw_iters() {
    local size=$1 n=$((BW_BYTES / $1))
    [ "$n" -lt 1000 ] && n=1000
    echo "$n"
}

runs=0
failures=0
fail() {
    failures=$((failures + 1))
    printf '  FAILED %-22s conn=%-5s size=%-8s depth=%-3s rep=%s (rc=%s)\n' \
        "$1" "$2" "$3" "$4" "$5" "$6" >&2
}

# ---- our benchmarks ---------------------------------------------------------
run_mine() {
    local bench=$1 conn=$2 size=$3 depth=$4 rep=$5 iters=${6:-$ITERS}
    local port out rc srv
    port=$(next_port); runs=$((runs + 1))

    taskset -c "$SERVER_CPU" ip netns exec "$NS1" "$BIN/$bench" \
        --port "$port" --conn "$conn" --size "$size" --depth "$depth" \
        --iters "$iters" </dev/null >/dev/null 2>&1 &
    srv=$!
    sleep 0.3   # both listeners go up before the device work, so this is margin

    out=$(timeout "$TIMEOUT" taskset -c "$CLIENT_CPU" ip netns exec "$NS2" \
        "$BIN/$bench" "$IP1" --port "$port" --conn "$conn" --size "$size" \
        --depth "$depth" --iters "$iters" --csv 2>/dev/null)
    rc=$?
    reap "$srv"

    if [ $rc -eq 0 ] && [ -n "$out" ]; then
        echo "$out,$rep" >> "$CSV"
    else
        fail "$bench" "$conn" "$size" "$depth" "$rep" "$rc"
    fi
}

# ---- perftest baseline ------------------------------------------------------
# perftest prints a fixed-width table; the data line is the first numeric line
# after the "#bytes" header. Column order is from perftest_parameters.h:
#   lat: bytes iters t_min t_max t_typical t_avg t_stdev p99 p99.9
#   bw : bytes iters BW_peak BW_average MsgRate
#
# UNITS DIFFER, and the rows are written raw: for SEND and WRITE, perftest
# halves the round trip before reporting (perftest_parameters.c:3196,
# rtt_factor = 2), so its latency columns are one-way while ours are full RTT.
# Halve ours, or double perftest's, before putting them on one axis.
run_perftest() {
    local tool=$1 kind=$2 size=$3 depth=$4 rep=$5 iters=${6:-$ITERS}
    local port out rc srv extra=()
    port=$(next_port); runs=$((runs + 1))
    # -t is the send window, -r the receive queue: the same knob our --depth
    # maps to for the corresponding test, so the two stay comparable. Only the
    # send tests take -r; a write test rejects anything but 1 ("On RDMA verbs
    # rx depth can be only 1") since it never consumes a receive WR.
    if   [ "$kind" = "bw" ];        then extra=(-t "$depth" --report_gbits)
    elif [ "$tool" = "ib_send_lat" ]; then extra=(-r "$depth")
    fi

    taskset -c "$SERVER_CPU" ip netns exec "$NS1" "$tool" \
        -d "$NS1_DEV" -x "$NS1_GID" -s "$size" -n "$iters" -p "$port" \
        "${extra[@]}" </dev/null >/dev/null 2>&1 &
    srv=$!
    sleep 0.3

    out=$(timeout "$TIMEOUT" taskset -c "$CLIENT_CPU" ip netns exec "$NS2" \
        "$tool" -d "$NS2_DEV" -x "$NS2_GID" -s "$size" -n "$iters" -p "$port" \
        "${extra[@]}" "$IP1" 2>/dev/null)
    rc=$?
    reap "$srv"

    if [ $rc -ne 0 ]; then
        fail "$tool" verbs "$size" "$depth" "$rep" "$rc"
        return
    fi
    local row
    row=$(echo "$out" | awk -v t="perftest_$tool" -v k="$kind" -v s="$size" \
                            -v d="$depth" -v n="$iters" -v r="$rep" '
        /#bytes/ { hdr = 1; next }
        hdr && /^[[:space:]]*[0-9]/ {
            if (k == "lat") printf "%s,verbs,%s,%s,%s,%s,%s,%s,%s,,%s\n", t,s,d,n,$3,$5,$8,$4,r
            else            printf "%s,verbs,%s,%s,%s,,,,,%s,%s\n",     t,s,d,n,$4,r
            exit
        }')
    if [ -n "$row" ]; then
        echo "$row" >> "$CSV"
    else
        fail "$tool(parse)" verbs "$size" "$depth" "$rep" "0"
    fi
}

# ---- 1. latency vs size, ours against perftest ------------------------------
echo "=== 1. latency vs size (mine + perftest) ==="
for size in $LAT_SIZES; do
    for rep in $(seq 1 "$REPS"); do
        run_mine     lat_send_recv  verbs "$size" "$LAT_DEPTH" "$rep"
        run_mine     lat_rdma_write verbs "$size" "$LAT_DEPTH" "$rep"
        run_perftest ib_send_lat    lat   "$size" "$LAT_DEPTH" "$rep"
        run_perftest ib_write_lat   lat   "$size" "$LAT_DEPTH" "$rep"
    done
    echo "  size=$size done"
done

# ---- 1b. latency vs receive queue depth -------------------------------------
# An arriving send needs a receive WQE before the HCA can place it. With a
# shallow queue the WQE has to be fetched from host memory on every arrival,
# putting a PCIe round trip on the critical path; perftest pre-posts rx_depth
# (512 by default) so the HCA can keep them prefetched. This sweep is where
# that shows up — lat_rdma_write is included as the control, since one-sided
# writes consume no receive WRs and so should be flat.
echo "=== 1b. latency vs receive queue depth (size $LAT_SIZE) ==="
for d in $RQ_DEPTHS; do
    for rep in $(seq 1 "$REPS"); do
        run_mine     lat_send_recv  verbs "$LAT_SIZE" "$d" "$rep"
        run_mine     lat_rdma_write verbs "$LAT_SIZE" "$d" "$rep"
        run_perftest ib_send_lat    lat   "$LAT_SIZE" "$d" "$rep"
    done
    echo "  rq_depth=$d done"
done

# ---- 2. bandwidth vs size ---------------------------------------------------
echo "=== 2. bandwidth vs size (depth $BW_SIZE_DEPTH) ==="
for size in $BW_SIZES; do
    for rep in $(seq 1 "$REPS"); do
        n=$(bw_iters "$size")
        run_mine     bw_rdma_write verbs "$size" "$BW_SIZE_DEPTH" "$rep" "$n"
        run_perftest ib_write_bw   bw    "$size" "$BW_SIZE_DEPTH" "$rep" "$n"
    done
    echo "  size=$size done"
done

# ---- 3. bandwidth vs depth --------------------------------------------------
echo "=== 3. bandwidth vs depth (size $REF_SIZE) ==="
for depth in $DEPTHS; do
    for rep in $(seq 1 "$REPS"); do
        run_mine     bw_rdma_write verbs "$REF_SIZE" "$depth" "$rep" "$(bw_iters "$REF_SIZE")"
        run_perftest ib_write_bw   bw    "$REF_SIZE" "$depth" "$rep" "$(bw_iters "$REF_SIZE")"
    done
    echo "  depth=$depth done"
done

# ---- 4. cm vs verbs, one point, repeated ------------------------------------
# Connection setup is not in the data path, so this is expected to come out as
# noise. Repeating one point says that with more confidence than sweeping would.
echo "=== 4. cm vs verbs (size $CONN_SIZE, $CONN_REPS reps) ==="
for conn in cm verbs; do
    for rep in $(seq 1 "$CONN_REPS"); do
        run_mine lat_send_recv  "$conn" "$CONN_SIZE" "$LAT_DEPTH" "$rep"
        run_mine lat_rdma_write "$conn" "$CONN_SIZE" "$LAT_DEPTH" "$rep"
        run_mine bw_rdma_write  "$conn" "$REF_SIZE"  16           "$rep" "$(bw_iters "$REF_SIZE")"
    done
    echo "  $conn done"
done

echo ""
echo "=== Done: $runs runs, $failures failed -> $CSV ==="
[ "$failures" -eq 0 ]
