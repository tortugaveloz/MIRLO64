# n64rt's part of a port's build (include it before lang/mips/program.mk):
# the runtime's objects and, without RSP=1 (the earlier mode, which nothing in
# this repository builds any more), the geom core's firmware, built here from
# MIRLO's sources with this runtime's display-list translator in it. That mode's
# audio path (the audio core's interpreter of the audio microcode) is no longer
# in the tree.
#
# Needs MIRLO, BUILD. RAWCC=0: the combiners approximated rather than written
# to RDP as the game sets them. RSPDUMP=1: each RSP task marked for
# sim/mips_soc's RSPDUMP (the RSP reference model's inputs).
RT        ?= $(MIRLO)/lang/n64rt
RAWCC     ?= 1
N64RT_F3D := -DF3D_N64RAM $(if $(filter 1,$(RAWCC)),-DF3D_RAW_CC) -DF3D_TRACE=0
# SI_GAME=1: the SI's functions (controllers, accessories, EEPROM) are left as
# the game has them, on the SI's registers as the runtime emulates them
# (n64rt_io.c); SI_GAME=2: likewise, but for __osSiRawStartDma, replaced where
# it is found; without it they are all replaced like the rest. STATS=1: a
# debugging build that counts what the game asks of the runtime.
# OWN=1: the game's whole libultra runs as it is (no function replaced, none of its
# threads, timers or interrupts the runtime's): the CPU's monitor level (MIRLO_MON)
# takes the RCP's registers and the hardware's events (n64rt_exc.S, n64rt_io.c);
# needs RSP=1 and the SoC built with MIRLO_MON.
# RSP=1: for the SoC built with MIRLO_RSP -- the game's tasks on its own
# microcode (n64rt_rsp.c): no translator, no geom or audio firmware.
ifeq ($(RSP),1)
N64RT_OBJECTS = $(addprefix $(BUILD)/,n64rt_boot.o n64rt_os.o n64rt_io.o n64rt_pak.o n64rt_exc.o n64rt_rsp.o n64rt_savedb.o)
CFLAGS += -DN64RT_RSP
else
N64RT_OBJECTS = $(addprefix $(BUILD)/,n64rt_boot.o n64rt_os.o n64rt_io.o n64rt_pak.o n64rt_exc.o n64rt_gfx.o n64rt_savedb.o \
                f3d_emit.o geom_load.o)
endif
CFLAGS += -I$(RT) -I$(MIRLO)/lang/c/game -I$(MIRLO)/lang/c/geom -I$(MIRLO)/lang/c/mips \
          -fno-strict-aliasing $(N64RT_F3D) $(if $(DEBUG),-DN64RT_DEBUG) \
          $(if $(RSPDUMP),-DN64RT_RSPDUMP) $(if $(PAK_FORCE),-DN64RT_PAK_FORCE=$(PAK_FORCE)u) \
          $(if $(SI_GAME),-DN64RT_SI_GAME=$(SI_GAME)) $(if $(filter 1,$(STATS)),-DN64RT_STATS) \
          $(if $(filter 1,$(OWN)),-DN64RT_OWN -DN64RT_SI_GAME=1) $(if $(TLB_FATAL),-DN64RT_TLB_FATAL) $(if $(EEP16K),-DN64RT_EEP16K) $(if $(PADLOG),-DN64RT_PADLOG) $(if $(TLBPROF),-DN64RT_TLBPROF) $(if $(TLBKEEP_CHECK),-DN64RT_TLBKEEP_CHECK=$(TLBKEEP_CHECK)u) $(if $(NO_TLBKEEP),-DN64RT_NO_TLBKEEP)

GEOM_FW_H  := $(abspath $(BUILD))/geom/geom_fw.h
# (no display-list cache on the geom core: an N64 game's lists are rebuilt
# every frame, and its RAM has no room for the tables)
$(GEOM_FW_H): $(wildcard $(MIRLO)/lang/c/geom/*.c $(MIRLO)/lang/c/geom/*.h $(MIRLO)/lang/c/geom/*.ld \
                         $(MIRLO)/lang/c/game/gdl_build.h $(RT)/f3d_emit.c $(RT)/f3d_emit.h)
	$(MAKE) -C $(MIRLO)/lang/c/geom B=$(abspath $(BUILD))/geom F3D_EMIT=$(abspath $(RT))/f3d_emit.c \
		'CFLAGS_EXTRA=$(N64RT_F3D) -DF3D_NO_DLC -I$(abspath $(RT))'
$(BUILD)/n64rt_gfx.o: $(GEOM_FW_H)
$(BUILD)/n64rt_gfx.o: CFLAGS += -DGEOM_FW_HEADER='"$(GEOM_FW_H)"'
$(BUILD)/%.o: $(MIRLO)/lang/c/game/%.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.c $(RT)/n64rt.h $(RT)/n64rt_savedb.h $(MIRLO)/lang/mips/include/layout.h
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: $(RT)/%.S
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
