all:
include ../program.mk
CFLAGS += -O2 -Wall -fno-strict-aliasing
ifneq ($(filter mon_test mi_test vi_test ai_test rcpcpu_test si_test viaa_test vi32_test pi2_test tlbmon_test tlbgame_test tlbdev_test ram8_test,$(PROGRAM)),)
CFLAGS += -DMIRLO_VEC_JUMP
endif
all: $(BUILD)/$(PROGRAM).bin
# (a program's checks for the VR4300's TLB: a main and a trap_handler of their own, which the program's calls)
$(BUILD)/%_real.o: %_real.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -Dmain=real_main -Dtrap_handler=real_trap_handler -c $< -o $@
$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
.SECONDARY:
# (rsp_replay's and ram8_test's RDRAM is the top 8 MiB of the SDRAM: the stack below it)
ifneq ($(filter rsp_replay% ram8_test,$(PROGRAM)),)
MIPS_LDFLAGS += --defsym=_fstack=0x400FFFF0
endif
