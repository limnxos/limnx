# Limnx

An operating system where AI agents are first-class citizens.

Limnx is a from-scratch x86_64/ARM64 kernel with built-in primitives for AI inference, agent orchestration, and security isolation. The kernel doesn't just run AI workloads — it **governs** them: routing inference requests, enforcing capability tokens, sandboxing workers with seccomp, and orchestrating multi-agent workflows through task graphs and pub/sub.

## Try It

```bash
# Build and boot (x86_64)
make clean && make
make disk
make run

# In the shell:
/orchestrator.elf     # Full AI-native demo
/generate.elf         # Interactive text generation
/infer_test.elf       # 49 inference pipeline tests
```

The orchestrator demo exercises 7 kernel primitives in one command:

```
=============================================
  Limnx Agent Orchestration Demo
=============================================

Step 1: Creating namespace...          → Isolated agent group
Step 2: Starting inference daemon...   → GGUF model loaded (dim=64, 2 layers)
Step 3: Creating pub/sub topics...     → Task distribution + result collection
Step 4: Creating supervisor...         → Managed worker lifecycle
Step 5: Creating capability tokens...  → Scoped CAP_INFER bearer token
Step 6: Adding 3 workers...            → Sandboxed with seccomp
Step 7: Creating task graph...         → A→B→C dependency chain
Step 8: Starting supervisor...         → Workers launch, subscribe, sandbox

=== Executing Task Graph ===

Task A completed ✓  (worker calls inference, publishes result)
Task B completed ✓  (waits for A, then executes)
Task C completed ✓  (waits for B, then executes)

=== Results ===

RESULT:8:1:<generated text from transformer>
RESULT:7:2:<generated text from transformer>
RESULT:8:3:<generated text from transformer>
Collected 3/3 results

Demo Complete
```

## What Makes It AI-Native

Traditional OSes treat AI as "just another process." Limnx provides **kernel primitives** purpose-built for AI workloads:

| Primitive | Syscalls | What It Does |
|-----------|----------|-------------|
| **Inference Service** | `infer_register`, `infer_request`, `infer_submit/poll/result` | Kernel-routed model serving with health monitoring, load balancing, result caching, async completion, model hot-swap |
| **Agent Namespaces** | `ns_create`, `ns_join`, `ns_setquota` | Resource-isolated agent groups with process/memory quotas |
| **Capability Tokens** | `token_create`, `token_delegate`, `token_revoke` | Fine-grained, delegated, revocable authorization (depth-4 delegation chains, cascading revocation) |
| **Task Graphs** | `task_create`, `task_depend`, `task_start`, `task_complete` | DAG workflow orchestration with cross-namespace dependencies |
| **Supervisor Trees** | `super_create`, `super_add`, `super_start`, `super_stop` | Erlang-style process supervision with ONE_FOR_ONE/ONE_FOR_ALL restart policies |
| **Pub/Sub** | `topic_create`, `topic_publish`, `topic_subscribe`, `topic_recv` | Broadcast messaging across agent groups |
| **Seccomp Sandbox** | `seccomp` | Syscall allowlist — workers can only call inference + I/O, not fork/exec/kill |

The security model is a **trifecta**:
- **Namespaces** isolate what agents can see
- **Capability tokens** control what agents can access
- **Seccomp** restricts how agents interact with the kernel

## Architecture

```
 User Space (Ring 3 / EL0)
 ┌─────────────────────────────────────────────────────────┐
 │  orchestrator   agent_worker(×3)   inferd   generate    │
 │  chat           toolagent          shell    busybox     │
 │                                                         │
 │  libc: syscalls, printf, math, tokenizer, GGUF, HTTP   │
 ├─────────────────────────────────────────────────────────┤
 │           SYSCALL/SYSRET (x86_64) | SVC (ARM64)         │
 ├─────────────────────────────────────────────────────────┤
 Kernel (Ring 0 / EL1)
 │                                                         │
 │  Process        Scheduler       Memory       Filesystem │
 │  fork/exec/COW  SMP preemptive  4-level PT   LimnFS/VFS │
 │  signals        2 CPUs          swap/demand  block cache │
 │                                                         │
 │  AI Primitives                  Security                │
 │  infer_svc (routing/cache)      namespaces              │
 │  supervisor trees               capability tokens       │
 │  task graphs (DAG)              seccomp filters         │
 │  pub/sub messaging              UID/GID/caps            │
 │  agent registry                                         │
 │                                                         │
 │  Networking     IPC             Devices                 │
 │  TCP/IP/UDP     unix sockets    virtio-net/blk          │
 │  ICMP/ARP       epoll/eventfd   PCI / MMIO              │
 │                 io_uring        LAPIC / GIC              │
 │                 pipes/shm       PL011 / COM1             │
 │                                                         │
 │  HAL (arch/)                                            │
 │  x86_64: GDT/IDT/TSS, LAPIC, MSR, CR3, SYSCALL/SYSRET │
 │  ARM64:  GIC, TTBR, VBAR, SVC, PSCI SMP                │
 └─────────────────────────────────────────────────────────┘
       Limine (x86_64 BIOS/UEFI)  |  Direct boot (ARM64)
```

## Inference Pipeline

```
User program                    Kernel                         inferd daemon
     │                            │                                │
     │ sys_infer_request ──────►  │ route to service ────────────► │
     │  ("default", prompt)       │ (namespace-aware,              │ load GGUF model
     │                            │  health-checked,               │ BPE tokenize
     │                            │  cached results)               │ transformer forward
     │                            │                                │ temperature + top-k sample
     │ ◄────── response ───────── │ ◄──── unix socket ─────────── │ BPE decode
     │                            │ cache result                   │
     │                            │                                │
     │ sys_infer_submit ────────► │ async worker thread ─────────► │
     │  (non-blocking)            │                                │
     │ sys_infer_poll ──────────► │ check completion               │
     │ sys_infer_result ────────► │ copy response                  │
```

Supported model formats: GGUF v3 (F32, F16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q2_K–Q6_K). BPE tokenizer loaded from GGUF metadata. Transformer: RMS norm, multi-head attention, GQA, RoPE, SwiGLU, KV cache.

## Build

**x86_64** requires: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `xorriso`, `qemu-system-x86_64`

**ARM64** requires: `aarch64-elf-gcc` (or `aarch64-linux-gnu-gcc`), `qemu-system-aarch64`

```bash
make clean && make     # build x86_64 ISO (fetches Limine on first run)
make run               # boot in QEMU with virtio-net + virtio-blk
make disk              # create 64MB virtio-blk disk image
make arm64             # build ARM64 kernel ELF
./run-arm64.sh         # clean build + boot ARM64 in QEMU
```

## Test Suite

```bash
# In the Limnx shell:
/infer_test.elf        # 49 inference pipeline tests (both archs)
/fs_test.elf           # Filesystem tests
/proc_test.elf         # Process/fork/exec tests
/ipc_test.elf          # IPC tests
/mm_test.elf           # Memory management tests
/net_test.elf          # Network tests
/security_test.elf     # Security tests
/system_test.elf       # System integration tests
```

## Kernel Stats

- **140+ syscalls** (Linux-compatible numbers + Limnx-specific 512+)
- **2 architectures**: x86_64 (primary), ARM64 (full feature parity)
- **SMP**: 2 CPUs, per-CPU data, LAPIC timer preemption
- **Memory**: 4-level paging, HHDM, kernel heap up to 1GB, mmap up to 2GB
- **Filesystem**: LimnFS (ext2-inspired, triple indirect blocks, 64MB disk)
- **Networking**: TCP (full state machine), UDP, ICMP, ARP, software loopback
- **Userspace**: Busybox ash (47 applets), custom libc, init system

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
