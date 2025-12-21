#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUT="${WIRE_HOST_OUTPUT:-${BUILD_DIR}/wire-host}"

mkdir -p "${BUILD_DIR}"

cc -std=c99 -Wall -Wextra -Werror -O2 \
  -o "${OUT}" \
  "${ROOT_DIR}/host/wire_host.c" \
  "${ROOT_DIR}/host/wire_serial.c"

echo "built ${OUT}"
