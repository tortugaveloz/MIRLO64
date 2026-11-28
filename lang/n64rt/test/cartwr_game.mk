# cartwr_game: a game that stores to the cartridge bus (cartwr_game.S), as a game on the SoC in sim/mips_soc (sim/mips_soc/cartwr_check.sh).
#   make -f cartwr_game.mk [BUILD=<dir>] [MIPS_CC=... MIPS_BIN=...]       -> $(BUILD)/cartwr_game.z64
MIRLO  ?= ../../..
BUILD  ?= build_cartwr
include $(MIRLO)/lang/mips/mips.mk
all: $(BUILD)/cartwr_game.z64
$(BUILD)/cartwr_game.o: cartwr_game.S
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(MIPS_GAME_CFLAGS) -c $< -o $@
$(BUILD)/cartwr_game.elf: $(BUILD)/cartwr_game.o layout_game.ld
	$(MIPS_LD) -EB -T layout_game.ld --gc-sections -o $@ $<
$(BUILD)/cartwr_game.bin: $(BUILD)/cartwr_game.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(BUILD)/cartwr_game.z64: $(BUILD)/cartwr_game.bin
	python3 -c "import struct,sys; g=open(sys.argv[1],'rb').read(); h=struct.pack('>IIII',0x80371240,0x0F,0x80000400,0x1444)+bytes(16)+b'CARTWR TEST'.ljust(20); open(sys.argv[2],'wb').write((h.ljust(0x1000,b'\0')+g).ljust(0x101000,b'\0'))" $< $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
