#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_SCRIPT="${ROOT_DIR}/deploy/benchmark_ec2.sh"

if [[ ! -x "${BENCH_SCRIPT}" ]]; then
    echo "[matrix] benchmark script not executable: ${BENCH_SCRIPT}" >&2
    exit 1
fi

DURATIONS_CSV="${DURATIONS:-300,1800}"
REPEATS="${REPEATS:-1}"
WARMUP_SECONDS="${WARMUP_SECONDS:-20}"
RUN_LABEL_PREFIX="${RUN_LABEL_PREFIX:-matrix}"

IFS=',' read -r -a DURATIONS <<<"${DURATIONS_CSV}"

CONFIGS=("$@")
if [[ ${#CONFIGS[@]} -eq 0 ]]; then
    CONFIGS=(
        "${ROOT_DIR}/config.benchmark.json"
    )
fi

for config in "${CONFIGS[@]}"; do
    if [[ ! -f "${config}" ]]; then
        echo "[matrix] missing config: ${config}" >&2
        exit 1
    fi

    config_name="$(basename "${config}" .json)"
    build_needed=1

    for duration in "${DURATIONS[@]}"; do
        run_label="${RUN_LABEL_PREFIX}-${config_name}-${duration}s"
        echo "[matrix] config=${config} duration=${duration}s repeats=${REPEATS} warmup=${WARMUP_SECONDS}s"
        SKIP_BUILD=$(( build_needed == 0 ? 1 : 0 )) \
        REPEATS="${REPEATS}" \
        WARMUP_SECONDS="${WARMUP_SECONDS}" \
        RUN_LABEL="${run_label}" \
        "${BENCH_SCRIPT}" "${config}" "${duration}"
        build_needed=0
    done
done
