# Builds the emulator. Natively by default (for development and differential
# testing); for Linux/Wasm pass CC=tools/wasm-cc and AR=llvm-ar.
# Out-of-tree, so the same sources can be built from a shared mount.

O ?= build
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
WARN := -std=gnu11 -Wall -Wextra -Wshadow
# Square roots are the host's instruction, not a call that might set errno, and
# no two floating-point operations are ever made into one (fp.c).
FP := -fno-math-errno -ffp-contract=off

# Berkeley SoftFloat, built as a library so only what fp.c uses gets linked.
SF := ../third_party/berkeley-softfloat-3
include softfloat.mk
SF_INC := -I$(SF)/build/Linux-RISCV64-GCC -I$(SF)/source/RISCV -I$(SF)/source/include
SF_DEFS := -DSOFTFLOAT_FAST_INT64 -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 \
	   -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32
# SoftFloat's rounding mode and exception flags are variables: one set per
# thread of ours, since the guest's threads compute at the same time.
SF_TLS := -DTHREAD_LOCAL=_Thread_local
SF_OBJS := $(SF_NAMES:%=$(O)/sf/%.o)
SF_LIB := $(O)/libsoftfloat.a
vpath %.c $(SF)/source $(SF)/source/RISCV

SRCS := main.c interp.c bitmanip.c compressed.c fp.c vector.c elf.c fork.c heapcheck.c identity.c jit.c mm.c seccomp.c signal.c syscall.c thread.c virtgpu.c
OBJS := $(SRCS:%.c=$(O)/%.o)

$(O)/rv: $(OBJS) $(SF_LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread -o $@ $(OBJS) $(SF_LIB)

$(O)/%.o: %.c rv.h wasm_emit.h virtgpu_formats.h | $(O)
	$(CC) $(CFLAGS) -pthread $(WARN) $(FP) $(SF_TLS) $(SF_INC) -c -o $@ $<

$(SF_LIB): $(SF_OBJS)
	rm -f $@
	$(AR) crs $@ $(SF_OBJS)

$(O)/sf/%.o: %.c | $(O)/sf
	$(CC) $(CFLAGS) -w $(SF_DEFS) $(SF_TLS) $(SF_INC) -c -o $@ $<

$(O) $(O)/sf:
	mkdir -p $@

clean:
	rm -rf $(O)

.PHONY: clean
