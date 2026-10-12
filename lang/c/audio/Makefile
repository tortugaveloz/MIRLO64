# Audio-core firmware for mips_lite (rtl/mips/mips_lite.sv: MIPS32's integer
# subset, no FPU, no divider, no caches). Self-contained like lang/c/geom:
# -nostdlib, so a stray float op or divide fails to LINK. The CFU
# instructions: audio_cfu.h (lang/c/mips/mips_cfu.h).
#
# Unlike the geom core's ROM, nothing here is baked into the bitstream: the
# game CPU loads build/$(PROG)/audio_imem.bin + audio_dmem.bin into the core's
# IMEM/DMEM and releases its reset (lang/c/game/audio_load.c). Game firmware
# embeds them through build/$(PROG)/audio_fw.h. So an audio firmware change
# needs no Quartus run -- rebuild this, then the game firmware that embeds it.
#
# The register addresses (the APF audio FIFO) come from
# lang/mips/include/generated/csr.h (tools/mirlo_regs.py).

include ../../mips/mips.mk
GEN_CSR   = ../../mips/include/generated/csr.h
CC        = $(MIPS_GCC)
OBJCOPY   = $(MIPS_OBJCOPY_LITE)
SIZE      = $(MIPS_BIN)size
# (no store merging, no memset/memcpy calls made of loops: -nostdlib)
ARCHFLAGS = $(MIPS_LITE_CFLAGS) -fno-store-merging -fno-tree-loop-distribute-patterns -I../mips
CFLAGS    = $(ARCHFLAGS) -O2 -g -ffreestanding -nostdlib -fsigned-char \
            -fno-strict-aliasing -Wall -Wextra -ffunction-sections -fdata-sections -std=gnu11 \
            -I$(GEN) $(CFLAGS_EXTRA)

# PROG=main: the bring-up program -- self-tests, benchmarks, then a 440 Hz
# tone (lang/c/game/tone_main.c). It builds into build/$(PROG)/.
PROG ?= main
B     = build/$(PROG)
GEN   = build
LINK  = $(MIPS_LD) -EB --gc-sections -Map=$(B)/audio.map -T audio_mips.ld
START = start_mips
OBJS  = $(B)/$(START).o $(B)/$(PROG).o $(B)/nolibc.o

all: $(B)/audio_imem.bin $(B)/audio_imem2.bin $(B)/audio_dmem.bin $(B)/audio_fw.h

$(GEN) $(B):
	mkdir -p $@

$(GEN)/generated_csr_addrs.h: $(GEN_CSR) | $(GEN)
	@echo "/* auto-extracted from $(GEN_CSR) -- do not edit */" > $@
	@echo "#ifndef AUDIO_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define AUDIO_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define CSR_BASE 0xf0000000L" >> $@
	@grep -E '#define CSR_APF_AUDIO_' $(GEN_CSR) >> $@
	@grep -E '#define CONFIG_CLOCK_FREQUENCY' $(dir $(GEN_CSR))soc.h >> $@
	@echo "#endif" >> $@

$(B)/%.o: %.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# IMEM is 8 KiB (+ 8 KiB IMEM2): the libc helpers are -Os.
$(B)/nolibc.o: CFLAGS := $(subst -O2,-Os,$(CFLAGS))
$(B)/%.o: %.c $(GEN)/generated_csr_addrs.h audio_hw.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/audio.elf: $(OBJS) audio_mips.ld
	$(LINK) -o $@ $(OBJS)
	python3 ../mips/check_isa.py $(MIPS_BIN)objcopy $@ .text .text2 || { rm -f $@; false; }
	$(SIZE) $@

$(B)/audio_imem.bin: $(B)/audio.elf
	$(OBJCOPY) -O binary -j .text $< $@

# the second IMEM bank (0x8000_6000)
$(B)/audio_imem2.bin: $(B)/audio.elf
	$(OBJCOPY) -O binary -j .text2 $< $@

$(B)/audio_dmem.bin: $(B)/audio.elf
	$(OBJCOPY) -O binary -j .rodata -j .data $< $@

# The images as C arrays of (big-endian) words, for the game firmware.
$(B)/audio_fw.h: $(B)/audio_imem.bin $(B)/audio_imem2.bin $(B)/audio_dmem.bin
	python3 ../../../tools/bin2words.py --big audio_imem $(B)/audio_imem.bin audio_imem2 $(B)/audio_imem2.bin audio_dmem $(B)/audio_dmem.bin > $@

clean:
	rm -rf build

.PHONY: all clean
