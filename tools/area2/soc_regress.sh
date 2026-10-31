#!/bin/bash
# soc_regress.sh <tb_soc dir> <out dir>: runs lang/mips/test/build/*.bin (built here) on a tb_soc build; per test: output text + md5
T=$1; O=$2; mkdir -p $O
W=$(cd "$(dirname "$0")/../.." && pwd)
for p in cpu_test int64_test libc_test n64win_test tlbram_test mon_test mi_test vi_test ai_test si_test viaa_test pi2_test hires_test vwin_test tlbmon_test tlbgame_test tlbpg_test tlbreal_test tlbdev_test tlbmon_real_test tlbgame_real_test tlbint_real_test tlbpg_real_test mapped_cache_test ualn_test; do
  f=$W/lang/mips/test/build/$p.bin; [ -f $f ] || continue
  ROM=/tmp/a2work/dummy.z64 timeout 900 $T/tb_soc $f -c 30 > $O/$p.txt 2>&1
  echo "$p rc=$? $(grep -ci "fail" $O/$p.txt) fail-lines, $(grep -ci "pass" $O/$p.txt) pass-lines, md5 $(md5sum < $O/$p.txt | cut -c1-8)"
done
