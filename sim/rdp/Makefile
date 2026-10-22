# rdp_task: the RSP model's commands through MRDP's model, to an image.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
# (the N64's commands: without MRDP_N64 the model is MRDP's, and draws none of a game's)
DEFS ?= -DMRDP_N64
# LOD=1: the texture LOD (docs/lod.md), as the RTL has it with its LOD parameter (build_rtl.sh takes the same switch)
LOD ?= 1
ifeq ($(LOD),1)
DEFS += -DRDP_LOD
endif
M = ../../lang/c/mrdp
SRC = rdp_task.c ../rsp/rsp.c ../rsp/rsp.h $(M)/mrdp.c $(M)/mrdp.h $(M)/mrdp_setup.h
rdp_task: $(SRC)
	$(CC) $(CFLAGS) $(DEFS) -I../rsp -I$(M) rdp_task.c ../rsp/rsp.c $(M)/mrdp.c -o $@
# the same with the model's texture LOD (docs/lod.md) whatever LOD says
rdp_task_lod: $(SRC)
	$(CC) $(CFLAGS) $(DEFS) -DRDP_LOD -I../rsp -I$(M) rdp_task.c ../rsp/rsp.c $(M)/mrdp.c -o $@
clean:
	rm -f rdp_task rdp_task_lod
.PHONY: clean
