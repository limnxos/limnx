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
            -fno-omit-frame-pointer \
            -Wall -Wextra -O2 -g -MMD -MP \
            -Ikernel/deps -Ikernel/src $(EXTRA_CFLAGS)
LDFLAGS  := -nostdlib -static -T kernel/linker.ld
NASMFLAGS := -f elf64 -g

# User-space C flags: SSE enabled for float math, small code model
USER_CFLAGS := -ffreestanding -nostdinc -isystem $(GCC_INCL) \
               -fno-stack-protector -fno-pic \
               -mno-red-zone -msse -msse2 \
               -Wall -Wextra -O2 -g -MMD -MP \
               -Iuser

KERNEL   := build/kernel
ISO      := limnx.iso

SRCS     := kernel/src/main.c \
            kernel/src/arch/x86_64/serial.c \
            kernel/src/arch/x86_64/gdt.c kernel/src/arch/x86_64/idt.c \
            kernel/src/arch/x86_64/tss.c kernel/src/arch/x86_64/lapic.c \
            kernel/src/arch/x86_64/smp.c kernel/src/arch/x86_64/boot.c \
            kernel/src/mm/pmm.c kernel/src/mm/vmm.c \
            kernel/src/mm/kheap.c \
            kernel/src/sched/thread.c \
            kernel/src/sched/sched.c \
            kernel/src/syscall/syscall.c \
            kernel/src/syscall/sys_fs.c \
            kernel/src/syscall/sys_proc.c \
            kernel/src/syscall/sys_mm.c \
            kernel/src/syscall/sys_fd.c \
            kernel/src/syscall/sys_net.c \
            kernel/src/syscall/sys_signal.c \
            kernel/src/syscall/sys_ipc.c \
            kernel/src/syscall/sys_security.c \
            kernel/src/syscall/sys_infer.c \
            kernel/src/syscall/sys_misc.c \
            kernel/src/proc/process.c kernel/src/proc/elf.c \
            kernel/src/fs/vfs.c kernel/src/fs/tar.c \
            kernel/src/pci/pci.c \
            kernel/src/net/virtio_net.c kernel/src/net/net.c kernel/src/net/tcp.c \
            kernel/src/blk/virtio_blk.c kernel/src/blk/limnfs.c kernel/src/blk/bcache.c \
            kernel/src/net/netstor.c \
            kernel/src/fb/fbcon.c \
            kernel/src/pty/pty.c \
            kernel/src/sync/mutex.c kernel/src/sync/futex.c \
            kernel/src/ipc/unix_sock.c kernel/src/ipc/eventfd.c kernel/src/ipc/agent_reg.c \
            kernel/src/ipc/epoll.c kernel/src/ipc/infer_svc.c kernel/src/ipc/uring.c \
            kernel/src/ipc/cap_token.c kernel/src/ipc/agent_ns.c kernel/src/ipc/taskgraph.c \
            kernel/src/ipc/supervisor.c kernel/src/ipc/pipe.c kernel/src/ipc/shm.c \
            kernel/src/ipc/pubsub.c \
            kernel/src/mm/swap.c
OBJS     := $(patsubst kernel/src/%.c,build/%.o,$(SRCS))

ASM_SRCS := kernel/src/arch/x86_64/isr_stubs.asm \
            kernel/src/arch/x86_64/switch.asm \
            kernel/src/arch/x86_64/syscall_entry.asm
ASM_OBJS := $(patsubst kernel/src/%.asm,build/%.o,$(ASM_SRCS))

# User-space ASM ELF programs (placed in initrd, loaded at runtime)
USER_ASM_ELFS := build/user/asm/hello.elf build/user/asm/cat.elf \
                 build/user/asm/udpecho.elf build/user/asm/writetest.elf

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
# Programs, agents, daemons
USER_C_PROGRAMS := build/user/programs/shell.elf build/user/programs/agent.elf \
                   build/user/programs/agentrt.elf build/user/programs/toolagent.elf \
                   build/user/programs/chat.elf build/user/programs/generate.elf \
                   build/user/programs/learn.elf build/user/programs/infer.elf \
                   build/user/programs/inferd.elf build/user/programs/worker.elf \
                   build/user/programs/multiagent.elf build/user/programs/netagent.elf \
                   build/user/programs/crasher.elf \
                   build/user/programs/serviced.elf

# Test programs
USER_C_TESTS := build/user/tests/mathtest.elf build/user/tests/agenttest.elf \
                build/user/tests/agenttest2.elf build/user/tests/infertest.elf \
                build/user/tests/pipetest.elf build/user/tests/memtest.elf \
                build/user/tests/ragtest.elf build/user/tests/fstest.elf \
                build/user/tests/fstest2.elf build/user/tests/lmstest.elf \
                build/user/tests/gguftest.elf build/user/tests/gguf2test.elf \
                build/user/tests/ostest.elf \
                build/user/tests/s25test.elf build/user/tests/s26test.elf \
                build/user/tests/s27test.elf build/user/tests/s28test.elf \
                build/user/tests/s29test.elf build/user/tests/s30test.elf \
                build/user/tests/s31test.elf build/user/tests/s32test.elf \
                build/user/tests/s33test.elf build/user/tests/s34test.elf \
                build/user/tests/s35test.elf build/user/tests/s36test.elf \
                build/user/tests/s37test.elf build/user/tests/s38test.elf \
                build/user/tests/s39test.elf build/user/tests/s41test.elf \
                build/user/tests/s42test.elf build/user/tests/s44test.elf \
                build/user/tests/s45test.elf build/user/tests/s47test.elf \
                build/user/tests/s48test.elf build/user/tests/s49test.elf \
                build/user/tests/s50test.elf build/user/tests/s51test.elf \
                build/user/tests/s52test.elf build/user/tests/s53test.elf \
                build/user/tests/s54test.elf build/user/tests/s55test.elf \
                build/user/tests/s56test.elf build/user/tests/s57test.elf \
                build/user/tests/s58test.elf build/user/tests/s59test.elf \
                build/user/tests/s61test.elf build/user/tests/s63test.elf \
                build/user/tests/s64test.elf build/user/tests/s65test.elf \
                build/user/tests/s66test.elf build/user/tests/s67test.elf \
                build/user/tests/s68test.elf build/user/tests/s69test.elf \
                build/user/tests/s70test.elf build/user/tests/s71test.elf \
                build/user/tests/s72test.elf build/user/tests/s73test.elf \
                build/user/tests/s74test.elf build/user/tests/s75test.elf \
                build/user/tests/s76test.elf build/user/tests/s77test.elf \
                build/user/tests/s78test.elf

USER_C_ELFS := $(USER_C_PROGRAMS) $(USER_C_TESTS)

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
# Static pattern rule: only applies to USER_ASM_ELFS, not C programs.

$(patsubst %.elf,%.o,$(USER_ASM_ELFS)): build/user/%.o: user/%.asm user/syscall.inc
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -I user/ $< -o $@

$(USER_ASM_ELFS): build/user/%.elf: build/user/%.o user/linker.ld
	$(LD) -nostdlib -static -T user/linker.ld $< -o $@

# --- User libc objects ---

build/user/libc/%.o: user/libc/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

build/user/libc/%.o: user/libc/%.c user/libc/libc.h
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# --- User C programs (linked with libc) ---
# Pattern rules replace 500+ lines of per-file boilerplate.
# Header deps auto-tracked via -MMD -MP (see -include at bottom).

build/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.elf: build/user/%.o $(LIBC_OBJS) user/libc/linker.ld
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

# --- Auto-generated dependencies ---
-include $(OBJS:.o=.d)
-include $(LIBC_C_OBJS:.o=.d)

# =============================================================
# ARM64 cross-compilation targets
# =============================================================

ARM64_CC     := aarch64-elf-gcc
ARM64_LD     := aarch64-elf-ld
ARM64_AS     := aarch64-elf-as
ARM64_OBJCOPY := aarch64-elf-objcopy

# Fallback to linux-gnu toolchain if elf toolchain not found
ARM64_CC_CHECK := $(shell which $(ARM64_CC) 2>/dev/null)
ifeq ($(ARM64_CC_CHECK),)
ARM64_CC     := aarch64-linux-gnu-gcc
ARM64_LD     := aarch64-linux-gnu-ld
ARM64_AS     := aarch64-linux-gnu-as
ARM64_OBJCOPY := aarch64-linux-gnu-objcopy
endif

ARM64_CFLAGS := -ffreestanding -nostdinc -isystem $(shell $(ARM64_CC) -print-file-name=include 2>/dev/null || echo /dev/null) \
                -fno-stack-protector -fno-pic \
                -Wall -Wextra -O2 -g -MMD -MP \
                -Ikernel/src
ARM64_LDFLAGS := -nostdlib -static -T kernel/src/arch/arm64/linker.ld

ARM64_KERNEL := build/arm64/kernel

ARM64_C_SRCS := kernel/src/arch/arm64/main.c \
                kernel/src/arch/arm64/serial.c \
                kernel/src/arch/arm64/arch_init.c \
                kernel/src/arch/arm64/interrupt.c \
                kernel/src/arch/arm64/timer.c \
                kernel/src/arch/arm64/smp.c \
                kernel/src/arch/arm64/gic.c \
                kernel/src/arch/arm64/syscall_entry.c
ARM64_ASM_SRCS := kernel/src/arch/arm64/boot.S \
                  kernel/src/arch/arm64/vectors.S \
                  kernel/src/arch/arm64/switch.S

ARM64_C_OBJS := $(patsubst kernel/src/%.c,build/arm64/%.o,$(ARM64_C_SRCS))
ARM64_ASM_OBJS := $(patsubst kernel/src/%.S,build/arm64/%.o,$(ARM64_ASM_SRCS))
ARM64_OBJS := $(ARM64_C_OBJS) $(ARM64_ASM_OBJS)

.PHONY: arm64 arm64-run arm64-clean

# ARM64 C compilation
build/arm64/%.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CFLAGS) -c $< -o $@

# ARM64 assembly
build/arm64/%.o: kernel/src/%.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CFLAGS) -c $< -o $@

# ARM64 kernel ELF
$(ARM64_KERNEL): $(ARM64_OBJS)
	$(ARM64_LD) $(ARM64_LDFLAGS) $(ARM64_OBJS) -o $@

arm64: $(ARM64_KERNEL)
	@echo "ARM64 kernel built: $(ARM64_KERNEL)"

# Run ARM64 kernel in QEMU
arm64-run: $(ARM64_KERNEL)
	qemu-system-aarch64 \
		-M virt \
		-cpu cortex-a57 \
		-m 256M \
		-nographic \
		-kernel $(ARM64_KERNEL) \
		-no-reboot

arm64-clean:
	rm -rf build/arm64

# --- Clean ---

clean:
	rm -rf build $(ISO) qemu_log.txt
