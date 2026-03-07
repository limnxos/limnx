# Limnx

The Limnx kernel — an AI-native operating system designed to run AI agent workloads as first-class citizens.

## What is Limnx?

Limnx is an operating system where AI agents are the primary inhabitants, not an afterthought. Every layer — from the scheduler to IPC to security — is built with autonomous agent workflows in mind: sandboxed tool execution, capability-based isolation, kernel-routed inference, and structured inter-agent communication.

It boots on real hardware (BIOS and UEFI), runs a preemptive SMP kernel, and provides a syscall interface with 84 system calls.

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
- Capability tokens (8 capabilities: NET_BIND, KILL, EXEC, FS_READ, ...)
- Per-process seccomp filters, resource limits, UID/GID isolation
- Sandboxed tool dispatch (fork + capability drop + pipe)
- Kernel-routed inference service registry
- io_uring-style async I/O

**User Space**
- Minimal libc (printf, string, math, HTTP, tokenizer, GGUF parser)
- Interactive shell
- Tool-using AI agent with multi-tool chains
- Inference daemon (unix socket server)
- Incremental test suites

## Build

Requires: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `xorriso`, `qemu-system-x86_64`

```
make clean && make     # build limnx.iso
make disk              # create 64MB virtio-blk disk image
make run               # boot in QEMU with virtio-net + virtio-blk
```

## Architecture

```
 User Space (Ring 3)
 ┌──────────────────────────────────────────────────┐
 │  shell   toolagent    inferd    agent programs   │
 │                                                  │
 │  libc (syscalls, printf, math, HTTP, tokenizer)  │
 ├──────────────────────────────────────────────────┤
 │              SYSCALL / SYSRET                    │
 ├──────────────────────────────────────────────────┤
 Kernel (Ring 0)
 │                                                  │
 │  Process     Scheduler    Memory     Filesystem  │
 │  fork/exec   SMP/steal    COW/swap   LimnFS/VFS  │
 │                                                  │
 │  Networking  IPC          Security   Device      │
 │  TCP/IP/UDP  unix/epoll   caps/sec   virtio      │
 │              uring/evfd   seccomp    PCI/LAPIC   │
 └──────────────────────────────────────────────────┘
        Limine Bootloader (BIOS / UEFI)
```

## License

All rights reserved.
