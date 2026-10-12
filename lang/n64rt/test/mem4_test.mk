# mem4_test: the "Expansion Pak" setting (mem4_test.c), as a game on the SoC in sim/mips_soc.
#
#   make -f mem4_test.mk [MIRLO=<checkout>] [BUILD=<dir>] [MIPS_CC=... MIPS_BIN=...]       -> $(BUILD)/mem4_test.z64
#   then, in sim/mips_soc, with the Pak in and with it out:
#     ROM=$(BUILD)/mem4_test.z64 [INTERACT=0x10000138:1] DUMP=0x43B00000,8 obj_soc/tb_soc <loader n64rt.bin> -c 60
#   The DUMP shows 0x600D600D at +0 when every check passed, the size found at +0x14 and 0xD0DE at +0x18.
MIRLO  ?= ../../..
BUILD  ?= build_mem4
include $(MIRLO)/lang/mips/mips.mk
GCFLAGS = $(MIPS_GAME_CFLAGS) -O2 -fno-strict-aliasing -ffreestanding $(EXTRA)
all: $(BUILD)/mem4_test.z64
$(BUILD)/mem4_test.o: mem4_test.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(GCFLAGS) -c $< -o $@
$(BUILD)/mem4_test.elf: $(BUILD)/mem4_test.o layout_game.ld
	$(MIPS_LD) -EB -T layout_game.ld --gc-sections -o $@ $<
$(BUILD)/mem4_test.bin: $(BUILD)/mem4_test.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(BUILD)/mem4_test.z64: $(BUILD)/mem4_test.bin
	python3 -c "import struct,sys; g=open(sys.argv[1],'rb').read(); h=struct.pack('>IIII',0x80371240,0x0F,0x80000400,0x1444)+bytes(16)+b'MEM4 TEST'.ljust(20); open(sys.argv[2],'wb').write((h.ljust(0x1000,b'\0')+g).ljust(0x101000,b'\0'))" $< $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
