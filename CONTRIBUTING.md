# Contributing to Limnx

Thank you for your interest in contributing to Limnx — the AI-native operating system.

## Getting Started

### Prerequisites

**x86_64 build:**
- `x86_64-elf-gcc` and `x86_64-elf-ld` (cross-compiler)
- `nasm` (assembler)
- `xorriso` (ISO creation)
- `qemu-system-x86_64` (testing)

**ARM64 build:**
- `aarch64-elf-gcc` or `aarch64-linux-gnu-gcc`
- `qemu-system-aarch64` (testing)

### Build and Test

```bash
# x86_64
make clean && make    # build ISO
make disk             # create 64MB disk image
make run              # boot in QEMU

# ARM64
make arm64            # build kernel
./run-arm64.sh        # build + boot in QEMU

# Run tests (in the Limnx shell)
/infer_test.elf       # 49 inference pipeline tests
/security_test.elf    # security/seccomp tests
/orchestrator.elf     # full AI-native demo
/tool_demo.elf        # tool calling demo
```

### Project Structure

```
kernel/src/
  arch/x86_64/     x86_64 arch code (GDT, IDT, LAPIC, SMP, context switch)
  arch/arm64/      ARM64 arch code (GIC, timer, PSCI, exception vectors)
  mm/              Memory management (PMM, VMM, kheap, swap)
  sched/           Scheduler (FIFO preemptive, SMP)
  proc/            Process management (fork, exec, signals, ELF loader)
  syscall/         Syscall handlers (split by subsystem: fs, proc, mm, net, ipc, security, infer)
  fs/              VFS + TAR parser
  blk/             Block devices (virtio-blk, LimnFS, bcache)
  net/             Network stack (TCP/IP/UDP/ICMP/ARP, virtio-net)
  ipc/             IPC subsystems:
    infer_svc.c    Inference service registry (routing, caching, async, hot-swap)
    supervisor.c   Erlang-style supervisor trees
    taskgraph.c    DAG workflow engine
    pubsub.c       Pub/sub messaging
    agent_reg.c    Agent name registry
    agent_ns.c     Agent namespaces
    cap_token.c    Capability token system
    unix_sock.c    Unix domain sockets
    epoll.c        I/O multiplexing
    eventfd.c      Event notification
    pipe.c         Pipes
    shm.c          Shared memory
    uring.c        io_uring-style async I/O
  pty/             Pseudo-terminal
  fb/              Framebuffer console
  sync/            Spinlocks, mutexes, futexes

user/
  libc/            Custom libc (syscalls, printf, string, math, malloc, transformer, GGUF, tokenizer)
  programs/        User programs (shell, inferd, orchestrator, agents, tools, coreutils)
  tests/           Test suites organized by subsystem
  arch/            Architecture-specific user entry points and linker scripts

include/limnx/    Shared kernel-user headers (syscall numbers, stat struct)
initrd/           Files bundled into the boot initrd
tools/            Host-side utilities (gen_gguf.py, train_gguf.py)
```

## How to Contribute

### Good First Issues

These are self-contained tasks that don't require deep kernel knowledge:

**User-space:**
- Add a new coreutil (e.g. `df`, `du`, `diff`, `patch`)
- Improve the tool_demo with new tools (e.g. `web_fetcher.elf` using TCP)
- Add command-line flags to existing programs
- Write more test cases for existing test suites

**Kernel (beginner):**
- Fix compiler warnings (grep for `-Wunused` in CI output)
- Add missing error messages to syscall handlers
- Improve `/proc` filesystem entries (add /proc/meminfo, /proc/uptime)

**Kernel (intermediate):**
- Add a new /dev device (e.g. /dev/random with better entropy)
- Implement `pipe2` O_NONBLOCK flag
- Add `CLOCK_REALTIME` support to `sys_clock_gettime`
- Fix ARM64 virtio-blk-mmio write reliability

**AI infrastructure:**
- Add inference request prioritization (high/low priority queues)
- Implement model warm-up (pre-load model on daemon start)
- Add inference metrics (latency tracking per request)
- Write a new tool ELF (e.g. `json_parser.elf`, `http_client.elf`)

**Documentation:**
- Add inline documentation to kernel subsystems
- Write architecture docs for specific subsystems
- Add more use case examples to README

### Coding Standards

See `CONVENTIONS.md` for the full coding standard. Key points:

- **Foundation headers**: Always include `klog.h` (for `pr_info`/`pr_err`/`panic`), `errno.h`, `compiler.h`
- **Logging**: Every kernel `.c` file must start with `#define pr_fmt(fmt) "[subsys] " fmt`
- **User pointers**: Always `validate_user_ptr()` before dereferencing
- **User strings**: Always `copy_string_from_user()` — never raw PTE walks
- **Concurrency**: All shared state needs a spinlock with documented lock ordering
- **No magic numbers**: Define named constants
- **Both architectures**: Use HAL dispatch headers (`arch/cpu.h`, `arch/paging.h`, etc.), never include arch-specific headers directly

### Commit Messages

```
Short summary (imperative mood, <72 chars)

Longer description if needed. Explain WHY, not just WHAT.
Reference the subsystem affected.
```

Examples:
- `Fix TCP retransmission timer overflow for connections > 5 minutes`
- `Add sys_mkfifo syscall for named pipe creation`
- `Improve orchestrator: per-worker targeted token delegation`

### Pull Request Process

1. Fork the repo and create a feature branch
2. Make your changes — both architectures must build (`make && make arm64`)
3. Run relevant tests in QEMU
4. Submit a PR with a clear description of what changed and why
5. CI must pass (x86_64 build + ARM64 build)

### Architecture Decisions

For changes that affect kernel architecture (new syscalls, IPC mechanisms, security boundaries, memory layout), please open an issue first to discuss the design. This avoids wasted effort on approaches that don't fit the project's direction.

## Communication

- **Issues**: Bug reports, feature requests, questions
- **Pull Requests**: Code contributions
- **Discussions**: Architecture proposals, design questions

## License

By contributing, you agree that your contributions will be licensed under the GNU General Public License v3.0.
