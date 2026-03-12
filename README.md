# Limnx

The Limnx kernel — the first AI-native operating system designed to run AI agent and inference workloads as first-class citizens.

## What is Limnx?

Limnx is an operating system where AI agents are the primary inhabitants, not an afterthought. Every layer — from the scheduler to IPC to security — is built with autonomous agent workflows in mind: sandboxed tool execution, capability-based isolation, kernel-routed inference, and structured inter-agent communication.

It boots on real hardware (BIOS and UEFI), runs a preemptive SMP kernel, and provides a syscall interface with over 130 system calls. The kernel targets x86_64 as the primary architecture with an ARM64 HAL layer and early boot support.

## Features

**Kernel**
- 4-level paging, kernel heap (1GB), demand paging, swap (2048 pages)
- Preemptive SMP scheduler (2 CPUs, per-CPU run queues, work stealing)
- Fork with copy-on-write, ELF64 loader, signals (sigaction/sigreturn)
- LimnFS disk filesystem (ext2-inspired, triple indirect blocks, 64MB)
- Block cache (256 entries, LRU, write-back)
- TCP/IP stack (full state machine, software loopback, dynamic receive window)
- Virtio-net and virtio-blk drivers
- PTY subsystem, framebuffer console

**Agent Infrastructure**
- Unix domain sockets, eventfd, epoll for multiplexed I/O
- Agent registry (name-based discovery, auto-cleanup)
- Capability tokens (12 capabilities: NET_BIND, KILL, EXEC, FS_READ, XNS_INFER, ...)
- Agent namespaces with quota enforcement
- Per-process seccomp filters, resource limits, UID/GID isolation
- Sandboxed tool dispatch (fork + capability drop + pipe)
- Kernel-routed inference service registry with health monitoring, model hot-swap, namespace-aware routing
- Pub/sub messaging (topic-based multi-agent communication)
- Task graph DAGs (dependency tracking, cross-namespace support)
- Supervisor trees (auto-restart, ONE_FOR_ONE / ONE_FOR_ALL policies)
- io_uring-style async I/O

**User Space**
- Minimal libc (printf, string, math, HTTP, tokenizer, GGUF parser)
- Interactive shell
- Tool-using AI agent with multi-tool chains
- Inference daemon (unix socket server)
- Incremental test suites

## Build

**x86_64** requires: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `xorriso`, `qemu-system-x86_64`
**ARM64** requires: `aarch64-elf-gcc` (or `aarch64-linux-gnu-gcc`), `qemu-system-aarch64`

```
make clean && make     # build x86_64 limnx.iso
make disk              # create 64MB virtio-blk disk image
make run               # boot in QEMU with virtio-net + virtio-blk
make arm64             # build ARM64 kernel ELF
make arm64-run         # boot ARM64 in QEMU (virt machine, PL011 serial)
```

## Architecture

```
 User Space (Ring 3 / EL0)
 ┌──────────────────────────────────────────────────┐
 │  shell   toolagent    inferd    agent programs   │
 │                                                  │
 │  libc (syscalls, printf, math, HTTP, tokenizer)  │
 ├──────────────────────────────────────────────────┤
 │              SYSCALL / SYSRET (x86_64)           │
 │              SVC (ARM64)                         │
 ├──────────────────────────────────────────────────┤
 Kernel (Ring 0 / EL1)
 │                                                  │
 │  Process     Scheduler    Memory     Filesystem  │
 │  fork/exec   SMP/steal    COW/swap   LimnFS/VFS  │
 │                                                  │
 │  Networking  IPC          Security   Device      │
 │  TCP/IP/UDP  unix/epoll   caps/sec   virtio      │
 │              uring/evfd   seccomp    PCI/LAPIC   │
 │              pubsub/tg    tokens/ns              │
 │                                                  │
 │  HAL (arch/)                                     │
 │  x86_64: CR3, LAPIC, MSR    ARM64: TTBR, GIC     │
 └──────────────────────────────────────────────────┘
        Limine (x86_64 BIOS/UEFI) | Direct boot (ARM64)
```

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
