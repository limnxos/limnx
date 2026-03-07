# Limnx — AI-Native Operating System

An x86_64 operating system built from scratch, designed for AI agent workloads.

## Build

Requires: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `xorriso`, `qemu-system-x86_64`.

```
make clean && make     # build ISO (fetches Limine on first run)
make run               # boot in QEMU with virtio-net + virtio-blk
make disk              # create 32MB virtio-blk disk image
```

## Directory Structure

```
kernel/
  linker.ld            Kernel linker script (loads at 0xFFFFFFFF80000000)
  deps/                Limine protocol headers
  src/
    main.c             Kernel entry point (kmain), all stage smoke tests
    serial.c/.h        Serial port I/O (COM1, used for all output)
    io.h               Port I/O macros (inb/outb/inw/outw/inl/outl)
    gdt/               Global Descriptor Table
    idt/               Interrupt Descriptor Table + ISR stubs + keyboard input
    mm/
      pmm.c/.h         Physical Memory Manager (bitmap allocator)
      vmm.c/.h         Virtual Memory Manager (4-level paging)
      kheap.c/.h       Kernel heap (first-fit, 16-byte aligned)
      swap.c/.h        Swap area + demand paging (last 8MB of disk, 2048 pages)
    sched/
      tss.c/.h         Task State Segment
      thread.c/.h      Thread creation/destruction
      switch.asm        Context switch + thread trampoline
      sched.c/.h       FIFO scheduler with preemptive timer
    proc/
      process.c/.h     Process creation, registry, user-mode entry via SYSRETQ, signals, process groups, fork with COW
      elf.c/.h         ELF64 loader
    syscall/
      syscall.c/.h     Syscall handlers (dispatch table, shared memory, fork, sigaction/sigreturn, page fault)
      syscall_entry.asm SYSCALL instruction entry point (signal delivery check)
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
    smp/
      percpu.h         Per-CPU data structure (MAX_CPUS=8, GS-relative access)
      smp.c            SMP init, AP bootstrap, per-CPU GDT/TSS
      lapic.c/.h       Local APIC driver (timer, EOI, calibration)
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
      infer_svc.c/.h   Inference service registry (4 entries, kernel-routed)
      uring.c/.h       io_uring-style async I/O (4 instances, 32 entries)

user/
  syscall.inc          Syscall numbers for NASM programs
  linker.ld            Linker script for ASM user programs
  hello.asm            Hello world (SYS_WRITE + SYS_EXIT)
  cat.asm              File cat (SYS_OPEN + SYS_READ + SYS_WRITE)
  udpecho.asm          UDP echo server
  writetest.asm        File create/write/read test
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
  mathtest.c           Float math + mmap test program
  pipetest.c           IPC test (getpid, pipe, exec+waitpid, fmmap)
  shell.c              Interactive system shell
  toolagent.c          Tool-using AI agent with multi-tool chains (ls/cat/write/stat/exec/send)
  memtest.c            Persistent vecstore save/load test
  ragtest.c            RAG end-to-end test (top-K retrieval + context injection)
  fstest.c             Filesystem test (directories, path resolution, mount)
  fstest2.c            Filesystem completion test (open flags, seek, truncate, cwd, fstat, rename)
  lmstest.c            Large model support test (mmap limits, dynamic transformer)
  gguftest.c           Modern transformer test (RoPE, SwiGLU, BPE, GGUF loading)
  gguf2test.c          Full GGUF test (dequantization, GQA, QK-norm, RoPE theta)
  agenttest2.c         Agent intelligence test (chain planning, execution, RAG loss)
  ostest.c             OS maturity test (dup, dup2, kill, permissions, disk persistence)
  infer.c              Network inference server (UDP port 9000, transformer model)
  worker.c             Pipe-based worker for multi-agent collaboration
  multiagent.c         Multi-agent coordinator (pipes + exec worker)
  s25test.c            Stage 25 tests (fd inheritance, multi-agent pipe comm, inference)
  s26test.c            Stage 26 tests (LimnFS disk filesystem, large files, directories)
  s27test.c            Stage 27 tests (large file I/O, double indirect blocks)
  s28test.c            Stage 28 tests (argc/argv, fcntl, FD_CLOEXEC, O_NONBLOCK)
  s29test.c            Stage 29 tests (fbcon scroll, bcache LRU, triple indirect, cloexec/nonblock cleanup)
  s30test.c            Stage 30 tests (bcache DLL, chmod, signals, process groups, shared memory, persistence)
  s31test.c            Stage 31 tests (fork, COW, sigaction, sigreturn, page fault)
  s32test.c            Stage 32 tests (PTY, TCP networking)
  s33test.c            Stage 33 tests (loopback, ioctl, console PTY)
  s34test.c            Stage 34 tests (time, env, control chars, poll, WNOHANG)
  s35test.c            Stage 35 tests (uid/gid, capabilities, rlimits, seccomp, audit, file perms)
  s36test.c            Stage 36 tests (unix sockets, agent registry, eventfd, HTTP, tool dispatch)
  s37test.c            Stage 37 tests (epoll, demand paging, swap, inference service, io_uring)
  s38test.c            Stage 38 tests (process cleanup, bcache write-back, TCP window, per-CPU scheduler)
  s39test.c            Stage 39 tests (exec fix, bcache flusher, TCP cleanup)
  s41test.c            Stage 41 tests (capability tokens, agent namespaces)
  inferd.c             Inference daemon (unix socket server, registers with kernel)

initrd/                Files bundled into initrd.tar (includes test.gguf, test_gqa.gguf)
docs/                  Architecture and stage documentation
tools/                 Host-side utilities (netstor_server.py, gen_gguf.py)
```

## Architecture

- **Bootloader**: Limine (v8.x, BIOS + UEFI)
- **Kernel**: Freestanding C, compiled with `-mno-sse -mcmodel=kernel`
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

## Development Workflow

For every implementation phase/stage, always create two documentation files in `docs/`:

- **Plan file** (`docs/STAGE<N>_PLAN.md`) — Written before implementation. Contains context, design decisions, implementation order, file changes, risks.
- **Result file** (`docs/STAGE<N>_RESULT.md`) — Written after implementation. Contains what was done, what changed, test results, any deviations from plan.

## Key Conventions

See `CONVENTIONS.md` for coding standards.
