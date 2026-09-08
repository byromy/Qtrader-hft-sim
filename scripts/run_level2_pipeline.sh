#!/usr/bin/env bash
set -euo pipefail
QTRADER_PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
QTRADER_PIPELINE_BUILD=${BUILD_DIR:-${QTRADER_PROJECT_DIR}/build}
QTRADER_PIPELINE_INPUT=${1:--}
QTRADER_PIPELINE_EVENTS=${EVENTS:-10000}
QTRADER_PIPELINE_MODE=${MODE:-paced}
for kind in reference dense; do
  "${QTRADER_PIPELINE_BUILD}/qtrader_pipeline_${kind}" \
    "${QTRADER_PIPELINE_INPUT}" "${QTRADER_PIPELINE_EVENTS}" "${QTRADER_PIPELINE_MODE}"
done
