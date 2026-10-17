# Native Ultrium programs (Ultrium OS component). Built as static ET_EXEC x86-64 ELF files in os/bin/.
# Invoked by the kernel build (which embeds the results) or directly: make -C os TOOLCHAIN=llvm
override .DEFAULT_GOAL := all
TOOLCHAIN ?= gcc
ifeq ($(TOOLCHAIN),llvm)
    CC := clang
    override TARGET_FLAGS := -target x86_64-unknown-none-elf -fuse-ld=lld
else
    CC := gcc
    override TARGET_FLAGS :=
endif
PROGRAMS := hello counter filedemo fpudemo spin
CFLAGS := -Os -g0 -std=gnu11 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -fno-asynchronous-unwind-tables -fno-builtin -fcf-protection=none -mno-red-zone
INCLUDES := -I runtime -I ../shared
LDFLAGS := -nostdlib -static -Wl,-T,link.ld -Wl,-z,max-page-size=1 -Wl,-z,noexecstack -Wl,--build-id=none -Wl,--gc-sections -Wl,--strip-all

.PHONY: all clean
all: $(addprefix bin/,$(addsuffix .elf,$(PROGRAMS)))

bin/%.elf: programs/%.c runtime/runtime.c runtime/ultrium.h link.ld GNUmakefile ../shared/abi/ultrium_syscall.h
	mkdir -p bin
	$(CC) $(TARGET_FLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) $< runtime/runtime.c -o $@

clean:
	rm -rf bin
