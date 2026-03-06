#!/bin/bash
# Limnx — Build and run in QEMU (terminal-only)

set -e

# Build
make clean && make

# Create disk image if missing
[ -f build/disk.img ] || make disk

# Run
echo ""
echo "=== Starting Limnx (Ctrl-A X to quit QEMU) ==="
echo ""

qemu-system-x86_64 \
    -M q35 \
    -cdrom limnx.iso \
    -m 2G \
    -serial stdio \
    -display none \
    -no-reboot \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=udp::5555-10.0.2.15:1234 \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0
