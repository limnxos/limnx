# Limnx — AI-Native Operating System

An x86_64 operating system built from scratch, designed for AI agent workloads.

## Build

**x86_64** requires: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `xorriso`, `qemu-system-x86_64`.
**ARM64** requires: `aarch64-elf-gcc` (or `aarch64-linux-gnu-gcc`), `qemu-system-aarch64`.

```
make clean && make     # build x86_64 ISO (fetches Limine on first run)
make run               # boot in QEMU with virtio-net + virtio-blk
make disk              # create 64MB virtio-blk disk image
make arm64             # build ARM64 kernel ELF
make arm64-run         # boot ARM64 kernel in QEMU (virt machine, PL011 serial)
make arm64-clean       # clean ARM64 build artifacts
```

## Directory Structure

```
kernel/
  linker.ld            Kernel linker script (loads at 0xFFFFFFFF80000000)
  deps/                Limine protocol headers
  src/
    main.c             Kernel entry point (kmain), all stage smoke tests
    arch/
      cpu.h            HAL dispatch — CPU ops, IRQ, FPU, TLS, memory barrier
      paging.h         HAL dispatch — address space switch, TLB flush, fault addr
      serial.h         HAL dispatch — serial I/O (serial_init, serial_putc, serial_printf)
      interrupt.h      HAL dispatch — IRQ registration, keyboard input
      timer.h          HAL dispatch — timer ticks, scheduler enable/disable
      smp_hal.h        HAL dispatch — SMP init, IPI, TLB shootdown
      percpu.h         HAL dispatch — per-CPU data struct (arch-specific)
      context.h        HAL dispatch — context_switch, thread_trampoline (asm)
      syscall_arch.h   HAL dispatch — arch_syscall_init
      boot.h           HAL dispatch — arch_early_init, arch_late_init
      pte.h            HAL dispatch — page table entry bit definitions
      x86_64/
        cpu.h          x86_64 inline: hlt, pause, cli/sti, fxsave, MSR, GS base
        paging.h       x86_64 inline: CR3, invlpg, CR2
        io.h           Port I/O macros (inb/outb/inw/outw/inl/outl)
        gdt.c/.h       Global Descriptor Table
        tss.c/.h       Task State Segment
        idt.c/.h       IDT + ISR dispatch + HAL timer/IRQ wrappers
        isr_stubs.asm  ISR assembly stubs (exceptions + IRQs)
        lapic.c/.h     Local APIC driver (timer, EOI, calibration)
        percpu.h       Per-CPU struct (GDT/TSS/lapic_id, GS-relative, asm offsets)
        serial.c       COM1 serial driver
        smp.c          SMP init, AP bootstrap, TLB shootdown, syscall MSR init
        switch.asm     Context switch + thread trampoline
        syscall_entry.asm  SYSCALL instruction entry (signal delivery check)
        boot.c         arch_early_init (GDT+IDT+FPU), arch_late_init (TSS)
        pte.h          x86_64 PTE bit definitions
      arm64/
        cpu.h          ARM64 inline: wfi, yield, DAIF, NEON, TPIDR
        paging.h       ARM64 inline: TTBR0, TLBI, FAR_EL1
        percpu.h       ARM64 per-CPU struct (TPIDR_EL1 accessor)
        pte.h          ARM64 VMSAv8-A descriptor bits
        boot.S         ARM64 entry point (QEMU virt)
        boot.c         ARM64 arch_early_init/arch_late_init stubs
        serial.c       PL011 UART driver
        interrupt.c    GIC interrupt stubs
        timer.c        Generic timer stubs
        smp.c          Single-core SMP stub
        main.c         ARM64 minimal kmain
        linker.ld      ARM64 kernel layout (0x40080000)
    mm/
      pmm.c/.h         Physical Memory Manager (bitmap allocator)
      vmm.c/.h         Virtual Memory Manager (4-level paging)
      kheap.c/.h       Kernel heap (first-fit, 16-byte aligned)
      swap.c/.h        Swap area + demand paging (last 8MB of disk, 2048 pages)
    sched/
      thread.c/.h      Thread creation/destruction
      sched.c/.h       FIFO scheduler with preemptive timer
    proc/
      process.c/.h     Process creation, registry, user-mode entry via SYSRETQ, signals, process groups, fork with COW
      elf.c/.h         ELF64 loader
    syscall/
      syscall.c/.h     Dispatch table + shared helpers (validate_user_ptr, copy_string_from_user)
      syscall_internal.h Handler prototypes + shared types
      sys_fs.c         File syscall handlers (open, read, write, stat, mkdir, etc.)
      sys_proc.c       Process syscall handlers (exit, exec, execve, fork, waitpid, kill)
      sys_mm.c         Memory syscall handlers (mmap, munmap, shm, mprotect)
      sys_fd.c         FD syscall handlers (dup, dup2, fcntl, pipe, ioctl, openpty)
      sys_net.c        Network syscall handlers (socket, TCP, UDP)
      sys_signal.c     Signal syscall handlers (sigaction, sigreturn, sigprocmask)
      sys_ipc.c        IPC syscall handlers (unix sockets, agent, eventfd, epoll, pubsub)
      sys_security.c   Security syscall handlers (uid/gid, caps, rlimits, seccomp, tokens, namespaces)
      sys_infer.c      Inference syscall handlers (register, request, health, route, async, swap)
      sys_misc.c       Misc syscall handlers (write, yield, getchar, time, env, poll, select, uring, taskgraph, supervisor)
    fs/
      vfs.c/.h         Virtual File System (tree hierarchy, directories, LimnFS integration)
      tar.c/.h         TAR archive parser (for initrd)
    pci/
      pci.c/.h         PCI bus enumeration
    net/
      virtio_net.c/.h  Virtio-net driver
      net.c/.h         Network stack (Ethernet/IP/UDP/ICMP/TCP dispatch, software loopback)
      tcp.c/.h         TCP stack (state machine, connect/listen/accept/send/recv/close)
      netstor.c/.h     Network key-value storage client
    blk/
      virtio_blk.c/.h  Virtio-blk driver
      limnfs.c/.h      LimnFS disk filesystem (ext2-inspired, hierarchical, triple indirect)
      bcache.c/.h      Block cache (256 entries × 4KB, DLL-LRU + hash table, write-back, kernel flusher thread)
    fb/
      fbcon.c/.h       Framebuffer text console (8x16 bitmap font, text buffer scroll, cursor)
    pty/
      pty.c/.h         Pseudo-terminal (master/slave, echo, canonical mode, console PTY)
      termios.h        Terminal I/O structures (termios_t, winsize_t, ioctl commands)
    sync/
      spinlock.h       Spinlock with IRQ save/restore
      mutex.c/.h       Sleeping mutex (spinlock + wait queue + THREAD_BLOCKED)
    ipc/
      unix_sock.c/.h   Unix domain sockets (16 slots, ring buffer IPC)
      eventfd.c/.h     Eventfd (counter-based notification, 16 slots)
      agent_reg.c/.h   Agent registry (name→pid, 16 entries, namespace-scoped, auto-cleanup)
      agent_ns.c/.h    Agent namespaces (8 namespaces, scoped agent isolation)
      cap_token.c/.h   Capability tokens (32 tokens, fine-grained delegated authorization)
      epoll.c/.h       epoll I/O multiplexing (8 instances, 64 fds each)
      infer_svc.c/.h   Inference service registry (16 entries, namespace-aware routing, model hot-swap, async, cache, queue)
      uring.c/.h       io_uring-style async I/O (4 instances, 32 entries)
      taskgraph.c/.h   Workflow task graph (DAG, 32 tasks, dependency tracking)
      supervisor.c/.h  Agent supervision trees (8 supervisors, restart policies)
      pubsub.c/.h      Pub/sub messaging (16 topics, 8 subs/topic, 4 queue slots)
      pipe.c/.h        Pipe infrastructure (8 pipes, ring buffer, spinlock-protected)
      shm.c/.h         Shared memory infrastructure (16 regions, spinlock-protected)

user/
  syscall.inc          Syscall numbers for NASM programs
  linker.ld            Linker script for ASM user programs
  asm/                 Assembly programs
    hello.asm          Hello world (SYS_WRITE + SYS_EXIT)
    cat.asm            File cat (SYS_OPEN + SYS_READ + SYS_WRITE)
    udpecho.asm        UDP echo server
    writetest.asm      File create/write/read test
  libc/                Minimal C runtime for user-space C programs
    libc.h             Unified header (types, syscalls, string, stdio, math)
    start.asm          _start entry → main() → SYS_EXIT
    syscalls.asm       C-callable syscall wrappers
    string.c           memcpy, memset, strlen, strcmp, strncmp, strcpy, strstr
    stdio.c            puts, printf (via SYS_WRITE)
    math.c             fabsf, sqrtf, expf, logf, tanhf, sinf, cosf, sigmoidf
    tokenizer.c        Character-level tokenizer + BPE tokenizer
    gguf.c             GGUF v3 model file parser and loader
    dequant.c          GGML dequantization (Q4_0, Q8_0, Q2_K–Q6_K, F16 → F32)
    http.c             HTTP/1.0 parser, formatter, server loop
    tooldispatch.c     Sandboxed tool execution via fork+caps+pipe
    linker.ld          Linker script for C user programs
  programs/            Programs, agents, daemons
    shell.c            Interactive system shell
    agent.c            AI agent
    agentrt.c          Agent runtime
    toolagent.c        Tool-using AI agent with multi-tool chains
    chat.c             Chat interface
    generate.c         Text generation
    learn.c            Learning program
    infer.c            Network inference server (UDP port 9000)
    inferd.c           Inference daemon (unix socket server)
    worker.c           Pipe-based worker for multi-agent collaboration
    multiagent.c       Multi-agent coordinator (pipes + exec worker)
    netagent.c         Network agent (epoll-driven TCP)
    crasher.c          Minimal exit-with-1 program (for supervisor restart test)
  tests/               Stage and feature test programs
    mathtest.c         Float math + mmap test
    pipetest.c         IPC test (getpid, pipe, exec+waitpid, fmmap)
    fstest.c           Filesystem test (directories, path resolution)
    fstest2.c          Filesystem completion test (seek, truncate, cwd, rename)
    ostest.c           OS maturity test (dup, dup2, kill, permissions)
    memtest.c          Persistent vecstore save/load test
    ragtest.c          RAG end-to-end test (top-K retrieval)
    lmstest.c          Large model support test (mmap, dynamic transformer)
    gguftest.c         Modern transformer test (RoPE, SwiGLU, BPE)
    gguf2test.c        Full GGUF test (dequantization, GQA, QK-norm)
    agenttest.c        Agent test
    agenttest2.c       Agent intelligence test (chain planning, RAG loss)
    infertest.c        Inference test
    s25test.c–s78test.c  Stage regression tests (49 files)

initrd/                Files bundled into initrd.tar (includes test.gguf, test_gqa.gguf)
docs/                  Architecture and stage documentation
tools/                 Host-side utilities (netstor_server.py, gen_gguf.py)
```

## Architecture

- **Bootloader**: Limine (v8.x, BIOS + UEFI)
- **Kernel**: Freestanding C, compiled with `-mno-sse -mcmodel=kernel`
- **Architectures**: x86_64 (primary), ARM64 (HAL layer + early boot stub)
- **User-space**: Ring 3, separate address spaces, SYSCALL/SYSRET
- **User C programs**: Compiled with `-msse2` (FPU/SSE enabled per-process)
- **SMP**: 2 CPUs via Limine SMP, per-CPU data via SWAPGS/GS.base, LAPIC timer preemption
- **Scheduling**: Preemptive FIFO, shared ready queue with spinlock, LAPIC timer (vector 48)
- **Memory**: 4-level paging, HHDM for physical access, kernel heap up to 1GB, mmap up to 2GB/call

## Syscalls

| # | Name | Args |
|---|------|------|
| 0 | SYS_WRITE | buf, len |
| 1 | SYS_YIELD | - |
| 2 | SYS_EXIT | status |
| 3 | SYS_OPEN | path, flags |
| 4 | SYS_READ | fd, buf, len |
| 5 | SYS_CLOSE | fd |
| 6 | SYS_STAT | path, stat_ptr |
| 7 | SYS_EXEC | path |
| 8 | SYS_SOCKET | - |
| 9 | SYS_BIND | sockfd, port |
| 10 | SYS_SENDTO | sockfd, buf, len, dst_ip, dst_port |
| 11 | SYS_RECVFROM | sockfd, buf, len, src_ip_ptr, src_port_ptr |
| 12 | SYS_FWRITE | fd, buf, len |
| 13 | SYS_CREATE | path |
| 14 | SYS_UNLINK | path |
| 15 | SYS_MMAP | num_pages |
| 16 | SYS_MUNMAP | virt_addr |
| 17 | SYS_GETCHAR | - |
| 18 | SYS_WAITPID | pid |
| 19 | SYS_PIPE | rfd_ptr, wfd_ptr |
| 20 | SYS_GETPID | - |
| 21 | SYS_FMMAP | fd |
| 22 | SYS_READDIR | dir_path, index, dirent_ptr |
| 23 | SYS_MKDIR | path |
| 24 | SYS_SEEK | fd, offset, whence |
| 25 | SYS_TRUNCATE | path, new_size |
| 26 | SYS_CHDIR | path |
| 27 | SYS_GETCWD | buf, size |
| 28 | SYS_FSTAT | fd, stat_ptr |
| 29 | SYS_RENAME | old_path, new_path |
| 30 | SYS_DUP | fd |
| 31 | SYS_DUP2 | oldfd, newfd |
| 32 | SYS_KILL | pid, signal |
| 33 | SYS_FCNTL | fd, cmd, arg |
| 34 | SYS_SETPGID | pid, pgid |
| 35 | SYS_GETPGID | pid |
| 36 | SYS_CHMOD | path, mode |
| 37 | SYS_SHMGET | key, num_pages |
| 38 | SYS_SHMAT | shmid |
| 39 | SYS_SHMDT | virt_addr |
| 40 | SYS_FORK | - |
| 41 | SYS_SIGACTION | signum, handler_ptr |
| 42 | SYS_SIGRETURN | - |
| 43 | SYS_OPENPTY | master_fd_ptr, slave_fd_ptr |
| 44 | SYS_TCP_SOCKET | - |
| 45 | SYS_TCP_CONNECT | conn_idx, ip, port |
| 46 | SYS_TCP_LISTEN | conn_idx, port |
| 47 | SYS_TCP_ACCEPT | listen_conn_idx |
| 48 | SYS_TCP_SEND | conn_idx, buf, len |
| 49 | SYS_TCP_RECV | conn_idx, buf, len |
| 50 | SYS_TCP_CLOSE | conn_idx |
| 51 | SYS_IOCTL | fd, cmd, arg |
| 52 | SYS_CLOCK_GETTIME | clockid, timespec_ptr |
| 53 | SYS_NANOSLEEP | timespec_ptr |
| 54 | SYS_GETENV | key_ptr, val_buf_ptr, val_buf_size |
| 55 | SYS_SETENV | key_ptr, value_ptr |
| 56 | SYS_POLL | pollfd_ptr, nfds, timeout_ms |
| 57 | SYS_GETUID | - |
| 58 | SYS_SETUID | uid |
| 59 | SYS_GETGID | - |
| 60 | SYS_SETGID | gid |
| 61 | SYS_GETCAP | - |
| 62 | SYS_SETCAP | pid, caps |
| 63 | SYS_GETRLIMIT | resource, rlimit_ptr |
| 64 | SYS_SETRLIMIT | resource, rlimit_ptr |
| 65 | SYS_SECCOMP | mask, strict |
| 66 | SYS_SETAUDIT | pid, flags |
| 67 | SYS_UNIX_SOCKET | — |
| 68 | SYS_UNIX_BIND | fd, path |
| 69 | SYS_UNIX_LISTEN | fd |
| 70 | SYS_UNIX_ACCEPT | fd |
| 71 | SYS_UNIX_CONNECT | path |
| 72 | SYS_AGENT_REGISTER | name |
| 73 | SYS_AGENT_LOOKUP | name, pid_out |
| 74 | SYS_EVENTFD | flags |
| 75 | SYS_EPOLL_CREATE | flags |
| 76 | SYS_EPOLL_CTL | epfd, op, fd, event_ptr |
| 77 | SYS_EPOLL_WAIT | epfd, events_ptr, max_events, timeout_ms |
| 78 | SYS_SWAP_STAT | stat_ptr |
| 79 | SYS_INFER_REGISTER | name, sock_path |
| 80 | SYS_INFER_REQUEST | name, req_buf, req_len, resp_buf, resp_len |
| 81 | SYS_URING_SETUP | entries, params_ptr |
| 82 | SYS_URING_ENTER | uring_fd, sqe_ptr, count, cqe_ptr |
| 83 | SYS_MMAP2 | num_pages, flags |
| 84 | SYS_TOKEN_CREATE | perms, target_pid, resource_path |
| 85 | SYS_TOKEN_REVOKE | token_id |
| 86 | SYS_TOKEN_LIST | buf_ptr, max_count |
| 87 | SYS_NS_CREATE | name_ptr |
| 88 | SYS_NS_JOIN | ns_id |
| 89 | SYS_PROCINFO | index, proc_info_ptr |
| 90 | SYS_FSSTAT | fs_stat_ptr |
| 91 | SYS_TASK_CREATE | name_ptr, ns_id |
| 92 | SYS_TASK_DEPEND | task_id, dep_id |
| 93 | SYS_TASK_START | task_id |
| 94 | SYS_TASK_COMPLETE | task_id, result |
| 95 | SYS_TASK_STATUS | task_id, status_ptr |
| 96 | SYS_TASK_WAIT | task_id |
| 97 | SYS_TOKEN_DELEGATE | parent_id, target_pid, perms, resource_path |
| 98 | SYS_NS_SETQUOTA | ns_id, resource, limit |
| 109 | SYS_ARCH_PRCTL | code, addr |
| 110 | SYS_SELECT | nfds, readfds, writefds, timeout_us |
| 111 | SYS_SUPER_CREATE | name |
| 112 | SYS_SUPER_ADD | super_id, elf_path, ns_id, caps |
| 113 | SYS_SUPER_SET_POLICY | super_id, policy |
| 114 | SYS_PIPE2 | rfd_ptr, wfd_ptr, flags |
| 115 | SYS_SUPER_START | super_id |
| 116 | SYS_TCP_SETOPT | conn_idx, opt, value |
| 117 | SYS_TCP_TO_FD | conn_idx |
| 118 | SYS_INFER_SET_POLICY | policy |
| 119 | SYS_INFER_QUEUE_STAT | stat_ptr |
| 120 | SYS_INFER_CACHE_CTRL | cmd, arg_ptr |
| 121 | SYS_INFER_SUBMIT | name, req_buf, req_len, eventfd_idx |
| 122 | SYS_INFER_POLL | request_id |
| 123 | SYS_INFER_RESULT | request_id, resp_buf, resp_len |
| 124 | SYS_EXECVE | path, argv |
| 125 | SYS_TOPIC_CREATE | name_ptr, ns_id |
| 126 | SYS_TOPIC_SUB | topic_id |
| 127 | SYS_TOPIC_PUB | topic_id, buf_ptr, len |
| 128 | SYS_TOPIC_RECV | topic_id, buf_ptr, max_len, pub_pid_ptr |
| 129 | SYS_INFER_SWAP | name_ptr, new_sock_path_ptr |
| 130 | SYS_ENVIRON | buf_ptr, buf_size |

## Development Workflow

For every implementation phase/stage, always create two documentation files in `docs/`:

- **Plan file** (`docs/STAGE<N>_PLAN.md`) — Written before implementation. Contains context, design decisions, implementation order, file changes, risks.
- **Result file** (`docs/STAGE<N>_RESULT.md`) — Written after implementation. Contains what was done, what changed, test results, any deviations from plan.

## Key Conventions

See `CONVENTIONS.md` for coding standards.
