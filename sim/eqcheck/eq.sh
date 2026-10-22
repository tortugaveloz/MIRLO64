#!/bin/bash
# A module side by side with another version of itself (the area rounds' cycle-identity check, sim/area_study.md round 4):
#   eq.sh <old.sv> <new.sv> <module> <outputs> [other sources...]  ->  obj_<module>/obj_dir/tb [seed] [cycles]
# gen.py writes a top with both versions on the same inputs, every input a top input, and each output pair compared at every
# clock; the <outputs> listed (comma-separated, "-" for none) are left out of that and compared only where they mean
# something: in the cycle after a read for mirlo_regs (its r_req protocol is driven: the address a cycle ahead), else where
# SAMPLE (a C++ expression over t->a_<output> / t->b_<output>), or SAMPLE_<output> for that one, is true. gen_tb.py's
# inputs are random at every clock, every clk* input one clock, every rst* input high for the first 20; EXTRA=<file> adds C++
# lines after the random inputs (a protocol's constraints), EXTRA_POST=<file> after the evaluation (a model's state).
# The ones used in round 4:
#   eq.sh old/mirlo_regs.sv rtl/soc/mirlo_regs.sv mirlo_regs r_rdata rtl/soc/rcp_mi.sv rtl/soc/fifo_fwft.sv   (EXTRA=regs.extra)
#   eq.sh old/sdr_ctrl.sv rtl/soc/sdr_ctrl.sv sdr_ctrl -
#   EXTRA=vi.extra eq.sh old/rcp_vi.sv rtl/soc/rcp_vi.sv rcp_vi -     (origins a row apart, the field turning over)
#   EXTRA=cport.extra SAMPLE="t->a_p_valid || t->b_p_valid" eq.sh old/cpu_port.sv rtl/soc/cpu_port.sv cpu_port p_addr,p_valid
#   EXTRA_POST=si.post SAMPLE_c_q='!pb_now' SAMPLE_wdata=pb_now SAMPLE_c_busy=1 eq.sh old/rcp_si.sv rtl/soc/rcp_si.sv rcp_si c_q,wdata,c_busy
#   EXTRA=video2.extra EXTRA_POST=video.post eq.sh old/n64_video.sv rtl/soc/n64_video.sv n64_video m_valid rtl/soc/afifo.sv
set -e
D=$(cd "$(dirname "$0")" && pwd)
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
old=$(abs "$1"); new=$(abs "$2"); mod=$3; excl=$4; shift 4
src=(); for f in "$@"; do src+=("$(abs "$f")"); done
for v in EXTRA EXTRA_POST; do [ -n "${!v:-}" ] && export $v="$(abs "${!v}")"; done
O=${EQ_OBJ:-$D/build/obj}_$mod; rm -rf "$O"; mkdir -p "$O"; cd "$O"
perl -pe "s/^module $mod\b/module ${mod}_old/" "$old" > old.sv
grep -q "^module ${mod}_old" old.sv
python3 "$D/gen.py" "$new" "$mod" "$excl" > /dev/null
python3 "$D/gen_tb.py" "$new" "$mod" "$excl" ${EXTRA:-}
"${VERILATOR:-verilator}" --cc --exe --build -j 8 -O2 -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    --x-assign fast --x-initial fast --top-module eq_top -I"$(dirname "$new")" eq_top.sv old.sv "$new" "${src[@]}" tb.cpp -o tb \
    > build.log 2>&1 || { grep -E "%Error" build.log | head; exit 1; }
echo "built $O/obj_dir/tb"
