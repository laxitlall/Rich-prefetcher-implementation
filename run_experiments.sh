#!/bin/bash
set -e

# Configuration
BINARY="./bin/perceptron-no-multi-no-ship-1core"
WARMUP=50000000
SIM_INSTR=200000000
RESULTS_DIR="./results4kb"
MAX_JOBS=$(nproc)

mkdir -p "$RESULTS_DIR"

# 1. Build ChampSim binary
echo "=== Building ChampSim (no-multi-no-1core) ==="
./build_champsim.sh no multi no 1

# 2. Collect all traces from ./traces/
TRACES=()
while IFS= read -r t; do
    TRACES+=("$t")
done < <(find ./traces -type f \( -name "*.xz" -o -name "*.gz" -o -name "*.trace*" \) | sort)

if [ "${#TRACES[@]}" -eq 0 ]; then
    echo "[ERROR] No traces found in ./traces!"
    exit 1
fi

TOTAL_RUNS=$(( ${#TRACES[@]} * 4 ))
echo "=== Found ${#TRACES[@]} traces (${TOTAL_RUNS} total runs, running up to $MAX_JOBS jobs in parallel) ==="
for t in "${TRACES[@]}"; do
    echo "  - $t"
done
echo ""

# Helper to throttle background jobs to MAX_JOBS
wait_for_slot() {
    while [ "$(jobs -rp | wc -l)" -ge "$MAX_JOBS" ]; do
        sleep 1
    done
}

# 3. Launch all simulations in parallel (up to MAX_JOBS concurrent processes)
RUN_IDX=0
for TRACE in "${TRACES[@]}"; do
    TRACE_NAME=$(basename "$TRACE")
    TRACE_OUT_DIR="$RESULTS_DIR/$TRACE_NAME"
    mkdir -p "$TRACE_OUT_DIR"

    wait_for_slot
    RUN_IDX=$((RUN_IDX + 1))
    echo "[$RUN_IDX/$TOTAL_RUNS] Starting nopref on $TRACE_NAME"
    $BINARY --warmup_instructions=$WARMUP --simulation_instructions=$SIM_INSTR \
        --config=config/nopref.ini -traces "$TRACE" > "$TRACE_OUT_DIR/out_nopref.txt" 2>&1 &

    wait_for_slot
    RUN_IDX=$((RUN_IDX + 1))
    echo "[$RUN_IDX/$TOTAL_RUNS] Starting sms    on $TRACE_NAME"
    $BINARY --warmup_instructions=$WARMUP --simulation_instructions=$SIM_INSTR \
        --l2c_prefetcher_types=sms --config=config/sms.ini -traces "$TRACE" > "$TRACE_OUT_DIR/out_sms.txt" 2>&1 &

    wait_for_slot
    RUN_IDX=$((RUN_IDX + 1))
    echo "[$RUN_IDX/$TOTAL_RUNS] Starting bingo  on $TRACE_NAME"
    $BINARY --warmup_instructions=$WARMUP --simulation_instructions=$SIM_INSTR \
        --l2c_prefetcher_types=bingo --config=config/bingo.ini -traces "$TRACE" > "$TRACE_OUT_DIR/out_bingo.txt" 2>&1 &

    wait_for_slot
    RUN_IDX=$((RUN_IDX + 1))
    echo "[$RUN_IDX/$TOTAL_RUNS] Starting rich   on $TRACE_NAME"
    $BINARY --warmup_instructions=$WARMUP --simulation_instructions=$SIM_INSTR \
        --l2c_prefetcher_types=rich --config=config/rich.ini -traces "$TRACE" > "$TRACE_OUT_DIR/out_rich.txt" 2>&1 &
done

echo ""
echo "=== Waiting for remaining simulations to finish... ==="
wait

# 4. Print IPC Summary Table across all traces
echo ""
echo "================================================================================"
printf "%-42s | %-10s | %-10s | %-10s | %-10s\n" "Trace" "nopref" "sms" "bingo" "rich"
echo "--------------------------------------------------------------------------------"
for TRACE in "${TRACES[@]}"; do
    TRACE_NAME=$(basename "$TRACE")
    TRACE_OUT_DIR="$RESULTS_DIR/$TRACE_NAME"

    get_ipc() {
        awk '/^Core_0_IPC/ {print $2; exit}' "$1" 2>/dev/null || echo "N/A"
    }

    IPC_NO=$(get_ipc "$TRACE_OUT_DIR/out_nopref.txt")
    IPC_SMS=$(get_ipc "$TRACE_OUT_DIR/out_sms.txt")
    IPC_BINGO=$(get_ipc "$TRACE_OUT_DIR/out_bingo.txt")
    IPC_RICH=$(get_ipc "$TRACE_OUT_DIR/out_rich.txt")

    printf "%-42s | %-10s | %-10s | %-10s | %-10s\n" "${TRACE_NAME:0:42}" "$IPC_NO" "$IPC_SMS" "$IPC_BINGO" "$IPC_RICH"
done
echo "================================================================================"
echo "All runs completed! Full logs saved in $RESULTS_DIR/"
