# snac_game: a game that talks to the PIF with the SI's DMA (snac_game.c) for the SNAC port's real controllers and the virtual channels, as a game on
# the SoC in sim/mips_soc (sim/mips_soc/snac_check.sh). 
#   make -f snac_game.mk RUN=A|B|C|D [BUILD=<dir>] [MIPS_CC=... MIPS_BIN=...]       -> $(BUILD)/snac_game.z64
MIRLO  ?= ../../..
RUN    ?= B
BUILD  ?= build_snac_$(RUN)
include $(MIRLO)/lang/mips/mips.mk
GCFLAGS = $(MIPS_GAME_CFLAGS) -O2 -fno-strict-aliasing -ffreestanding -DSNAC_RUN="'$(RUN)'" $(EXTRA)
all: $(BUILD)/snac_game.z64
$(BUILD)/snac_game.o: snac_game.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(GCFLAGS) -c $< -o $@
$(BUILD)/snac_game.elf: $(BUILD)/snac_game.o layout_game.ld
	$(MIPS_LD) -EB -T layout_game.ld --gc-sections -o $@ $<
$(BUILD)/snac_game.bin: $(BUILD)/snac_game.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(BUILD)/snac_game.z64: $(BUILD)/snac_game.bin
	python3 -c "import struct,sys; g=open(sys.argv[1],'rb').read(); h=struct.pack('>IIII',0x80371240,0x0F,0x80000400,0x1444)+bytes(16)+b'SNAC TEST'.ljust(20); open(sys.argv[2],'wb').write((h.ljust(0x1000,b'\0')+g).ljust(0x101000,b'\0'))" $< $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
