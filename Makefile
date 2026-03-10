# Limnx — Makefile
# Builds a bootable x86_64 ISO using Limine boot protocol

CC       := x86_64-elf-gcc
LD       := x86_64-elf-ld
NASM     := nasm
GCC_INCL := $(shell $(CC) -print-file-name=include)
EXTRA_CFLAGS ?=
CFLAGS   := -ffreestanding -nostdinc -isystem $(GCC_INCL) \
            -fno-stack-protector -fno-pic \
            -mno-red-zone -mcmodel=kernel -mno-sse -mno-mmx -mno-sse2 \
            -Wall -Wextra -O2 -g \
            -Ikernel/deps -Ikernel/src $(EXTRA_CFLAGS)
LDFLAGS  := -nostdlib -static -T kernel/linker.ld
NASMFLAGS := -f elf64 -g

# User-space C flags: SSE enabled for float math, small code model
USER_CFLAGS := -ffreestanding -nostdinc -isystem $(GCC_INCL) \
               -fno-stack-protector -fno-pic \
               -mno-red-zone -msse -msse2 \
               -Wall -Wextra -O2 -g \
               -Iuser

KERNEL   := build/kernel
ISO      := limnx.iso

SRCS     := kernel/src/main.c kernel/src/serial.c \
            kernel/src/gdt/gdt.c kernel/src/idt/idt.c \
            kernel/src/mm/pmm.c kernel/src/mm/vmm.c \
            kernel/src/mm/kheap.c \
            kernel/src/sched/tss.c kernel/src/sched/thread.c \
            kernel/src/sched/sched.c \
            kernel/src/syscall/syscall.c \
            kernel/src/proc/process.c kernel/src/proc/elf.c \
            kernel/src/fs/vfs.c kernel/src/fs/tar.c \
            kernel/src/pci/pci.c \
            kernel/src/net/virtio_net.c kernel/src/net/net.c kernel/src/net/tcp.c \
            kernel/src/blk/virtio_blk.c kernel/src/blk/limnfs.c kernel/src/blk/bcache.c \
            kernel/src/net/netstor.c \
            kernel/src/fb/fbcon.c \
            kernel/src/pty/pty.c \
            kernel/src/sync/mutex.c kernel/src/sync/futex.c \
            kernel/src/smp/smp.c kernel/src/smp/lapic.c \
            kernel/src/ipc/unix_sock.c kernel/src/ipc/eventfd.c kernel/src/ipc/agent_reg.c \
            kernel/src/ipc/epoll.c kernel/src/ipc/infer_svc.c kernel/src/ipc/uring.c \
            kernel/src/ipc/cap_token.c kernel/src/ipc/agent_ns.c kernel/src/ipc/taskgraph.c \
            kernel/src/ipc/supervisor.c kernel/src/mm/swap.c
OBJS     := $(patsubst kernel/src/%.c,build/%.o,$(SRCS))

ASM_SRCS := kernel/src/idt/isr_stubs.asm \
            kernel/src/sched/switch.asm \
            kernel/src/syscall/syscall_entry.asm
ASM_OBJS := $(patsubst kernel/src/%.asm,build/%.o,$(ASM_SRCS))

# User-space ASM ELF programs (placed in initrd, loaded at runtime)
USER_ASM_ELFS := build/user/hello.elf build/user/cat.elf build/user/udpecho.elf \
                 build/user/writetest.elf

# User-space C libc objects
LIBC_ASM_SRCS := user/libc/start.asm user/libc/syscalls.asm
LIBC_C_SRCS   := user/libc/string.c user/libc/stdio.c user/libc/math.c user/libc/tensor.c \
                 user/libc/vecstore.c user/libc/agent.c user/libc/transformer.c \
                 user/libc/tokenizer.c user/libc/gguf.c user/libc/dequant.c \
                 user/libc/http.c user/libc/tooldispatch.c user/libc/malloc.c \
                 user/libc/fio.c
LIBC_ASM_OBJS := $(patsubst user/libc/%.asm,build/user/libc/%.o,$(LIBC_ASM_SRCS))
LIBC_C_OBJS   := $(patsubst user/libc/%.c,build/user/libc/%.o,$(LIBC_C_SRCS))
LIBC_OBJS     := $(LIBC_ASM_OBJS) $(LIBC_C_OBJS)

# User-space C ELF programs (linked with libc)
USER_C_ELFS := build/user/mathtest.elf build/user/agenttest.elf build/user/agentrt.elf \
               build/user/infertest.elf build/user/pipetest.elf build/user/shell.elf \
               build/user/generate.elf build/user/chat.elf build/user/learn.elf \
               build/user/agent.elf build/user/toolagent.elf build/user/memtest.elf \
               build/user/ragtest.elf build/user/fstest.elf build/user/fstest2.elf \
               build/user/lmstest.elf build/user/gguftest.elf build/user/gguf2test.elf \
               build/user/agenttest2.elf build/user/ostest.elf \
               build/user/infer.elf build/user/worker.elf \
               build/user/multiagent.elf build/user/s25test.elf \
               build/user/s26test.elf \
               build/user/s27test.elf \
               build/user/s28test.elf \
               build/user/s29test.elf \
               build/user/s30test.elf \
               build/user/s31test.elf \
               build/user/s32test.elf \
               build/user/s33test.elf \
               build/user/s34test.elf \
               build/user/s35test.elf \
               build/user/s36test.elf \
               build/user/s37test.elf \
               build/user/s38test.elf \
               build/user/s39test.elf \
               build/user/s41test.elf \
               build/user/s42test.elf \
               build/user/s44test.elf \
               build/user/s45test.elf \
               build/user/s47test.elf \
               build/user/s48test.elf \
               build/user/s49test.elf \
               build/user/s50test.elf \
               build/user/s51test.elf \
               build/user/s52test.elf \
               build/user/s53test.elf \
               build/user/s54test.elf \
               build/user/s55test.elf \
               build/user/s56test.elf \
               build/user/s57test.elf \
               build/user/s58test.elf \
               build/user/s59test.elf \
               build/user/s61test.elf \
               build/user/s63test.elf \
               build/user/s64test.elf \
               build/user/s65test.elf \
               build/user/s66test.elf \
               build/user/s67test.elf \
               build/user/s68test.elf \
               build/user/s69test.elf \
               build/user/s70test.elf \
               build/user/s71test.elf \
               build/user/s72test.elf \
               build/user/s73test.elf \
               build/user/s74test.elf \
               build/user/crasher.elf \
               build/user/inferd.elf \
               build/user/netagent.elf

USER_ELFS := $(USER_ASM_ELFS) $(USER_C_ELFS)

# Initrd (tar archive from initrd/ directory + user ELFs)
INITRD   := build/initrd.tar

# Disk image for virtio-blk
DISK_IMG := build/disk.img

ALL_OBJS := $(OBJS) $(ASM_OBJS)

LIMINE_DIR := build/limine

.PHONY: all clean run deps limine iso disk

all: $(ISO)

# --- Dependencies ---

deps:
	@bash kernel/get-deps.sh

# --- Limine bootloader (binary release branch) ---

$(LIMINE_DIR)/limine:
	@echo "Cloning Limine (binary branch)..."
	git clone --depth 1 --branch v8.x-binary https://github.com/limine-bootloader/limine.git $(LIMINE_DIR)
	$(MAKE) -C $(LIMINE_DIR)

limine: $(LIMINE_DIR)/limine

# --- Kernel ---

build/%.o: kernel/src/%.c | deps
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: kernel/src/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# --- User ASM programs (ELF64, linked with user/linker.ld) ---

build/user/%.o: user/%.asm user/syscall.inc
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -I user/ $< -o $@

build/user/%.elf: build/user/%.o user/linker.ld
	$(LD) -nostdlib -static -T user/linker.ld $< -o $@

# --- User libc objects ---

build/user/libc/%.o: user/libc/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

build/user/libc/%.o: user/libc/%.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# --- User C programs (linked with libc) ---

build/user/mathtest.o: user/mathtest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/mathtest.elf: build/user/mathtest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/agenttest.o: user/agenttest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/agenttest.elf: build/user/agenttest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/agentrt.o: user/agentrt.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/agentrt.elf: build/user/agentrt.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/infertest.o: user/infertest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/infertest.elf: build/user/infertest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/pipetest.o: user/pipetest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/pipetest.elf: build/user/pipetest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/shell.o: user/shell.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/shell.elf: build/user/shell.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/generate.o: user/generate.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/generate.elf: build/user/generate.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/chat.o: user/chat.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/chat.elf: build/user/chat.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/learn.o: user/learn.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/learn.elf: build/user/learn.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/agent.o: user/agent.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/agent.elf: build/user/agent.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/toolagent.o: user/toolagent.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/toolagent.elf: build/user/toolagent.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/memtest.o: user/memtest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/memtest.elf: build/user/memtest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/ragtest.o: user/ragtest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/ragtest.elf: build/user/ragtest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/fstest.o: user/fstest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/fstest.elf: build/user/fstest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/fstest2.o: user/fstest2.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/fstest2.elf: build/user/fstest2.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/lmstest.o: user/lmstest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/lmstest.elf: build/user/lmstest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/gguftest.o: user/gguftest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/gguftest.elf: build/user/gguftest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/gguf2test.o: user/gguf2test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/gguf2test.elf: build/user/gguf2test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/agenttest2.o: user/agenttest2.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/agenttest2.elf: build/user/agenttest2.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/ostest.o: user/ostest.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/ostest.elf: build/user/ostest.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/infer.o: user/infer.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/infer.elf: build/user/infer.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/worker.o: user/worker.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/worker.elf: build/user/worker.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/multiagent.o: user/multiagent.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/multiagent.elf: build/user/multiagent.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s25test.o: user/s25test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s25test.elf: build/user/s25test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s26test.o: user/s26test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s26test.elf: build/user/s26test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s27test.o: user/s27test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s27test.elf: build/user/s27test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s28test.o: user/s28test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s28test.elf: build/user/s28test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s29test.o: user/s29test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s29test.elf: build/user/s29test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s30test.o: user/s30test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s30test.elf: build/user/s30test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s31test.o: user/s31test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s31test.elf: build/user/s31test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s32test.o: user/s32test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s32test.elf: build/user/s32test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s33test.o: user/s33test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s33test.elf: build/user/s33test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s34test.o: user/s34test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s34test.elf: build/user/s34test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s35test.o: user/s35test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s35test.elf: build/user/s35test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s36test.o: user/s36test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s36test.elf: build/user/s36test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s37test.o: user/s37test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s37test.elf: build/user/s37test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s38test.o: user/s38test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s38test.elf: build/user/s38test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s39test.o: user/s39test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s39test.elf: build/user/s39test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s41test.o: user/s41test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s41test.elf: build/user/s41test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s42test.o: user/s42test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s42test.elf: build/user/s42test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s44test.o: user/s44test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s44test.elf: build/user/s44test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s45test.o: user/s45test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s45test.elf: build/user/s45test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s47test.o: user/s47test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s47test.elf: build/user/s47test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s48test.o: user/s48test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s48test.elf: build/user/s48test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s49test.o: user/s49test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s49test.elf: build/user/s49test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s50test.o: user/s50test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s50test.elf: build/user/s50test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s51test.o: user/s51test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s51test.elf: build/user/s51test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s52test.o: user/s52test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s52test.elf: build/user/s52test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s53test.o: user/s53test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s53test.elf: build/user/s53test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s54test.o: user/s54test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s54test.elf: build/user/s54test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s55test.o: user/s55test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s55test.elf: build/user/s55test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s56test.o: user/s56test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s56test.elf: build/user/s56test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s57test.o: user/s57test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s57test.elf: build/user/s57test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s58test.o: user/s58test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s58test.elf: build/user/s58test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s59test.o: user/s59test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s59test.elf: build/user/s59test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s61test.o: user/s61test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s61test.elf: build/user/s61test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s63test.o: user/s63test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s63test.elf: build/user/s63test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s64test.o: user/s64test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s64test.elf: build/user/s64test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s65test.o: user/s65test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s65test.elf: build/user/s65test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s66test.o: user/s66test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s66test.elf: build/user/s66test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s67test.o: user/s67test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s67test.elf: build/user/s67test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s68test.o: user/s68test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s68test.elf: build/user/s68test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s69test.o: user/s69test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s69test.elf: build/user/s69test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s70test.o: user/s70test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s70test.elf: build/user/s70test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/crasher.o: user/crasher.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/crasher.elf: build/user/crasher.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s71test.o: user/s71test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s71test.elf: build/user/s71test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s72test.o: user/s72test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s72test.elf: build/user/s72test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s73test.o: user/s73test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s73test.elf: build/user/s73test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/s74test.o: user/s74test.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/s74test.elf: build/user/s74test.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/netagent.o: user/netagent.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/netagent.elf: build/user/netagent.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

build/user/inferd.o: user/inferd.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/inferd.elf: build/user/inferd.o $(LIBC_OBJS) user/libc/linker.ld
	$(LD) -nostdlib -static -T user/libc/linker.ld $(LIBC_OBJS) $< -o $@

# --- Disk image for virtio-blk ---

$(DISK_IMG):
	@mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	@echo "Created 64MB disk image: $@"

disk: $(DISK_IMG)

# --- Initrd (tar archive with user ELFs + data files) ---

$(INITRD): $(USER_ELFS) $(wildcard initrd/*)
	@mkdir -p build/initrd_staging
	cp initrd/* build/initrd_staging/ 2>/dev/null || true
	cp $(USER_ELFS) build/initrd_staging/
	tar cf $@ -C build/initrd_staging .
	rm -rf build/initrd_staging

$(KERNEL): $(ALL_OBJS)
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@

# --- ISO ---

$(ISO): $(KERNEL) $(INITRD) $(LIMINE_DIR)/limine
	@echo "Building ISO..."
	@mkdir -p build/iso_root/boot build/iso_root/boot/limine build/iso_root/EFI/BOOT
	cp $(KERNEL) build/iso_root/boot/kernel
	cp $(INITRD) build/iso_root/boot/initrd.tar
	cp limine.conf build/iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/BOOTX64.EFI build/iso_root/EFI/BOOT/BOOTX64.EFI
	cp $(LIMINE_DIR)/BOOTIA32.EFI build/iso_root/EFI/BOOT/BOOTIA32.EFI 2>/dev/null || true
	cp $(LIMINE_DIR)/limine-bios-cd.bin build/iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys build/iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin build/iso_root/boot/limine/
	xorriso -as mkisofs \
		-R -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso_root -o $(ISO) 2>&1
	$(LIMINE_DIR)/limine bios-install $(ISO)
	@echo "ISO ready: $(ISO)"

iso: $(ISO)

# --- Run in QEMU ---

run: $(ISO) $(DISK_IMG)
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(ISO) \
		-m 4G \
		-smp 2 \
		-serial stdio \
		-display none \
		-no-reboot \
		-device virtio-net-pci,netdev=net0 \
		-netdev user,id=net0,hostfwd=udp::5555-10.0.2.15:1234,hostfwd=tcp::8080-10.0.2.15:80 \
		-drive file=$(DISK_IMG),format=raw,if=none,id=disk0 \
		-device virtio-blk-pci,drive=disk0

# --- Test (boot + run 'test all' via shell) ---

test:
	@echo "=== Building with RUN_BOOT_TESTS ==="
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS=-DRUN_BOOT_TESTS
	$(MAKE) disk
	@echo "=== Running all tests ==="
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(ISO) \
		-m 4G \
		-smp 2 \
		-serial stdio \
		-display none \
		-no-reboot \
		-device virtio-net-pci,netdev=net0 \
		-netdev user,id=net0 \
		-drive file=$(DISK_IMG),format=raw,if=none,id=disk0 \
		-device virtio-blk-pci,drive=disk0

# --- Clean ---

clean:
	rm -rf build $(ISO) qemu_log.txt
