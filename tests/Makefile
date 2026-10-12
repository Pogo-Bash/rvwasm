# Guest test programs (riscv64) and the differential test run.
#
# Freestanding: no libc, and no linker relaxation (nothing sets up gp in these
# programs). Each test is built for the smallest ISA that covers it, so that a
# failure points at one extension.

O ?= build
RV ?= $(O)/../rv
CROSS ?= riscv64-linux-gnu-
GUEST_CFLAGS := -static -nostdlib -ffreestanding -mno-relax \
		-fno-tree-loop-distribute-patterns -fno-stack-protector -Wall

IM   := -march=rv64im -mabi=lp64
IMC  := -march=rv64imc -mabi=lp64
IMA  := -march=rv64ima -mabi=lp64
IMFD := -march=rv64imfd -mabi=lp64d
GC   := -march=rv64gc -mabi=lp64d
GCB  := -march=rv64gc_zba_zbb_zbs -mabi=lp64d

TESTS := $(O)/arith-O0 $(O)/arith-O1 $(O)/arith-O2 $(O)/mm \
	 $(O)/arith-c-O2 $(O)/arith-c-Os $(O)/mm-c \
	 $(O)/atomic $(O)/fp $(O)/fp-gc $(O)/atomic-gc $(O)/signal $(O)/signal-gc $(O)/codegen \
	 $(O)/bitmanip

# Not compared instruction by instruction (too long for that): by output, for the JIT (tools/bitmanip-check.sh).
EXTRA := $(O)/bitmanip-loop $(O)/hwprobe

all: $(TESTS) $(EXTRA)

$(O)/arith-O%: arith.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IM) -O$* -o $@ $<

$(O)/arith-c-O%: arith.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IMC) -O$* -o $@ $<

$(O)/mm: mm.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IM) -O1 -o $@ $<

$(O)/mm-c: mm.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IMC) -Os -o $@ $<

$(O)/atomic: atomic.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IMA) -O1 -o $@ $<

$(O)/atomic-gc: atomic.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(GC) -O2 -o $@ $<

$(O)/fp: fp.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IMFD) -O1 -o $@ $<

$(O)/fp-gc: fp.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(GC) -O2 -o $@ $<

$(O)/codegen: codegen.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IM) -O1 -o $@ $<

$(O)/bitmanip: bitmanip.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(GCB) -O1 -o $@ $<

$(O)/bitmanip-loop: bitmanip.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(GCB) -O1 -DREPEAT=300 -o $@ $<

$(O)/hwprobe: hwprobe.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(GC) -O1 -o $@ $<

$(O)/signal: signal.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(IM) -O1 -o $@ $<

$(O)/signal-gc: signal.c guest.h | $(O)
	$(CROSS)gcc $(GUEST_CFLAGS) $(GC) -O2 -o $@ $<

$(O):
	mkdir -p $(O)

# Each run writes a large trace, so tests run one at a time. T=name runs one.
#
# The signal tests are compared by output only: a signal frame's size is not
# part of the ABI (QEMU's is smaller than the kernel's), so stack addresses
# inside handlers differ between correct implementations.
check: $(if $(T),$(O)/$(T),$(TESTS))
	@fail=0; for t in $^; do \
		case $$t in */signal*) mode=--output-only;; *) mode=;; esac; \
		python3 ../tools/difftest.py $$mode --rv $(RV) $$t || fail=1; \
	done; exit $$fail

clean:
	rm -rf $(O)

.PHONY: all check clean
