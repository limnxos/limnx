#!/bin/bash
# Limnx ARM64 — clean build + QEMU boot
# Usage: ./run-arm64.sh [extra qemu args...]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building ARM64 ==="
make arm64-clean
make arm64

echo "=== Booting ARM64 ==="
exec qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a57 \
    -m 256M \
    -nographic \
    -kernel build/arm64/kernel \
    -no-reboot \
    "$@"
