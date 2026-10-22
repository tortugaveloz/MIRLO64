# n64rt.sys: the program of the core's data slot 3 (Assets/n64/common/n64rt.sys), the runtime that runs an N64 game's
# own ROM (a .z64 in slot 0) with the game's own libultra: loader/main.c plus the runtime's objects (n64rt.mk).
# It is built for the SoC as the bitstream has it (RSP, the CPU's monitor level), so these are fixed here: RSP=1 OWN=1.
#
#   (once, from the repository's root, the steps of README.md's "Building from source" up to the libraries, the
#    boot ROM and the bitstream's images -- that is, the first four commands)
#   make -C lang/n64rt MIPS_CC=... MIPS_BIN=...
#   -> lang/n64rt/build/n64rt.bin, which is released as n64rt.sys (cp build/n64rt.bin n64rt.sys)
#
#   GAME_MEM=<bytes>: the RDRAM the game sees (8 MiB by default). DEBUG=1, PADLOG=1 ...: n64rt.mk's switches.
MIRLO   ?= $(abspath ../..)
RSP     := 1
OWN     := 1
RT       = $(MIRLO)/lang/n64rt
PROGRAM  = n64rt
BUILD    = build
OBJECTS  = $(BUILD)/main.o $(N64RT_OBJECTS)
.DEFAULT_GOAL := all
include $(RT)/n64rt.mk
include $(MIRLO)/lang/mips/program.mk
CFLAGS  += -O2 -g -Wall -Wextra -Wno-unused-parameter -I$(RT) $(if $(GAME_MEM),-DGAME_MEM=$(GAME_MEM)u)
# (the ROM is at 1 MiB: the program and its boot stack stay below it)
MIPS_LDFLAGS += --defsym=_fstack=0x400FFFF0

all: $(BUILD)/$(PROGRAM).bin
	@s=$$(stat -c %s $(BUILD)/$(PROGRAM).bin); echo "$(BUILD)/$(PROGRAM).bin: $$s bytes"; test $$s -lt 983040

$(BUILD)/main.o: loader/main.c $(RT)/n64rt.h
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
