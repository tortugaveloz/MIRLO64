# rdpo: the model against the oracle (README.md). Run ./setup.sh first.
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-function
ROOT := $(abspath ../..)
# MODEL_DIR=<dir>: another copy of the model; ORACLE_DIR: where setup.sh put the oracle
MODEL_DIR ?= $(ROOT)/lang/c/mrdp
ORACLE_DIR ?= $(ROOT)/build/oracle/src
M := $(abspath $(MODEL_DIR))
ORC := $(abspath $(ORACLE_DIR))/src/core
B := build
# the oracle's source: its own warnings are not ours
ORC_CFLAGS := -O2 -g -w -I$(ORC)

all: $(B)/rdpo $(B)/rdpo_exact $(B)/rdpgen

$(B):
	mkdir -p $(B)

# The model with its LOD hooks: a copy of mrdp.c with two calls added (lod_pixel's inputs, draw_pixel_cvg's result).
# The copy is checked to have taken both, so that a change to the model is noticed here.
define hook
	sed -e 's|^\(            const mrdp_lod_t L = lod_pixel(r, pr, at);\)$$|\1 mdl_lod_hook(x, y, \&L);|' \
	    -e 's|^    \(return mrdp_lod(r->other_h, r->min_level, pr->level, (unsigned)pr->tile, s, t, oob);\)$$|    mdl_lod_inputs(s, t, oob); \1|' \
	    $(1) > $(2).tmp
	test `grep -c 'mdl_lod_hook(x, y\|mdl_lod_inputs(s, t, oob)' $(2).tmp` = 2
	( printf '#include "mrdp.h"\nvoid mdl_lod_inputs(const int32_t *s, const int32_t *t, int oob);\n'; \
	  printf 'void mdl_lod_hook(int x, int y, const mrdp_lod_t *L);\n#line 1 "mrdp.c"\n'; cat $(2).tmp ) > $(2)
	rm -f $(2).tmp
endef

$(B)/mrdp_hooked.c: $(M)/mrdp.c | $(B)
	$(call hook,$<,$@)

$(B)/oracle.o: oracle_glue.c $(ORC)/n64video.c | $(B)
	$(CC) $(ORC_CFLAGS) -c oracle_glue.c -o $@

# the model as the RTL is
$(B)/rdpo: rdpo.c $(B)/oracle.o $(B)/mrdp_hooked.c $(M)/mrdp.h | $(B)
	$(CC) $(CFLAGS) -DMRDP_N64 -I$(M) rdpo.c $(B)/mrdp_hooked.c $(B)/oracle.o -o $@

# the model with RDP_EXACT (docs/rdp_exact.md: the console's rules, which the RTL does not follow yet)
$(B)/rdpo_exact: rdpo.c $(B)/oracle.o $(B)/mrdp_hooked.c $(M)/mrdp.h | $(B)
	$(CC) $(CFLAGS) -DMRDP_N64 -DRDP_EXACT -I$(M) rdpo.c $(B)/mrdp_hooked.c $(B)/oracle.o -o $@

$(B)/rdpgen: rdpgen.c | $(B)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers rdpgen.c -lm -o $@

clean:
	rm -rf $(B)
.PHONY: all clean

# rdp_task (sim/rdp) built here: a task dump's RDP commands, for task2rdps.sh
$(B)/rdp_task: ../rdp/rdp_task.c ../rsp/rsp.c $(M)/mrdp.c $(M)/mrdp.h | $(B)
	$(CC) -O2 -DMRDP_N64 -I../rsp -I$(M) ../rdp/rdp_task.c ../rsp/rsp.c $(M)/mrdp.c -o $@
