# Game-CPU programs that draw through the geometry core and RDP: frame.c
# (framebuffers, GDL submission, flips) + one program, for the MIPS game CPU
# (lang/mips/program.mk; libraries: make -f ../../mips/lib.mk VARIANT=game).
#   make                  the spinning cube (cube_main.c)
#   make PROG=demo        the spinning triangle (demo_main.c)
#   make PROG=videotest   the scan-out test (videotest_main.c)

PROG ?= cube

# lang/mips: the toolchain, picolibc, compiler-rt, crt0 and linker files
BUILD    = build
PROGRAM  = build
OBJECTS  = $(addprefix $(BUILD)/,log.o frame.o geom_load.o $(PROG)_main.o $(OBJECTS_$(PROG)))
all: $(BUILD)/build.bin
include ../../mips/program.mk
CFLAGS  += -O2 -Wall -fno-strict-aliasing -I. -I../geom $(EXTRA_CFLAGS)
# the geom core's firmware, loaded by frame_init() (geom_load.h)
GEOM_FW_H ?= ../geom/build/geom_fw.h
CFLAGS  += -DGEOM_FW_HEADER='"$(abspath $(GEOM_FW_H))"'
$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
clean:
	rm -rf build
.PHONY: all clean
