# Limnx AI Agent Workflows — Comprehensive Guide

This document provides real, executable workflows for building and operating AI agents on Limnx. Every command shown runs on the actual system.

---

## 1. Running an Inference Service

### Start the inference daemon with a model

```bash
# Boot Limnx, then:
/inferd.elf /test.gguf default /tmp/inferd.sock 0 &
```

Arguments: `<model_path> <service_name> <socket_path> <max_requests (0=unlimited)>`

The daemon:
1. Loads the GGUF model (parses metadata, dequantizes weights, builds transformer)
2. Initializes BPE tokenizer from GGUF vocab
3. Creates a unix socket and starts listening
4. Registers with the kernel: `sys_infer_register("default", "/tmp/inferd.sock")`
5. Reports health heartbeats to the kernel load balancer

### Send inference requests

```bash
# Interactive text generation (routes through kernel to inferd)
/generate.elf
prompt> hello world

# Interactive chat with RAG memory
/chat.elf
you> what is limnx

# Run all inference tests
/infer_test.elf
```

### Multiple model instances

```bash
# Start two different models
/inferd.elf /test.gguf summarizer /tmp/sum.sock 0 &
/inferd.elf /test_gqa.gguf analyst /tmp/ana.sock 0 &

# From code: route to specific service
sys_infer_request("summarizer", prompt, len, resp, resp_len);
sys_infer_request("analyst", prompt, len, resp, resp_len);
```

The kernel routes, load-balances, caches, and health-checks all registered services.

---

## 2. Creating an AI Agent

### Simple agent pattern

An agent is any process that:
1. Registers a name with the kernel
2. Subscribes to message topics
3. Processes requests using inference + tools
4. Publishes results

```bash
# Run the tool-using agent
/toolagent.elf
agent> list files and read hello.txt
  [step 1/2: ls]
  [step 2/2: cat]
  [ai] <model commentary>
```

### Agent with inference service

```bash
# Terminal 1: Start inference daemon
/inferd.elf /test.gguf default /tmp/inferd.sock 0 &

# Terminal 2: Run agent (routes AI calls through kernel)
/toolagent.elf
agent> read hello.txt
  [ai] <response from inferd via kernel routing>
```

### Agent lifecycle in code

```c
// 1. Register with the kernel
sys_agent_register("my_agent");

// 2. Subscribe to a work topic
long topic = sys_topic_subscribe(work_topic_id);

// 3. Main loop
while (running) {
    char msg[256];
    unsigned long sender;
    long n = sys_topic_recv(work_topic_id, msg, 256, &sender);
    if (n > 0) {
        // 4. Call inference
        char resp[256];
        sys_infer_request("default", msg, n, resp, 256);

        // 5. Use a tool
        tool_result_t result;
        tool_dispatch("/file_reader.elf", argv, 0, 1000, msg, n, &result);

        // 6. Publish result
        sys_topic_publish(result_topic_id, resp, strlen(resp));
    }
    sys_yield();
}
```

---

## 3. Tool Use and Tool Chains

### Available tools

```bash
# Interactive tool-calling agent
/tool_demo.elf

# Single tool calls
tool> read /hello.txt          # reads file via file_reader.elf
tool> run ls /                 # executes command via code_executor.elf
tool> run date                 # get current date
tool> run cat /etc/passwd      # read system file

# Multi-tool chains
tool> read /hello.txt and count words
# Step 1: file_reader reads file
# Step 2: built-in word counter processes output
# Result: "4 words"
```

### Creating a custom tool

A tool is an ELF binary that reads from stdin/argv and writes to stdout:

```c
// my_tool.c
#include "../libc/libc.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        sys_write("usage: my_tool <input>\n", 23);
        return 1;
    }

    // Process input
    const char *input = argv[1];

    // Do work...
    printf("processed: %s\n", input);

    return 0;
}
```

Add to Makefile, rebuild. The tool is callable via `tool_dispatch()` from any agent — it runs in a forked sandboxed process with pipe I/O.

### Tool security model

Each tool runs with:
- **Forked process** — isolated address space
- **Pipe I/O** — no shared memory side channels
- **Capability scoping** — tool only gets the caps it needs
- **Seccomp** — syscall allowlist (optional)

---

## 4. Agent Communication Patterns

### Pattern A: Pub/Sub broadcast

Best for: event-driven systems, notifications, task distribution

```bash
# Run the orchestrator demo (creates topics, publishes tasks)
/orchestrator.elf
```

In code:
```c
// Publisher
long topic = sys_topic_create("alerts", ns_id);
sys_topic_publish(topic, "disk full", 9);

// Subscriber (another process)
sys_topic_subscribe(topic);
char msg[256];
unsigned long sender;
long n = sys_topic_recv(topic, msg, 256, &sender);
// msg = "disk full", sender = publisher PID
```

### Pattern B: Unix socket (bidirectional)

Best for: request/response, streaming, persistent connections

```c
// Server
long fd = sys_unix_socket();
sys_unix_bind(fd, "/tmp/my_agent.sock");
sys_unix_listen(fd);
long client = sys_unix_accept(fd);
// read request, send response

// Client
long fd = sys_unix_connect("/tmp/my_agent.sock");
sys_fwrite(fd, "request", 7);
sys_read(fd, response, 256);
```

### Pattern C: Pipe (parent→child)

Best for: tool invocation, data pipelines

```c
long rfd, wfd;
sys_pipe(&rfd, &wfd);
long pid = sys_fork();
if (pid == 0) {
    // Child reads from pipe
    sys_dup2(rfd, 0);
    sys_execve("/processor.elf", argv);
}
// Parent writes data
sys_fwrite(wfd, data, len);
sys_close(wfd);  // signal EOF
sys_waitpid(pid);
```

### Pattern D: Inference service (kernel-routed)

Best for: AI model access with load balancing and caching

```c
// Any process can call any registered model
char resp[256];
long n = sys_infer_request("summarizer", text, text_len, resp, 256);
// Kernel routes to healthy instance, caches result
```

### Pattern E: Shared memory (zero-copy)

Best for: large data transfer between cooperating agents

```c
// Process A
long shm_id = sys_shmget(42, 4);  // 4 pages = 16KB
long addr = sys_shmat(shm_id);
memcpy((void *)addr, big_data, 16384);

// Process B (same shm key)
long shm_id = sys_shmget(42, 4);
long addr = sys_shmat(shm_id);
// addr points to same physical pages — zero copy
```

---

## 5. Agent Orchestration

### Full orchestration demo

```bash
/orchestrator.elf
```

This runs the complete stack:
1. Creates isolated namespace
2. Starts inferd with GGUF model
3. Creates pub/sub topics for task distribution + result collection
4. Creates supervisor tree with 3 workers
5. Creates CAP_INFER capability tokens, delegates to workers
6. Workers self-sandbox with seccomp
7. Creates task graph: A → B → C (dependency chain)
8. Dispatches tasks via pub/sub
9. Workers call inference, publish results
10. Collects 3/3 results
11. Revokes tokens, stops supervisor, kills inferd

### Building a custom orchestrator

```c
// 1. Namespace — isolate your agent group
long ns_id = sys_ns_create("my_pipeline");
sys_ns_setquota(ns_id, NS_QUOTA_PROCS, 16);

// 2. Inference service
long inferd_pid = sys_fork();
if (inferd_pid == 0) {
    const char *argv[] = {"inferd", "/model.gguf", "my_model",
                           "/tmp/my.sock", "0", NULL};
    sys_execve("/inferd.elf", argv);
    sys_exit(99);
}

// 3. Capability tokens
long token = sys_token_create(CAP_INFER | CAP_XNS_INFER, 0, "my_model");

// 4. Supervisor
long super = sys_super_create("my_supervisor");
sys_super_set_policy(super, SUPER_ONE_FOR_ONE);
sys_super_add(super, "/my_worker.elf", ns_id, 0);
sys_super_add(super, "/my_worker.elf", ns_id, 0);
sys_super_start(super);

// 5. Task graph
long t1 = sys_task_create("fetch_data", ns_id);
long t2 = sys_task_create("analyze", ns_id);
long t3 = sys_task_create("report", ns_id);
sys_task_depend(t2, t1);  // analyze waits for fetch
sys_task_depend(t3, t2);  // report waits for analyze

// 6. Execute
sys_task_start(t1);
sys_topic_publish(task_topic, "TASK:1:fetch http://...");

// 7. Collect results
// ... poll topic, wait for tasks ...

// 8. Cleanup
sys_token_revoke(token);
sys_super_stop(super);
sys_kill(inferd_pid, 9);
```

---

## 6. Running Agents as Services

### Using the init system

Edit `/etc/inittab` to auto-start agents at boot:

```
# /etc/inittab format: name:path:flags
# flags: respawn (auto-restart), once, wait

serviced:/serviced.elf:respawn
shell:/bin/ash:wait
inferd:/inferd.elf:respawn
my_agent:/my_agent.elf:respawn
```

The init system (pid 1) reads this file and:
- Forks each service
- Monitors for crashes
- Respawns services with `respawn` flag

### Using supervisor trees (runtime)

```bash
# From the shell or another program:
# supervisors manage lifecycle without editing inittab
```

```c
long super = sys_super_create("data_pipeline");
sys_super_set_policy(super, SUPER_ONE_FOR_ONE);

// Add workers — they auto-restart on crash
sys_super_add(super, "/fetcher.elf", ns_id, CAP_NET);
sys_super_add(super, "/parser.elf", ns_id, CAP_FS_READ);
sys_super_add(super, "/analyzer.elf", ns_id, CAP_INFER);

sys_super_start(super);
// Workers run until you call sys_super_stop(super)
```

---

## 7. WASM Agent Plugins

### Run a WASM module

```bash
/wasm_runner.elf /test.wasm add 3 4      # → 7
/wasm_runner.elf /test.wasm fib 10       # → 55
/wasm_runner.elf /test.wasm hello        # → calls host print(42)
/wasm_runner.elf /test.wasm              # list and run all exports
```

### Creating WASM plugins

Generate with Python:
```bash
python3 tools/gen_wasm.py    # creates initrd/test.wasm
```

Or compile C to WASM using any WASM toolchain (wasi-sdk, emscripten) on your host, then copy the .wasm file to Limnx.

WASM modules can call host functions (print, putchar) registered by the runner. The WASM interpreter runs in userspace — fully portable across x86_64 and ARM64.

---

## 8. Security: Sandboxing Agents

### Capability tokens (what agents can access)

```c
// Create a token: CAP_INFER for service "summarizer"
long token = sys_token_create(CAP_INFER, target_pid, "summarizer");

// Delegate a sub-token to a worker
long sub = sys_token_delegate(token, worker_pid, CAP_INFER, "summarizer");

// Worker can now call sys_infer_request("summarizer", ...)
// Other services are denied

// Revoke — cascading (kills all sub-tokens)
sys_token_revoke(token);
```

### Seccomp (what syscalls agents can make)

```c
#include "limnx/syscall_nr.h"

unsigned long mask_lo = 0, mask_hi = 0;
#define ALLOW(nr) do { \
    if ((nr) < 64) mask_lo |= (1UL << (nr)); \
    else if ((nr) < 128) mask_hi |= (1UL << ((nr) - 64)); \
} while(0)

ALLOW(SYS_READ);
ALLOW(SYS_WRITE);
ALLOW(SYS_EXIT);
ALLOW(SYS_SCHED_YIELD);
// All Limnx syscalls (512+) pass through automatically

sys_seccomp(mask_lo, 1 /* strict */, mask_hi);
// Now: fork → SIGKILL, exec → SIGKILL, socket → SIGKILL
```

### Namespaces (what agents can see)

```c
long ns = sys_ns_create("sandbox");
sys_ns_setquota(ns, NS_QUOTA_PROCS, 4);     // max 4 processes
sys_ns_setquota(ns, NS_QUOTA_MEM_PAGES, 256); // max 1MB memory
sys_ns_join(ns);
// Now isolated — can't see agents in other namespaces
```

---

## 9. Real-World Use Case: Data Processing Pipeline

### Scenario
Fetch a file, analyze its content with AI, write a summary.

```bash
# 1. Start inference service
/inferd.elf /test.gguf default /tmp/inferd.sock 0 &

# 2. Use tool_demo for the pipeline
/tool_demo.elf
tool> read /etc/passwd
  [result] root:x:0:0:root:/:/limnsh.elf...

tool> read /hello.txt and count words
  [chain: step 1] file_reader → "Hello from the initrd!"
  [chain: step 2] count words → "4 words"
```

### Scenario in code (orchestrated)

```c
// Create pipeline
long ns = sys_ns_create("pipeline");
long super = sys_super_create("pipe_super");

// Stage 1: Fetcher reads data
sys_super_add(super, "/file_reader.elf", ns, CAP_FS_READ);

// Stage 2: Analyzer calls inference
sys_super_add(super, "/agent_worker.elf", ns, CAP_INFER);

// Task graph: fetch → analyze → report
long t1 = sys_task_create("fetch", ns);
long t2 = sys_task_create("analyze", ns);
sys_task_depend(t2, t1);

sys_super_start(super);
sys_task_start(t1);
// ... workers pick up tasks via pub/sub ...
```

---

## 10. Real-World Use Case: Multi-Agent Customer Service

### Scenario
Three agents cooperate: classifier, responder, logger.

```c
// Namespace for the service
long ns = sys_ns_create("customer_svc");

// Topics
long incoming = sys_topic_create("incoming", ns);
long classified = sys_topic_create("classified", ns);
long responses = sys_topic_create("responses", ns);

// Agent 1: Classifier — reads incoming, publishes to classified
// Agent 2: Responder — reads classified, calls inference, publishes to responses
// Agent 3: Logger — reads responses, writes to file

long super = sys_super_create("cs_super");
sys_super_add(super, "/classifier.elf", ns, 0);
sys_super_add(super, "/responder.elf", ns, CAP_INFER);
sys_super_add(super, "/logger.elf", ns, CAP_FS_WRITE);
sys_super_start(super);

// Feed incoming messages
sys_topic_publish(incoming, "How do I reset my password?", 27);
// classifier → classified → responder → inference → responses → logger
```

---

## 11. Real-World Use Case: Code Review Agent

### Scenario
Agent reads source files, sends to model for review, writes report.

```bash
/tool_demo.elf
tool> read /init.elf
  [result] (ELF binary data)

# Better: read a text file
tool> run cat /etc/passwd
  [result] root:x:0:0:root:/:/limnsh.elf...

tool> read /hello.txt
  [result] Hello from the initrd!
```

### Automated via orchestrator

```c
// 1. File reader agent reads all .c files
// 2. Each file sent to inference for review
// 3. Results aggregated and written to /review_report.txt

// This uses: task graph (file1 → file2 → file3 → aggregate)
//            inference service (code review model)
//            file tools (read source, write report)
//            pub/sub (distribute work, collect results)
```

---

## Test Matrix

| Test | Command | Expected | Status |
|------|---------|----------|--------|
| Inference tests | `/infer_test.elf` | 49/49 pass | Verified x86+ARM64 |
| Orchestrator | `/orchestrator.elf` | 3/3 results, tasks complete | Verified x86+ARM64 |
| WASM add | `/wasm_runner.elf /test.wasm add 3 4` | 7 | Verified x86 |
| WASM fib | `/wasm_runner.elf /test.wasm fib 10` | 55 | Verified x86 |
| Tool read | `/tool_demo.elf` → `read /hello.txt` | File contents | Verified x86 |
| Tool exec | `/tool_demo.elf` → `run ls /` | Directory listing | Verified x86 |
| Tool chain | `/tool_demo.elf` → `read /hello.txt and count words` | Word count | Verified x86 |
| Generate | `/generate.elf` → type prompt | Generated text | Verified x86 |
| Chat | `/chat.elf` → type message | Response + memory | Verified x86 |
| Security test | `/security_test.elf` | 15/16 pass | Verified x86 |
| IPC test | `/ipc_test.elf` | Pipes, sockets, pubsub, namespaces | Verified x86 |
| Busybox | `wget`, `ping`, `date`, `bc`, `hexdump` | Applet output | Available (381) |
| Filesystem | `/fs_test.elf` | Create, read, write, seek, rename | Verified |
| Process | `/proc_test.elf` | Fork, exec, waitpid, signals | Verified |
| Memory | `/mm_test.elf` | mmap, munmap, COW | Verified |
| Network | `/net_test.elf` | TCP, UDP, ICMP | Verified |
| Scheduler | `/sched_test.elf` | Yield, preemption, SMP | Verified |

---

## Disk Space

Limnx disk is now **256MB** (248MB filesystem + 8MB swap). Previous limit was 64MB which was insufficient for larger models and test data.

To recreate the disk:
```bash
make disk    # creates build/disk.img (256MB)
```

---

## Planned Workflows (Not Yet Implemented)

- **Network agent**: fetch data from external API via wget/nc, feed to inference
- **SSH/TLS**: secure remote agent access (requires OpenSSL port)
- **Persistent agent state**: save/restore agent memory across reboots via LimnFS
- **Cross-architecture orchestration**: x86_64 coordinator dispatches to ARM64 workers
- **Real model integration**: load a pre-trained TinyLlama/SmolLM GGUF from disk
