# tlbstate_test: a save state of a game that uses the TLB (tlbstate_test.c), as a game on the SoC in sim/mips_soc.
#
#   make -f tlbstate_test.mk [MIRLO=<checkout>] [BUILD=<dir>] [MIPS_CC=... MIPS_BIN=...]       -> $(BUILD)/tlbstate_test.z64
#   then, in sim/mips_soc (a bench with the CPU's own TLB): the state is made at field 100, the game having switched its pages at 70
#     ROM=$(BUILD)/tlbstate_test.z64 SS_SAVE=100 SS_EXIT=1 obj_soc/tb_soc <loader n64rt.bin> -c 400 -o <dir>
#     ROM=$(BUILD)/tlbstate_test.z64 SS_LOAD=<dir>/state.bin SS_LOAD_AT=40 DUMP=0x43B00000,8 obj_soc/tb_soc <loader n64rt.bin> -c 400
#   The DUMP's word 0 is 0x600D0000 + the turns the game did, word 1 the failed checks (0), word 2 the phase (1).
MIRLO  ?= ../../..
BUILD  ?= build_tlbstate
include $(MIRLO)/lang/mips/mips.mk
GCFLAGS = $(MIPS_GAME_CFLAGS) -O2 -fno-strict-aliasing -ffreestanding $(EXTRA)
all: $(BUILD)/tlbstate_test.z64
$(BUILD)/tlbstate_test.o: tlbstate_test.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(GCFLAGS) -c $< -o $@
$(BUILD)/tlbstate_test.elf: $(BUILD)/tlbstate_test.o layout_game.ld
	$(MIPS_LD) -EB -T layout_game.ld --gc-sections -o $@ $<
$(BUILD)/tlbstate_test.bin: $(BUILD)/tlbstate_test.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(BUILD)/tlbstate_test.z64: $(BUILD)/tlbstate_test.bin
	python3 -c "import struct,sys; g=open(sys.argv[1],'rb').read(); h=struct.pack('>IIII',0x80371240,0x0F,0x80000400,0x1444)+bytes(16)+b'TLB STATE'.ljust(20); open(sys.argv[2],'wb').write((h.ljust(0x1000,b'\0')+g).ljust(0x101000,b'\0'))" $< $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
