#!/usr/bin/env bash
# test/wire_qemu_test.sh — QEMU integration test for wire GDB RSP stub
#
# Builds the mps2-an385 test firmware, starts QEMU with a PTY serial port,
# connects wire-host to bridge that PTY to TCP :3333, then drives GDB to
# verify that register state is visible through the wire stub.
#
# Prerequisites:
#   arm-none-eabi-gcc   in PATH
#   cmake               in PATH
#   qemu-system-arm     in PATH (version >= 7.0)
#   wire-host binary    at ${WIRE_HOST_BIN} or built by this script
#
# Usage:
#   bash test/wire_qemu_test.sh [--no-build]
#
# Exit codes:
#   0  all checks passed
#   1  test failure
#   2  prerequisite missing
#
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/qemu_test"
FIRMWARE="${BUILD_DIR}/wire_qemu_test.elf"
WIRE_HOST_BIN="${WIRE_HOST_BIN:-${REPO_ROOT}/build/wire-host}"
GDB="${ARM_GDB:-arm-none-eabi-gdb}"
GDB_PORT=3333
QEMU_PTY_LOG="${BUILD_DIR}/qemu_pty.txt"
PASS=0
FAIL=0

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GRN='\033[0;32m'
NC='\033[0m'

pass() { echo -e "${GRN}PASS${NC}: $1"; PASS=$(( PASS + 1 )); }
fail() { echo -e "${RED}FAIL${NC}: $1"; FAIL=$(( FAIL + 1 )); }

cleanup() {
    [[ -n "${WIRE_HOST_PID:-}" ]] && kill "${WIRE_HOST_PID}" 2>/dev/null || true
    [[ -n "${QEMU_PID:-}" ]]      && kill "${QEMU_PID}"      2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

# ── Prerequisite checks ──────────────────────────────────────────────────────
require() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: '$1' not found in PATH" >&2
        exit 2
    fi
}
require cmake
require arm-none-eabi-gcc
require qemu-system-arm

# ── Build step (skipped with --no-build) ────────────────────────────────────
if [[ "${1:-}" != "--no-build" ]]; then
    echo "==> building test firmware"
    mkdir -p "${BUILD_DIR}"
    cmake -S "${SCRIPT_DIR}/qemu" \
          -B "${BUILD_DIR}" \
          -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/qemu/toolchain-arm.cmake" \
          -DCMAKE_BUILD_TYPE=Debug \
          --no-warn-unused-cli \
          -G "Unix Makefiles" \
          >/dev/null
    cmake --build "${BUILD_DIR}" -- -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)" \
          >/dev/null
    echo "    firmware: ${FIRMWARE}"

    if [[ ! -f "${WIRE_HOST_BIN}" ]]; then
        echo "==> building wire-host"
        HOST_BUILD="${REPO_ROOT}/build/host_native"
        mkdir -p "${HOST_BUILD}"
        cc -std=c99 -Wall -Wextra -Werror -O2 \
            -o "${WIRE_HOST_BIN}" \
            "${REPO_ROOT}/host/wire_host.c" \
            "${REPO_ROOT}/host/wire_serial.c"
        echo "    wire-host: ${WIRE_HOST_BIN}"
    fi
fi

[[ -f "${FIRMWARE}" ]]       || { echo "error: firmware not found: ${FIRMWARE}"       >&2; exit 2; }
[[ -f "${WIRE_HOST_BIN}" ]] || { echo "error: wire-host not found: ${WIRE_HOST_BIN}" >&2; exit 2; }

# ── Start QEMU ───────────────────────────────────────────────────────────────
echo "==> starting QEMU"
qemu-system-arm \
    -M mps2-an385 \
    -kernel "${FIRMWARE}" \
    -serial "pty" \
    -nographic \
    -display none \
    2>"${QEMU_PTY_LOG}" &
QEMU_PID=$!

# QEMU prints the PTY path within ~1 second.
PTY_PATH=""
for _ in $(seq 1 20); do
    if grep -q "char device redirected to" "${QEMU_PTY_LOG}" 2>/dev/null; then
        PTY_PATH="$(grep "char device redirected to" "${QEMU_PTY_LOG}" | sed 's/.*redirected to //' | tr -d '[:space:]')"
        break
    fi
    sleep 0.2
done

if [[ -z "${PTY_PATH}" ]]; then
    fail "QEMU did not emit a PTY path within 4 seconds"
    echo "--- QEMU log ---"
    cat "${QEMU_PTY_LOG}" || true
    echo "--- end ---"
    exit 1
fi
pass "QEMU started, PTY at ${PTY_PATH}"

# ── Start wire-host ──────────────────────────────────────────────────────────
echo "==> starting wire-host"
"${WIRE_HOST_BIN}" --port "${PTY_PATH}" --baud 0 --tcp-port ${GDB_PORT} &
WIRE_HOST_PID=$!
sleep 0.5  # give wire-host time to open the PTY and bind TCP socket

# ── GDB session ──────────────────────────────────────────────────────────────
echo "==> connecting GDB"
GDB_OUTPUT="${BUILD_DIR}/gdb_output.txt"

"${GDB}" \
    --batch \
    --ex "set confirm off" \
    --ex "set pagination off" \
    --ex "target remote :${GDB_PORT}" \
    --ex "info registers" \
    --ex "x/4xw 0x20000000" \
    --ex "set \$r0 = 0xDEADBEEF" \
    --ex "info registers r0" \
    --ex "disconnect" \
    "${FIRMWARE}" \
    >"${GDB_OUTPUT}" 2>&1 || true

if [[ ! -f "${GDB_OUTPUT}" ]]; then
    fail "GDB produced no output"
    exit 1
fi

# ── Validate GDB output ──────────────────────────────────────────────────────

# Check that GDB connected and received register state (pc should be non-zero
# given the firmware got far enough to call wire_debug_loop).
if grep -qiE "^pc\s+0x[0-9a-f]+" "${GDB_OUTPUT}"; then
    pass "GDB received PC register from wire stub"
else
    fail "GDB did not receive PC register"
fi

# Check SP is in RAM range (0x20000000–0x20040000).
SP_LINE="$(grep -iE "^sp\s+" "${GDB_OUTPUT}" || true)"
if [[ -n "${SP_LINE}" ]]; then
    SP_HEX="$(echo "${SP_LINE}" | awk '{print $2}')"
    SP_DEC="$(( 16#${SP_HEX#0x} ))"
    RAM_START=$(( 16#20000000 ))
    RAM_END=$(( 16#20040000 ))
    if (( SP_DEC >= RAM_START && SP_DEC <= RAM_END )); then
        pass "SP (${SP_HEX}) is within RAM bounds"
    else
        fail "SP (${SP_HEX}) is outside expected RAM range"
    fi
else
    fail "GDB did not return SP register"
fi

# Check that the register write (r0 = 0xDEADBEEF) was accepted.
if grep -q "deadbeef\|0xdeadbeef\|DEADBEEF\|0xDEADBEEF" "${GDB_OUTPUT}"; then
    pass "GDB register write (r0 = 0xDEADBEEF) accepted by stub"
else
    fail "GDB register write not reflected in output"
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo ""
echo "========================================"
echo "wire QEMU integration test"
echo "  PASS: ${PASS}   FAIL: ${FAIL}"
echo "========================================"

if (( FAIL > 0 )); then
    echo ""
    echo "--- GDB output ---"
    cat "${GDB_OUTPUT}" || true
    echo "--- end ---"
    exit 1
fi
