#!/bin/bash
# Limnx automated test runner — runs all test suites on both architectures
set -e

TESTS="fs_test proc_test mm_test ipc_test security_test libc_test sched_test system_test net_test infer_test"
X86_DELAY=30
ARM64_DELAY=35
X86_TIMEOUT=60
ARM64_TIMEOUT=70
PASS=0
FAIL=0
TOTAL=0

echo "============================================"
echo "  Limnx Test Runner"
echo "============================================"
echo ""

# Build
echo "[build] x86_64..."
make clean >/dev/null 2>&1
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) >/dev/null 2>&1
echo "[build] x86_64 OK"

echo "[build] ARM64..."
make arm64 -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) >/dev/null 2>&1
echo "[build] ARM64 OK"
echo ""

run_test() {
    local ARCH=$1
    local TEST=$2
    local DELAY=$3
    local TIMEOUT=$4
    local LOGFILE="/tmp/limnx_test_${ARCH}_${TEST}.log"

    dd if=/dev/zero of=build/disk.img bs=1M count=64 2>/dev/null

    if [ "$ARCH" = "x86_64" ]; then
        (sleep $DELAY; printf "/${TEST}.elf\n"; sleep 10; printf "exit\n") | \
            timeout $TIMEOUT qemu-system-x86_64 -M q35 -m 4G -smp 2 -nographic -no-reboot \
            -cdrom limnx.iso \
            -drive file=build/disk.img,format=raw,if=virtio \
            -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
            >"$LOGFILE" 2>&1 || true
    else
        (sleep $DELAY; printf "/${TEST}.elf\n"; sleep 12; printf "exit\n") | \
            timeout $TIMEOUT qemu-system-aarch64 -M virt -cpu cortex-a57 -m 256M -smp 2 -nographic \
            -kernel build/arm64/kernel \
            -drive file=build/disk.img,format=raw,if=none,id=hd0 \
            -device virtio-blk-device,drive=hd0 \
            -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
            >"$LOGFILE" 2>&1 || true
    fi

    # Parse results
    local RESULT=$(strings "$LOGFILE" | grep "Results:" | tail -1)
    local P=$(echo "$RESULT" | grep -o '[0-9]* passed' | grep -o '[0-9]*')
    local F=$(echo "$RESULT" | grep -o '[0-9]* failed' | grep -o '[0-9]*')

    if [ -z "$P" ]; then
        # No results found — check for PANIC
        if strings "$LOGFILE" | grep -q "PANIC\|HALTING"; then
            printf "  %-12s %-8s PANIC\n" "$TEST" "$ARCH"
            FAIL=$((FAIL + 1))
        else
            printf "  %-12s %-8s NO OUTPUT (timing?)\n" "$TEST" "$ARCH"
            FAIL=$((FAIL + 1))
        fi
    elif [ "$F" = "0" ]; then
        printf "  %-12s %-8s %s/%s PASS\n" "$TEST" "$ARCH" "$P" "$P"
        PASS=$((PASS + 1))
    else
        printf "  %-12s %-8s %s passed, %s FAILED\n" "$TEST" "$ARCH" "$P" "$F"
        FAIL=$((FAIL + 1))
    fi
    TOTAL=$((TOTAL + 1))
}

# x86_64 tests
echo "--- x86_64 ---"
for T in $TESTS; do
    run_test x86_64 "$T" $X86_DELAY $X86_TIMEOUT
done

# x86_64 arch-specific test
run_test x86_64 "x86_test" $X86_DELAY $X86_TIMEOUT

echo ""
echo "--- ARM64 ---"
for T in $TESTS; do
    run_test arm64 "$T" $ARM64_DELAY $ARM64_TIMEOUT
done

# ARM64 arch-specific test
run_test arm64 "arm64_test" $ARM64_DELAY $ARM64_TIMEOUT

echo ""
echo "============================================"
if [ $FAIL -eq 0 ]; then
    echo "  ALL $TOTAL SUITES PASSED"
    echo "============================================"
    exit 0
else
    echo "  $PASS PASSED, $FAIL FAILED (of $TOTAL)"
    echo "============================================"
    exit 1
fi
