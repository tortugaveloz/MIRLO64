# The accessory (../n64rt_pak.c) under libultra's own Controller Pak file
# system and Transfer Pak functions, on the SoC in sim/mips_soc. libultra is
# not in this repository: ULTRALIB is a decompilation's ultralib checkout
# (its src/io and include), built here as its VERSION (7: 2.0I with patches).
#
#   make ULTRALIB=<...>/lib/ultralib
#   python3 make_gb.py $(ULTRALIB) /tmp/test.gb     a Game Boy ROM for the test (with the
#                                           logo libultra checks for: keep it out of here)
#   ROM=<any .z64> <sim/mips_soc>/tb_soc build/pak_test.bin -c 150 +gbcart=/tmp/test.gb
# (+gbcart: the ROM as the cartridge in the Pocket's slot, on a SoC built with
# MIRLO_GB_CART; without it the slot is empty. ROM= makes the test the program
# of data.json's runtime slot, as sim/mips_soc/tb_soc.cpp says)
MIRLO    ?= ../../..
VERSION  ?= 7
PROGRAM   = pak_test
BUILD     = build
ULT_SRC   = gbpakinit gbpakpower gbpakgetstatus gbpakreadid gbpakreadwrite gbpaksetbank gbpakcheckconnector \
            pfsinitpak pfschecker contpfs pfsallocatefile pfsreadwritefile pfssearchfile pfsdeletefile \
            pfsfreeblocks pfsfilestate pfsselectbank
OBJECTS   = $(BUILD)/pak_test.o $(BUILD)/pak_glue.o $(BUILD)/n64rt_pak.o $(addprefix $(BUILD)/ult_,$(addsuffix .o,$(ULT_SRC)))
.DEFAULT_GOAL := all
include $(MIRLO)/lang/mips/program.mk
CFLAGS   += -O2 -Wall -fno-strict-aliasing
ULT_INC   = -I$(ULTRALIB)/include -I$(ULTRALIB)/include/PR -I$(ULTRALIB)/include/PRinternal -I$(ULTRALIB)/src -I$(ULTRALIB)/src/io \
            -DBUILD_VERSION=$(VERSION) -DBUILD_VERSION_STRING=\"2.0\" -D_LANGUAGE_C -D_FINALROM -DNDEBUG -DF3DEX_GBI_2
all: $(BUILD)/$(PROGRAM).bin
$(BUILD)/ult_%.o: $(ULTRALIB)/src/io/%.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -w $(ULT_INC) -c $< -o $@
$(BUILD)/pak_test.o: pak_test.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) $(ULT_INC) -c $< -o $@
$(BUILD)/pak_glue.o: pak_glue.c ../n64rt.h
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -I.. -c $< -o $@
$(BUILD)/n64rt_pak.o: ../n64rt_pak.c ../n64rt.h pak_force.h $(MIRLO)/lang/mips/include/layout.h
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -I.. -DN64RT_DEBUG -include pak_force.h -c $< -o $@
clean:
	rm -rf $(BUILD)
.PHONY: all clean
.SECONDARY:
