# rdp_task: the RSP model's commands through MRDP's model, to an image.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
# (the N64's commands: without MRDP_N64 the model is MRDP's, and draws none of a game's)
DEFS ?= -DMRDP_N64
# EXACT=1: the console's rules the RTL does not follow yet (docs/rdp_exact.md), its LOD among them
EXACT ?= 0
ifeq ($(EXACT),1)
DEFS += -DRDP_EXACT
endif
M = ../../lang/c/mrdp
SRC = rdp_task.c ../rsp/rsp.c ../rsp/rsp.h $(M)/mrdp.c $(M)/mrdp.h $(M)/mrdp_setup.h
rdp_task: $(SRC)
	$(CC) $(CFLAGS) $(DEFS) -I../rsp -I$(M) rdp_task.c ../rsp/rsp.c $(M)/mrdp.c -o $@
clean:
	rm -f rdp_task
.PHONY: clean
