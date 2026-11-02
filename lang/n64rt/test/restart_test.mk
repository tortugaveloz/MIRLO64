# restart_test: a game that reads its Transfer Pak's status once, a moment after it starts (restart_test.c), for the core's "Restart Game"
# action, as a game on the SoC in sim/mips_soc.
#
#   make -f restart_test.mk [MIRLO=<checkout>] [BUILD=<dir>] [MIPS_CC=... MIPS_BIN=...]       -> $(BUILD)/restart_test.z64
#   sim/mips_soc/restart_check.sh runs it with the runtime, the Game Boy ROM and the action (the commands are in its header).
MIRLO  ?= ../../..
BUILD  ?= build_restart
include $(MIRLO)/lang/mips/mips.mk
GCFLAGS = $(MIPS_GAME_CFLAGS) -O2 -fno-strict-aliasing -ffreestanding $(EXTRA)
all: $(BUILD)/restart_test.z64
$(BUILD)/restart_test.o: restart_test.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(GCFLAGS) -c $< -o $@
$(BUILD)/restart_test.elf: $(BUILD)/restart_test.o layout_game.ld
	$(MIPS_LD) -EB -T layout_game.ld --gc-sections -o $@ $<
$(BUILD)/restart_test.bin: $(BUILD)/restart_test.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(BUILD)/restart_test.z64: $(BUILD)/restart_test.bin
	python3 -c "import struct,sys; g=open(sys.argv[1],'rb').read(); h=struct.pack('>IIII',0x80371240,0x0F,0x80000400,0x1444)+bytes(16)+b'RESTART TEST'.ljust(20); open(sys.argv[2],'wb').write((h.ljust(0x1000,b'\0')+g).ljust(0x101000,b'\0'))" $< $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
