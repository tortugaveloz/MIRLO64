# Tests for MIRLO's MIPS SoC, run in sim/mips_soc (tb_soc build/<test>.bin).
TESTS := cpu_test int64_test cvtl_test ddiv_test fpu64_test libc_test n64win_test tlbram_test rsp_replay vwin_test hires_test mon_test mi_test vi_test ai_test rcpcpu_test si_test viaa_test pi2_test tlbmon_test tlbgame_test ram8_test tlbpg_test padtest
all: $(TESTS)
$(TESTS):
	$(MAKE) -f test.mk PROGRAM=$@ OBJECTS=build/$@.o
clean:
	rm -rf build
.PHONY: all clean $(TESTS)
