# il_test: a serrated hi-res picture driven as libultra drives one (il_test.c), as a game on the SoC in sim/mips_soc or a Pocket.
#
#   make -f il_test.mk [MIRLO=<checkout>] [BUILD=<dir>] [EXTRA="-DROWCODE=1 -DPROG_EVERY=600"]       -> $(BUILD)/il_test.z64
MIRLO  ?= ../../..
BUILD  ?= build_il
include $(MIRLO)/lang/mips/mips.mk
GCFLAGS = $(MIPS_GAME_CFLAGS) -O2 -fno-strict-aliasing -ffreestanding $(EXTRA)
all: $(BUILD)/il_test.z64
$(BUILD)/il_test.o: il_test.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(GCFLAGS) -c $< -o $@
$(BUILD)/il_test.elf: $(BUILD)/il_test.o layout_game.ld
	$(MIPS_LD) -EB -T layout_game.ld --gc-sections -o $@ $<
$(BUILD)/il_test.bin: $(BUILD)/il_test.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(BUILD)/il_test.z64: $(BUILD)/il_test.bin
	python3 -c "import struct,sys; g=open(sys.argv[1],'rb').read(); h=struct.pack('>IIII',0x80371240,0x0F,0x80000400,0x1444)+bytes(16)+b'IL TEST'.ljust(20); open(sys.argv[2],'wb').write((h.ljust(0x1000,b'\0')+g).ljust(0x101000,b'\0'))" $< $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
