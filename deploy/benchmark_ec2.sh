#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/logs"
CONFIG_PATH="${1:-${ROOT_DIR}/config.json}"
DURATION_SECONDS="${2:-75}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
LIVE_OUTPUT="${LIVE_OUTPUT:-0}"
ALLOW_CONDA_BOOST="${PM_ALLOW_CONDA_BOOST:-ON}"
ALLOCATOR="${PM_ALLOCATOR:-system}"
REPEATS="${REPEATS:-1}"
WARMUP_SECONDS="${WARMUP_SECONDS:-0}"
SUMMARY_INTERVAL_OVERRIDE="${SUMMARY_INTERVAL_SECONDS:-}"
RUN_LABEL="${RUN_LABEL:-}"

if [[ ! -f "${CONFIG_PATH}" ]]; then
    echo "[bench] config not found: ${CONFIG_PATH}" >&2
    exit 1
fi

mkdir -p "${LOG_DIR}"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "[bench] building (${BUILD_TYPE})"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DPM_ALLOW_CONDA_BOOST="${ALLOW_CONDA_BOOST}" \
        -DPM_ALLOCATOR="${ALLOCATOR}"
    cmake --build "${BUILD_DIR}" -j"${JOBS}"
fi

if grep -Eq '"metrics_enabled"[[:space:]]*:[[:space:]]*false' "${CONFIG_PATH}"; then
    echo "[bench] warning: metrics_enabled=false in ${CONFIG_PATH}" >&2
    echo "[bench] warning: benchmarks will not print latency summaries" >&2
fi

if ulimit -l unlimited 2>/dev/null; then
    echo "[bench] memlock raised to unlimited"
else
    echo "[bench] memlock unchanged"
fi

echo "[bench] binary=${BUILD_DIR}/arb_detector"
echo "[bench] config=${CONFIG_PATH}"
echo "[bench] duration=${DURATION_SECONDS}s"
echo "[bench] repeats=${REPEATS}"
echo "[bench] warmup=${WARMUP_SECONDS}s"

if (( DURATION_SECONDS <= WARMUP_SECONDS )); then
    echo "[bench] warning: duration <= warmup; no post-warmup benchmark window will remain" >&2
fi

sanitize_label() {
    local input="$1"
    if [[ -z "${input}" ]]; then
        echo ""
        return
    fi
    printf '%s' "${input}" | tr -cs 'A-Za-z0-9._-' '-'
}

make_run_config() {
    local source_config="$1"
    local target_config="$2"
    local run_tag="$3"
    /usr/bin/python3 - "${source_config}" "${target_config}" "${run_tag}" "${ROOT_DIR}" "${WARMUP_SECONDS}" "${SUMMARY_INTERVAL_OVERRIDE}" <<'PY'
import json
import pathlib
import sys

source, target, run_tag, root_dir, warmup_seconds, summary_override = sys.argv[1:]
root = pathlib.Path(root_dir)
with open(source, "r", encoding="utf-8") as fh:
    data = json.load(fh)

data["warmup_seconds"] = int(warmup_seconds)
if summary_override:
    data["summary_interval_seconds"] = int(summary_override)

data["log_file"] = str(root / "logs" / f"arb-{run_tag}.csv")
data["near_miss_log_file"] = str(root / "logs" / f"near-miss-{run_tag}.csv")
data["replay_log_file"] = str(root / "logs" / f"replay-{run_tag}.csv")

with open(target, "w", encoding="utf-8") as fh:
    json.dump(data, fh, indent=4)
    fh.write("\n")
PY
}

run_label_safe="$(sanitize_label "${RUN_LABEL}")"
config_stem="$(basename "${CONFIG_PATH}" .json)"

for ((run_index = 1; run_index <= REPEATS; ++run_index)); do
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    run_tag="${config_stem}"
    if [[ -n "${run_label_safe}" ]]; then
        run_tag+="-${run_label_safe}"
    fi
    if (( REPEATS > 1 )); then
        run_tag+="-r${run_index}"
    fi
    run_tag+="-${stamp}"

    out_file="${LOG_DIR}/benchmark-${run_tag}.log"
    run_config="${LOG_DIR}/benchmark-${run_tag}.config.json"
    make_run_config "${CONFIG_PATH}" "${run_config}" "${run_tag}"

    echo "[bench] run ${run_index}/${REPEATS}"
    echo "[bench] run_config=${run_config}"
    echo "[bench] output=${out_file}"

    set +e
    if [[ "${LIVE_OUTPUT}" == "1" ]]; then
        timeout --signal=INT --kill-after=5s "${DURATION_SECONDS}" \
            "${BUILD_DIR}/arb_detector" --config "${run_config}" \
            2>&1 | tee "${out_file}"
        status=${PIPESTATUS[0]}
    else
        timeout --signal=INT --kill-after=5s "${DURATION_SECONDS}" \
            "${BUILD_DIR}/arb_detector" --config "${run_config}" \
            >"${out_file}" 2>&1
        status=$?
    fi
    set -e

    if [[ ${status} -ne 0 && ${status} -ne 124 ]]; then
        echo "[bench] detector exited with status ${status}" >&2
        exit "${status}"
    fi

    echo "[bench] done"
    echo "[bench] log saved to ${out_file}"
done

if [[ "${LIVE_OUTPUT}" != "1" ]]; then
    echo "[bench] use LIVE_OUTPUT=1 to mirror logs to stdout during the run"
fi
