# rdp_task: the RSP model's commands through RDP's model, to an image.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
# (the N64's commands: without RDP_N64 the model is RDP's, and draws none of a game's)
DEFS ?= -DRDP_N64
# EXACT=1: the console's rules the RTL does not follow yet (docs/rdp_exact.md), its LOD among them
EXACT ?= 0
ifeq ($(EXACT),1)
DEFS += -DRDP_EXACT
endif
M = ../../lang/c/rdp
SRC = rdp_task.c ../rsp/rsp.c ../rsp/rsp.h $(M)/rdp.c $(M)/rdp.h $(M)/rdp_setup.h
rdp_task: $(SRC)
	$(CC) $(CFLAGS) $(DEFS) -I../rsp -I$(M) rdp_task.c ../rsp/rsp.c $(M)/rdp.c -o $@
clean:
	rm -f rdp_task
.PHONY: clean
