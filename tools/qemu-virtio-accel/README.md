# virtio-accel QEMU Device

Custom QEMU virtio device (ID 48) that provides tensor compute acceleration
for the Limnx kernel. The device reads tensor data directly from guest RAM,
performs operations on the host, and writes results back.

## Build Instructions

1. Clone QEMU source:
```bash
git clone https://gitlab.com/qemu-project/qemu.git
cd qemu
git checkout v9.0.0  # or latest stable
```

2. Copy device files:
```bash
cp /path/to/limnx/tools/qemu-virtio-accel/virtio-accel.c hw/virtio/
cp /path/to/limnx/tools/qemu-virtio-accel/virtio-accel.h include/hw/virtio/
```

3. Add to `hw/virtio/meson.build`:
```meson
specific_ss.add(when: 'CONFIG_VIRTIO_ACCEL', if_true: files('virtio-accel.c'))
```

4. Add to `hw/virtio/Kconfig`:
```
config VIRTIO_ACCEL
    bool
    default y
    depends on VIRTIO
```

5. Build QEMU:
```bash
mkdir build && cd build
../configure --target-list=aarch64-softmmu,x86_64-softmmu
make -j$(nproc)
```

## Usage

### ARM64 (virtio-mmio, auto-discovered):
```bash
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 256M \
    -device virtio-accel-device \
    -kernel build/arm64/kernel \
    ...
```

### x86_64 (PCI):
```bash
qemu-system-x86_64 -M q35 -m 4G \
    -device virtio-accel-pci \
    ...
```

## Protocol

The device uses a single virtqueue (requestq). Each request is a 2-descriptor
chain: request (80 bytes, readable) + response (16 bytes, writable).

Tensor data is NOT in the virtqueue — the request contains guest physical
addresses. The device reads/writes guest RAM directly via `dma_memory_read/write`.

## Supported Operations

| Op | Code | Description |
|----|------|-------------|
| MATMUL | 1 | out = A @ B |
| SOFTMAX | 2 | in-place softmax |
| RMSNORM | 3 | RMS normalization |
| ROPE | 4 | Rotary position embedding |
| SILU | 5 | SiLU activation |
| ELEMUL | 6 | Element-wise multiply |
| ELEMADD | 7 | Element-wise add |
| PING | 255 | Health check |

## Extending with GPU Compute

Replace the `host_*` functions in `virtio-accel.c` with CUDA/OpenCL calls
for actual GPU acceleration. The device interface stays the same.
