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

## Use Cases

### 1. Loading and Serving an AI Model

```
┌─────────────┐     sys_infer_register      ┌──────────────────┐
│   inferd     │ ──────────────────────────► │  Kernel           │
│              │   "summarizer"              │  infer_svc        │
│  1. Open     │   "/tmp/summarizer.sock"    │  registry         │
│     model.gguf                             │                   │
│  2. Parse    │     sys_infer_health ──────►│  health monitor   │
│     GGUF v3  │     (heartbeat)             │  load balancer    │
│  3. Dequant  │                             │  result cache     │
│     Q4_0→F32 │◄────── unix socket ────────│  request router   │
│  4. Init     │   receive prompt            │                   │
│     transformer                            └──────────────────┘
│  5. Listen   │
│     on socket│
└─────────────┘

# Start the inference daemon:
/inferd.elf /model.gguf summarizer /tmp/summarizer.sock
```

Multiple daemons can register under the same name — the kernel load-balances across them.

### 2. Chatting with an AI Model

```
┌──────────┐   sys_infer_request    ┌────────┐   unix sock   ┌─────────┐
│  chat.elf │ ───────────────────►  │ Kernel  │ ───────────► │  inferd  │
│           │  "summarizer"         │ infer   │              │          │
│ you> Hi   │  "Hi there"           │ _svc    │              │ tokenize │
│           │                       │         │              │ forward  │
│ [bot] ... │ ◄─────────────────── │ cache?  │ ◄─────────── │ sample   │
│           │   response            │ return  │  response    │ decode   │
└──────────┘                        └────────┘               └─────────┘

# Interactive chat with RAG memory:
/chat.elf

# Or direct text generation:
/generate.elf
prompt> The quick brown fox
[gen] jumps over the lazy...
```

The kernel caches responses — repeated prompts return instantly without hitting the daemon.

### 3. Setting Up a Single AI Agent

```
┌──────────────────┐
│   toolagent.elf   │
│                   │
│  1. Register      │─── sys_agent_register("code_reviewer")
│  2. Load model    │─── gguf_load / transformer_init
│  3. Listen        │─── sys_topic_subscribe(review_topic)
│                   │
│  Loop:            │
│    recv task  ◄───│─── sys_topic_recv(review_topic)
│    think      ────│─── transformer_forward (local)
│         or    ────│─── sys_infer_request (remote)
│    act        ────│─── sys_exec / sys_fwrite / sys_sendto
│    publish    ────│─── sys_topic_publish(results_topic)
│                   │
└──────────────────┘
```

Agents discover each other via `sys_agent_lookup("code_reviewer")` → returns PID.

### 4. Agent Communication Channels

Limnx provides 5 IPC channels, each suited to different agent patterns:

```
 Agent A                              Agent B
 ┌──────┐    pub/sub (broadcast)     ┌──────┐
 │      │ ═══════════════════════════│      │    1-to-many, fire-and-forget
 │      │                            │      │    sys_topic_publish / _recv
 │      │    unix socket (stream)    │      │
 │      │ ───────────────────────────│      │    1-to-1, bidirectional
 │      │                            │      │    sys_unix_connect / _send
 │      │    pipe (parent→child)     │      │
 │      │ ──────────────────────────►│      │    1-to-1, unidirectional
 │      │                            │      │    sys_pipe + sys_fork
 │      │    shared memory (fast)    │      │
 │      │ ◄═══════════════════════►  │      │    zero-copy, lock-free
 │      │                            │      │    sys_shmget / _shmat
 │      │    inference service       │      │
 │      │ ─────── kernel ───────────►│      │    routed, cached, load-balanced
 │      │                            │      │    sys_infer_request
 └──────┘                            └──────┘
```

### 5. Agent Swarm with Orchestration

```
                    ┌──────────────────┐
                    │   orchestrator    │
                    │                  │
                    │ 1. ns_create     │─── isolated namespace
                    │ 2. token_create  │─── CAP_INFER bearer token
                    │ 3. super_create  │─── supervisor tree
                    │ 4. task_create   │─── A → B → C (DAG)
                    │ 5. super_start   │─── launch workers
                    └──────┬───────────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
         ┌────▼───┐   ┌───▼────┐   ┌───▼────┐
         │worker_0│   │worker_1│   │worker_2│
         │        │   │        │   │        │
         │seccomp │   │seccomp │   │seccomp │    sandboxed
         │sandbox │   │sandbox │   │sandbox │    (no fork/exec/kill)
         │        │   │        │   │        │
         │cap_token│  │cap_token│  │cap_token│   scoped CAP_INFER
         │(bearer)│   │(bearer)│   │(bearer)│
         │        │   │        │   │        │
         │topic_  │   │topic_  │   │topic_  │    receive tasks
         │ recv   │   │ recv   │   │ recv   │    via pub/sub
         │        │   │        │   │        │
         │infer_  │   │infer_  │   │infer_  │    call AI model
         │request │   │request │   │request │    via kernel router
         │        │   │        │   │        │
         │topic_  │   │topic_  │   │topic_  │    publish results
         │ pub    │   │ pub    │   │ pub    │    via pub/sub
         └────────┘   └────────┘   └────────┘

Task graph enforces execution order:
  Task A (analyze)  ─── must complete before ───►  Task B (transform)
  Task B (transform) ── must complete before ───►  Task C (summarize)
```

### 6. Spawning Agents Dynamically

```
# Supervisor handles lifecycle — crashed agents auto-restart

super_id = sys_super_create("data_pipeline")
sys_super_set_policy(super_id, ONE_FOR_ONE)    # restart only crashed child

sys_super_add(super_id, "/fetcher.elf",   ns_id, CAP_NET)
sys_super_add(super_id, "/parser.elf",    ns_id, CAP_FS_READ)
sys_super_add(super_id, "/analyzer.elf",  ns_id, CAP_INFER)
sys_super_add(super_id, "/writer.elf",    ns_id, CAP_FS_WRITE)

sys_super_start(super_id)    # launch all 4

# If analyzer crashes → supervisor restarts only analyzer
# If using ONE_FOR_ALL → supervisor restarts all 4

sys_super_stop(super_id)     # clean shutdown
```

Agents can also be spawned ad-hoc via `fork + execve` with scoped capabilities:

```c
long pid = sys_fork();
if (pid == 0) {
    sys_seccomp(allowed_mask, 1, allowed_mask_hi);  // sandbox
    sys_execve("/agent.elf", argv);
}
// Parent delegates a capability token to the child:
sys_token_delegate(parent_token, pid, CAP_INFER, "summarizer");
```

### 7. AI Agent Orchestration as a Service

```
┌─────────────────────────────────────────────────────────┐
│                   Limnx Kernel                           │
│                                                         │
│  ┌─────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │Namespace │  │Supervisor│  │Task Graph│  │Inference│ │
│  │  ns=1    │  │  tree    │  │  DAG     │  │ Service │ │
│  │ agents   │  │ restart  │  │ deps     │  │ routing │ │
│  │ quotas   │  │ policy   │  │ fan-out  │  │ caching │ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬────┘ │
│       │             │             │              │      │
│  ┌────▼─────────────▼─────────────▼──────────────▼────┐ │
│  │              Pub/Sub Message Bus                    │ │
│  │   topics: tasks, results, alerts, model_updates    │ │
│  └────────────────────────────────────────────────────┘ │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │Cap Tokens│  │ Seccomp  │  │  Epoll   │              │
│  │ delegate │  │ sandbox  │  │ I/O mux  │              │
│  │ revoke   │  │ restrict │  │ events   │              │
│  └──────────┘  └──────────┘  └──────────┘              │
└─────────────────────────────────────────────────────────┘

The kernel IS the orchestration layer:

  ┌──────────┐        ┌──────────┐        ┌──────────┐
  │ Customer │        │ Internal │        │ External │
  │ Service  │        │ Pipeline │        │ API      │
  │          │        │          │        │          │
  │ ns=1     │        │ ns=2     │        │ ns=3     │
  │ 3 agents │        │ 5 agents │        │ 2 agents │
  │ CAP_NET  │        │ CAP_INFER│        │ CAP_NET  │
  │          │        │ CAP_FS   │        │ CAP_INFER│
  └──────────┘        └──────────┘        └──────────┘

  Each namespace is a self-contained service:
  - Own supervisor tree (auto-restart)
  - Own task graphs (workflow DAGs)
  - Own capability tokens (scoped permissions)
  - Own pub/sub topics (internal messaging)
  - Cross-namespace access requires CAP_XNS_* tokens
```

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
