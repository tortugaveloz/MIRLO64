# Geometry-core firmware for mips_geom (rtl/mips/mips_geom.sv): MIPS32's
# integer subset with a divider and clz, NO FPU (the real N64 RSP had none
# either, only the main CPU did -- see geom_fixed.h's header). Every fp32 op
# here is pure-integer S15.16 fixed point underneath (geom_fixed.c,
# geom_vecmath.c); `float` stays the C-level type at call sites. The little
# soft-float there is comes from compiler-rt (lang/mips/build/lite/libcrt.a),
# in ROM A. The CFU instructions: geom_vpar.h / geom_fixed.h (lang/c/mips/
# mips_cfu.h); the D-cache flush: geom_dcache.h.
#
# Each game loads its own build of this firmware into the geom core's ROMs
# at boot (lang/c/game/geom_load.c, from $(B)/geom_fw.h), the way the audio
# core is loaded: nothing of it is in the bitstream. A game builds it into its
# own directory: make -C <this> B=<dir> F3D_EMIT=<its translator>
# [CFLAGS_EXTRA=...]. The register addresses come from
# lang/mips/include/generated/csr.h (tools/mirlo_regs.py).
#
# Needs: make -f ../../mips/lib.mk VARIANT=lite (picolibc's headers, compiler-rt).

include ../../mips/mips.mk
B        ?= build
CC        = $(MIPS_GCC)
OBJCOPY   = $(MIPS_OBJCOPY_LITE)
GEN_CSR   = ../../mips/include/generated/csr.h
CRT_LIB   = ../../mips/build/lite/libcrt.a

# -fno-jump-tables: a jump table is a D-cache read from the ROM, and with it
# the same frames took ~30 % longer (sim/geom_full). -O2 throughout: ~15 %
# fewer cycles than -Os.
CFLAGS    = $(MIPS_LITE_CFLAGS) -I../mips -fsigned-char -O2 -g -fno-jump-tables -ffreestanding -nostdlib -fno-pic \
            -fno-strict-aliasing -Wall -Wextra -ffunction-sections -fdata-sections -std=gnu11 \
            -DGEOM_N64_ORDER -I$(B) $(CFLAGS_EXTRA)

OBJS = $(B)/start_mips.o $(B)/nolibc.o $(B)/main.o \
       $(B)/geom_pipeline.o $(B)/geom_triangle.o \
       $(B)/geom_fixed.o $(B)/geom_vecmath.o $(B)/geom_rdp_hw.o

# F3D_EMIT=<path to an f3d_emit.c>: the geom core processes F3DEX2 display
# lists itself (GDL_F3D, geom_f3d.c) with that translator (its f3d_emit.h next
# to it): no divide instruction here (GDL_NO_DIVIDE), its big tables in SDRAM
# (.sdram_bss), its full-screen background fills drawn where they come
# (F3D_ON_GEOM: the game CPU's frame clear cannot learn the colour).
ifneq ($(F3D_EMIT),)
F3D_DIR  := $(dir $(F3D_EMIT))
CFLAGS   += -I. -I$(F3D_DIR) -I../game -I../rdp -DGEOM_F3D -DGDL_NO_DIVIDE -DF3D_ON_GEOM \
            -DF3D_BIG='__attribute__((section(".sdram_bss")))'
OBJS     += $(B)/f3d_emit.o $(B)/geom_f3d.o
$(B)/f3d_emit.o: $(F3D_EMIT) $(B)/generated_csr_addrs.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
endif

.DEFAULT_GOAL := all
all: $(B)/geom.bin $(B)/geom.init $(B)/geom_fw.h

# ---- host tests: the fixed-point maths and the VPU C model ----
HOSTCC ?= cc
test/test_geom_fixed: test/test_geom_fixed.c geom_fixed.c geom_fixed.h
	$(HOSTCC) -O2 -Wall -Wextra -DGEOM_HOST_TEST -I. test/test_geom_fixed.c geom_fixed.c -lm -o $@

test/test_vpar: test/test_vpar.c geom_vpar.h geom_fixed.c
	$(HOSTCC) -O2 -Wall -Wextra -DGEOM_HOST_TEST -I. test/test_vpar.c geom_fixed.c -lm -o $@

hosttest: test/test_geom_fixed test/test_vpar
	./test/test_geom_fixed
	./test/test_vpar

$(B):
	mkdir -p $(B)

# Extract just the register addresses / field offsets this firmware needs.
$(B)/generated_csr_addrs.h: $(GEN_CSR) | $(B)
	@echo "/* auto-extracted from $(GEN_CSR) -- do not edit */" > $@
	@echo "#ifndef GEOM_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define GEOM_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define CSR_BASE 0xf0000000L" >> $@
	@grep -E '#define CSR_(MAILBOX|RDP)_' $(GEN_CSR) >> $@
	@echo "#endif" >> $@

$(B)/%.o: %.S $(B)/generated_csr_addrs.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/%.o: %.c $(B)/generated_csr_addrs.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/geom.elf: $(OBJS) geom_mips.ld $(CRT_LIB)
	$(MIPS_LD) -EB --gc-sections -Map=$(B)/geom.map -T geom_mips.ld -o $@ $(OBJS) $(CRT_LIB)
	python3 ../mips/check_isa.py --clz $(MIPS_BIN)objcopy $@ .text .text_b || { rm -f $@; false; }

$(B)/geom.bin: $(B)/geom.elf
	$(OBJCOPY) -O binary $< $@

# The two ROMs as C arrays of (big-endian) words, for the game's loader:
# ROM A the image's first 16 KiB, ROM B what lies from 0x1_0000 on.
$(B)/geom_fw.h: $(B)/geom.elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $(B)/geom_rom_a.bin
	$(OBJCOPY) -O binary -j .text_b $< $(B)/geom_rom_b.bin
	python3 ../../../tools/bin2words.py --big geom_rom_a $(B)/geom_rom_a.bin geom_rom_b $(B)/geom_rom_b.bin > $@

# 32-bit-word hex, one per line -- ready for $readmemh.
$(B)/geom.init: $(B)/geom.bin
	od -An -tx4 -v --endian=big $< | tr -s ' ' '\n' | sed '/^$$/d' > $@

clean:
	rm -rf $(B) test/test_geom

.PHONY: all clean hosttest

# geom_triangle.c at -O3: its fixed-count loops (3 vertices, 3 channels) unrolled
# -- sim/geom_full -3 % (JRB) / -4 % (Bowser), +264 bytes of ROM
$(B)/geom_triangle.o: CFLAGS := $(subst -O2,-O3,$(CFLAGS))
