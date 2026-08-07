#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
DATA_DIR=${1:?"Usage: bash run_on_b.sh DATA_DIR [WORK_ROOT]"}
WORK_ROOT=${2:-"$SCRIPT_DIR/runs"}
BIN="$SCRIPT_DIR/bin/zsq_benchmark"

if [[ $(uname -m) != "aarch64" && $(uname -m) != "arm64" ]]; then
    echo "ERROR: B must be an ARM64 Linux machine; current architecture: $(uname -m)" >&2
    exit 1
fi
if [[ ! -x "$BIN" ]]; then
    echo "ERROR: benchmark binary not found: $BIN" >&2
    exit 1
fi
for file in sift_base.fvecs sift_query.fvecs sift_groundtruth.ivecs; do
    if [[ ! -f "$DATA_DIR/$file" ]]; then
        echo "ERROR: dataset file missing: $DATA_DIR/$file" >&2
        exit 1
    fi
done

export LD_LIBRARY_PATH="$SCRIPT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)
RUN_DIR="$WORK_ROOT/zsq_run_$RUN_ID"
mkdir -p "$RUN_DIR/indexes" "$RUN_DIR/results"

BASE_COUNT=${BASE_COUNT:-1000000}
QUERY_COUNT=${QUERY_COUNT:-10000}
DIM=${DIM:-128}
GROUNDTRUTH_K=${GROUNDTRUTH_K:-100}
THREAD_COUNT=${THREAD_COUNT:-16}
LINK_RANGE=${LINK_RANGE:-32}
LINK_CANDIDATE_SIZE=${LINK_CANDIDATE_SIZE:-300}
BUILD_ITER_COUNT=${BUILD_ITER_COUNT:-3}
BATCH_SIZE_MB=${BATCH_SIZE_MB:-1024}
ROTATOR_TYPE=${ROTATOR_TYPE:-0}
SEARCH_RANGES=${SEARCH_RANGES:-50,100,200,400}
TOP_KS=${TOP_KS:-1,10,100}
WARMUP_QUERIES=${WARMUP_QUERIES:-1000}
ROUNDS=${ROUNDS:-5}
BUILD_ORDER=${BUILD_ORDER:-rbq,zsq}
SEARCH_ORDER=${SEARCH_ORDER:-rbq,zsq}

RUN_PREFIX=()
if [[ -n ${CPUSET:-} ]]; then
    command -v taskset >/dev/null || { echo "ERROR: CPUSET was set but taskset is unavailable" >&2; exit 1; }
    RUN_PREFIX=(taskset -c "$CPUSET")
fi

COMMON_FLAGS=(
    "--base_count=$BASE_COUNT"
    "--query_count=$QUERY_COUNT"
    "--dim=$DIM"
    "--groundtruth_k=$GROUNDTRUTH_K"
    "--thread_count=$THREAD_COUNT"
    "--link_range=$LINK_RANGE"
    "--link_candidate_size=$LINK_CANDIDATE_SIZE"
    "--build_iter_count=$BUILD_ITER_COUNT"
    "--batch_size_mb=$BATCH_SIZE_MB"
    "--rotator_type=$ROTATOR_TYPE"
)

run_timed() {
    local time_file=$1
    shift
    if [[ -x /usr/bin/time ]]; then
        /usr/bin/time -v -o "$time_file" "${RUN_PREFIX[@]}" "$@"
    else
        echo "WARN: /usr/bin/time not found; external peak-RSS report will be absent" >&2
        "${RUN_PREFIX[@]}" "$@"
    fi
}

{
    echo "run_id=$RUN_ID"
    echo "bundle_manifest:"
    sed 's/^/  /' "$SCRIPT_DIR/manifest.txt"
    echo
    echo "uname:"
    uname -a
    echo
    echo "lscpu:"
    lscpu
    echo
    echo "memory:"
    free -h
    echo
    echo "glibc:"
    ldd --version 2>&1 | head -n 1
    echo
    echo "cpu_governors:"
    grep -H . /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null || true
    echo
    echo "parameters:"
    printf '%s\n' "${COMMON_FLAGS[@]}"
    echo "SEARCH_RANGES=$SEARCH_RANGES"
    echo "TOP_KS=$TOP_KS"
    echo "WARMUP_QUERIES=$WARMUP_QUERIES"
    echo "ROUNDS=$ROUNDS"
    echo "BUILD_ORDER=$BUILD_ORDER"
    echo "SEARCH_ORDER=$SEARCH_ORDER"
    echo "CPUSET=${CPUSET:-}"
} > "$RUN_DIR/results/environment.txt"

echo "[1/3] Validate SIFT files"
"${RUN_PREFIX[@]}" "$BIN" \
    --mode=validate \
    "--base_path=$DATA_DIR/sift_base.fvecs" \
    "--query_path=$DATA_DIR/sift_query.fvecs" \
    "--groundtruth_path=$DATA_DIR/sift_groundtruth.ivecs" \
    "${COMMON_FLAGS[@]}" | tee "$RUN_DIR/results/validate.log"

build_variant() {
    local variant=$1
    case "$variant" in rbq|zsq) ;; *) echo "ERROR: invalid build variant: $variant" >&2; exit 1 ;; esac
    echo "Build $variant"
    run_timed "$RUN_DIR/results/build_${variant}.time.txt" \
        "$BIN" \
        --mode=build \
        "--variant=$variant" \
        "--base_path=$DATA_DIR/sift_base.fvecs" \
        "--index_path=$RUN_DIR/indexes/${variant}.index" \
        "--output_csv=$RUN_DIR/results/build_${variant}.csv" \
        "${COMMON_FLAGS[@]}" 2>&1 | tee "$RUN_DIR/results/build_${variant}.log"
}

search_variant() {
    local variant=$1
    case "$variant" in rbq|zsq) ;; *) echo "ERROR: invalid search variant: $variant" >&2; exit 1 ;; esac
    echo "Search $variant"
    run_timed "$RUN_DIR/results/search_${variant}.time.txt" \
        "$BIN" \
        --mode=search \
        "--variant=$variant" \
        "--query_path=$DATA_DIR/sift_query.fvecs" \
        "--groundtruth_path=$DATA_DIR/sift_groundtruth.ivecs" \
        "--index_path=$RUN_DIR/indexes/${variant}.index" \
        "--output_csv=$RUN_DIR/results/search_${variant}.csv" \
        "--search_ranges=$SEARCH_RANGES" \
        "--top_ks=$TOP_KS" \
        "--warmup_queries=$WARMUP_QUERIES" \
        "--rounds=$ROUNDS" \
        "${COMMON_FLAGS[@]}" 2>&1 | tee "$RUN_DIR/results/search_${variant}.log"
}

echo "[2/3] Build indexes in order: $BUILD_ORDER"
IFS=',' read -r -a BUILD_VARIANTS <<< "$BUILD_ORDER"
for variant in "${BUILD_VARIANTS[@]}"; do
    build_variant "$variant"
done

echo "[3/3] Search indexes in order: $SEARCH_ORDER"
IFS=',' read -r -a SEARCH_VARIANTS <<< "$SEARCH_ORDER"
for variant in "${SEARCH_VARIANTS[@]}"; do
    search_variant "$variant"
done

(
    cd "$RUN_DIR"
    find results indexes -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)

echo "Run complete: $RUN_DIR"
echo "Please retain the entire run directory, especially results/*.csv, results/*.time.txt, environment.txt, and SHA256SUMS."
