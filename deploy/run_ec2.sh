#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
LOG_DIR="${ROOT_DIR}/logs"
CONFIG_PATH="${1:-${ROOT_DIR}/config.json}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
ALLOW_CONDA_BOOST="${PM_ALLOW_CONDA_BOOST:-ON}"
ALLOCATOR="${PM_ALLOCATOR:-system}"

if [[ ! -f "${CONFIG_PATH}" ]]; then
    echo "[run] config not found: ${CONFIG_PATH}" >&2
    exit 1
fi

mkdir -p "${LOG_DIR}"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "[run] building (${BUILD_TYPE})"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DPM_ALLOW_CONDA_BOOST="${ALLOW_CONDA_BOOST}" \
        -DPM_ALLOCATOR="${ALLOCATOR}"
    cmake --build "${BUILD_DIR}" -j"${JOBS}"
fi

if grep -Eq '"metrics_enabled"[[:space:]]*:[[:space:]]*true' "${CONFIG_PATH}"; then
    echo "[run] warning: metrics_enabled=true in ${CONFIG_PATH}" >&2
    echo "[run] warning: set metrics_enabled=false for lowest deploy latency" >&2
fi

if ulimit -l unlimited 2>/dev/null; then
    echo "[run] memlock raised to unlimited"
else
    echo "[run] memlock unchanged"
fi

echo "[run] binary=${BUILD_DIR}/arb_detector"
echo "[run] config=${CONFIG_PATH}"

exec "${BUILD_DIR}/arb_detector" --config "${CONFIG_PATH}"
