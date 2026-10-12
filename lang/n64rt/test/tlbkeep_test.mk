# tlbkeep_test: the TLB entries the runtime keeps when a tlbwr puts them out (tlbkeep_test.c), as a game on the SoC in sim/mips_soc.
#
#   make -f tlbkeep_test.mk [MIRLO=<checkout>] [BUILD=<dir>] [MIPS_CC=... MIPS_BIN=...]       -> $(BUILD)/tlbkeep_test.z64
#   (a ROM the OWN loader runs as it is); then, in sim/mips_soc:
#     ROM=$(BUILD)/tlbkeep_test.z64 DUMP=0x43B00000,8 obj_soc/tb_soc <loader n64rt.bin> -c 60
#   The DUMP shows 0x600D600D at +0 when every check passed (0xBADBAD00 + the failed ones, the first of them at +8) and 0xD0DE at +0x18.
MIRLO  ?= ../../..
BUILD  ?= build_tlbkeep
include $(MIRLO)/lang/mips/mips.mk
GCFLAGS = $(MIPS_GAME_CFLAGS) -O2 -fno-strict-aliasing -ffreestanding $(EXTRA)
all: $(BUILD)/tlbkeep_test.z64
$(BUILD)/tlbkeep_test.o: tlbkeep_test.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(GCFLAGS) -c $< -o $@
$(BUILD)/tlbkeep_test.elf: $(BUILD)/tlbkeep_test.o layout_game.ld
	$(MIPS_LD) -EB -T layout_game.ld --gc-sections -o $@ $<
$(BUILD)/tlbkeep_test.bin: $(BUILD)/tlbkeep_test.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(BUILD)/tlbkeep_test.z64: $(BUILD)/tlbkeep_test.bin
	python3 -c "import struct,sys; g=open(sys.argv[1],'rb').read(); h=struct.pack('>IIII',0x80371240,0x0F,0x80000400,0x1444)+bytes(16)+b'TLB KEEP'.ljust(20); open(sys.argv[2],'wb').write((h.ljust(0x1000,b'\0')+g).ljust(0x101000,b'\0'))" $< $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
