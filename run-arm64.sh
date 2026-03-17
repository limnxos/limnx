#!/bin/bash
# Limnx ARM64 — clean build + QEMU boot
# Usage: ./run-arm64.sh [extra qemu args...]

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building ARM64 ==="
make arm64-clean
make arm64

echo "=== Creating disk ==="
dd if=/dev/zero of=build/disk.img bs=1M count=64 2>/dev/null

echo "=== Booting ARM64 ==="

# Save terminal state, set raw mode (no local echo), restore on exit
saved_stty=$(stty -g 2>/dev/null || true)
cleanup() { stty "$saved_stty" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

stty raw -echo 2>/dev/null || true

qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a57 \
    -m 256M \
    -smp 2 \
    -nographic \
    -kernel build/arm64/kernel \
    -drive file=build/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -no-reboot \
    "$@"
