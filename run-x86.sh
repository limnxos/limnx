#!/bin/bash
# Limnx x86_64 — clean build + QEMU boot
# Usage: ./run-x86.sh [extra qemu args...]
#
# Environment variables:
#   MODEL=/path/to/model.gguf  — Create LimnFS disk with model pre-loaded
#   RAM=4G                     — QEMU memory (default 4G, use 6G+ for large models)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building x86_64 ==="
make clean
make

# Disk setup: if MODEL is set, create LimnFS with model; otherwise blank disk
if [ -n "$MODEL" ]; then
    echo "=== Creating model disk: $MODEL ==="
    RAM=${RAM:-6G}
    python3 tools/mklimnfs.py -o build/disk.img "$MODEL:model.gguf"
else
    make disk
    RAM=${RAM:-4G}
fi

echo "=== Booting x86_64 (RAM=$RAM) ==="

# Save terminal state, set raw mode (no local echo), restore on exit
saved_stty=$(stty -g 2>/dev/null || true)
cleanup() { stty "$saved_stty" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

stty raw -echo 2>/dev/null || true

qemu-system-x86_64 \
    -M q35 \
    -cdrom limnx.iso \
    -m "$RAM" \
    -smp 2 \
    -serial stdio \
    -display none \
    -no-reboot \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=udp::5555-10.0.2.15:1234,hostfwd=tcp::8080-10.0.2.15:80 \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0 \
    "$@"
