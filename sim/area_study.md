# Area study: room for texture LOD and depth precision

Goal: free 1,000 to 1,500 ALMs in the RDP, the RSP and the CPU (the fits are at
17,885 to 17,941 of 18,480 ALMs, worst slack +0.003 to +0.036 ns) by moving
registers to block RAM, sharing arithmetic and narrowing it, without changing
a cycle. This note is what was measured and what it means.

## How it was measured

`tools/area2/ooc.sh` (Analysis & Synthesis of one unit alone with the
project's settings) was run in the Quartus Prime 23.1std.1 Lite container
(`chriz2600/quartus-lite`). A unit takes about 45 seconds, so every figure
below that says "measured" is a synthesis of that exact source. All runs used
one snapshot of `rtl/` taken from HEAD (the third argument of `ooc.sh`), so
that edits being made elsewhere in the tree could not move the numbers; the
baseline run from the snapshot gives the same counts as the run from the
working tree.

Synthesis figures are ALUTs, registers and the map's "ALMs needed" estimate.
The fitter packs better than the map estimate, so a fitted saving is smaller;
as a rule of thumb from the earlier area work, a fitted ALM saving is about
half the ALUT saving.

| Unit (SoC configuration) | ALM estimate | ALUTs | Registers | Memory ALUTs | DSP | Block memory bits |
|---|---|---|---|---|---|---|
| RDP (`rdp_top`) | 4,865 | 7,438 | 6,094 | 64 | 39 | 269,704 |
| RSP, 8 lanes, MULW | 4,446 | 6,464 | 1,968 | 256 | 8 | 83,968 |
| RSP, 4 lanes, MULW | 3,919 | 5,585 | 1,939 | 256 | 8 | 83,968 |
| CPU (`mips_sys`, FAST, XWB) | 4,425 | 7,532 | 3,094 | 0 | 4 | 633,920 |
| Register block (`mirlo_regs`) | 761 | 1,276 | 801 | 0 | 0 | 131,072 |

By entity: `rdp_top` itself 2,874 ALUTs, `rdp_pix` 3,388, `rdp_mem` 1,121;
`rsp_vu` 4,288, `rsp_su` 847, `rsp_top` itself 1,276; `mips_core` itself 4,879,
`fpu_n64` 2,606.

Two notes on the tool: `ooc.sh` as committed fails for `rsp` (the instance
does not connect `tr_e2`) and for `video` (`vi_il`), and it instantiates the
RSP with 4 lanes while the SoC builds 8 (`MIRLO_RSP_LANES` defaults to 8 in
`rtl/soc/mirlo_mips.sv`). The figures above come from a copy of the script
with those two points fixed; the script itself is not changed here.

## The main finding: the register-to-RAM moves are already made

The synthesis report was read for what is still in flip-flops, and the
synthesised netlist (written with `quartus_cdb --vqm`) for every register by
name. What a RAM could hold is, almost everywhere, already in one:

* **RDP.** The tiles are M10K (`tl_attr`, `tl_size`, two read copies each,
  and the `ts_*` / `ts1_*` registers are the RAMs' own output registers). The
  attributes' `e[]` and `ade[]` are MLAB. The write FIFO `wf` and the 4-entry
  write-back FIFO `rf` of `rdp_mem` are M10K. The reciprocal table of the
  perspective divide is M10K. And Quartus already turned the pixel pipeline's
  delay lines into seven RAM shift registers (`altshift_taps`: x, the line
  buffer set, the depth, the memory colour and its coverage bit, the depth's
  word), 33 bits by 11 stages for the largest. What is left in flip-flops in
  `rdp_pix` are taps that logic reads at every stage (`shade0`, `shade10a`,
  `shade10`, the texels into the two combiner cycles, `o10a` / `o11a`, the
  blender's operands), the DSP products, and the stage registers that cut the
  long paths. `cur[]` and `adx[]` (512 registers) step every pixel, all eight
  at once: they cannot be a RAM.
* **RSP.** The vector registers are two MLAB copies (the 256 memory ALUTs),
  the scalar registers, IMEM, DMEM and the reciprocal ROM are M10K. The
  accumulators, the operand and product registers and `hold` are read by all
  eight lanes in one cycle.
* **CPU.** The register file (`rf_lo`, `rf_hx`), the FPU's register banks,
  the caches and their tags and the TLB tables are block RAM with synchronous
  reads; with XWB the core's own write buffer is pruned (the store FIFO is
  `cpu_port.sv`, outside the core). What remains in flip-flops is pipeline
  state and COP0.

So the 1,000 to 1,500 ALMs cannot come from register-to-RAM moves alone. The
largest single lever is a configuration choice (4 RSP lanes), and the rest is
a series of 50 to 200 ALM changes to logic.

## Ranked candidates

Savings are synthesis ALUTs unless marked; "fit" is the expected fitted ALM
saving. Effort: S an hour or two, M a day, L several days.

| # | Candidate | Block | ALMs saved (est.) | M10K cost | Cycle-identical | Timing risk | Effort |
|---|---|---|---|---|---|---|---|
| 1 | 4 RSP lanes with MULW (`MIRLO_RSP_LANES=4`) | RSP | measured -879 ALUTs, -527 map ALMs; fit ~220 (rsp.md) to ~450 | 0 | no (~8 % more RSP cycles) | none (fewer paths) | S (a macro) |
| 2 | **Prototyped:** DaDx copy in the free rows of the `ade` MLAB, and one adder for the edges' starts | RDP (`rdp_top`) | measured -159 ALUTs, -117 map ALMs; fit ~60-90 | 0 | yes (verified) | low | done |
| 3 | COP0 storage-only registers in a small RAM (EntryLo0/1, EntryHi, Context, TagLo, LLAddr, WatchLo/Hi, BadVAddr), read at E for an `mfc0` at M | CPU | ~100-200 | 1 | yes, with a one-entry bypass | medium (93.75 MHz, M's result select) | M-L |
| 4 | The rest of the shared multiplier's operand select (`mul_a` still a 21-way choice on 20 bits, `mul_b` 28-way on 17) | RDP (`rdp_top`) | ~50-100 | 0 | yes | low | M |
| 5 | Products registered in the DSP blocks (the RSP's `p_r`: 272 flip-flops; the RDP's `ps3`, `pt3`, the blender's sums) if the fitter does not pack them | RSP, RDP | 0 to ~130 (check the fit report first) | 0 | yes | low | S to check, M to do |
| 6 | FPU result and normalisation selects (`res` 35-way on 23 bits, `zmag` 31-way, `ir` 18-way: ~500 ALUTs of selects) | CPU (`fpu_n64`) | ~150-300 | 0 | not as a pure restructure; yes if only the selects change | high (CPU clock) | L |
| 7 | `rdp_mem`'s write-back FIFO `rf`: Quartus adds 118 pass-through registers and a 113-bit select for its read-during-write, `wf` 45 more | RDP (`rdp_mem`) | ~50-80 | 0 | yes, if rewritten with care | low | M |
| 8 | Two TMEM copies in true dual port instead of three simple dual port | RDP | ~0 (the copies' output selects, ~25 ALUTs, against the shared port's address select) | -5 (saves) | yes | low | M |
| 9 | `e[]` / `ade[]` from MLAB to M10K read on the falling edge | RDP | ~20-40 | 2 | yes | high (half cycle into the e + DaDe adder and write) | M |
| 10 | Vector registers from MLAB to M10K | RSP | ~0 net | 8 | only with a 2 x 128-bit bypass | medium | M |
| 11 | DMEM write data chosen as one word before the 128-bit choice | RSP (`rsp_top`) | measured 0 (+1 ALUT) | 0 | yes | none | done, reverted |
| 12 | `rdp_mem` FIFOs forced to MLAB | RDP | measured worse: +219 ALUTs, +865 registers (a 4-deep FIFO is not inferred as MLAB, it went to flip-flops) | | | | done, reverted |

### 1. Four RSP lanes

Measured on the SoC's configuration (MULW on): 6,464 to 5,585 ALUTs, map
estimate 4,446 to 3,919 ALMs, registers 1,968 to 1,939, DSP blocks unchanged (MULW keeps
eight multipliers). `docs/rsp.md` gives ~220 fitted ALMs and about 8 % more
of a graphics or audio task's cycles. The synthesis difference suggests the
fitted saving may now be larger than 220 (the 8-lane unit has grown with the
pairs, LPAIR); a fit of both is the way to know. It is the only candidate
that alone covers a third to a half of the target. It costs RSP time, which
`sim/rdp/rdp_flow.py` and `rsp_replay` can weigh against the RDP's.

### 2. The prototype (done): the RDP's shared multiplier

The synthesis report's multiplexer table shows where `rdp_top`'s ALUTs go:
the operand registers of the shared 32 x 32 multiplier, `mul_a` and `mul_b`,
were behind 28- to 33-way selects, about 510 ALUTs between them. The largest
single source was `adx[mi]`, an 8-way choice of 32 bits, used only by the span
setup (`cur = e + DaDx * (xs - xh)`).

* **DaDx in the `ade` MLAB.** `ade_ram` was 8 rows of a 32-row MLAB. Rows 8
  to 15 now hold a copy of DaDx, written during S_VSET once `vgx` is complete
  and before the step that writes DaDe (step 5 for Z, step 7 for the others),
  so the RAM's one write port is never wanted twice in a clock. The span
  setup reads row 8 + mi, and takes the product the other way round
  (`mul_a <= dxs`, `mul_b <= DaDx`): the product of two signed 32-bit numbers
  is the same. An attribute that is off is never written in the RAM (its
  `adx` is the 0 the command's start put there), so its operand is forced to
  0 by `attr_on`. `adx[]` itself stays in flip-flops for the per-pixel step.
  Measured: 7,438 to 7,334 ALUTs, map ALMs 4,865 to 4,777, no register and
  no memory added (`mul_a`'s select went from ~330 to ~180 ALUTs).
* **One adder for the edges' starts.** S_XL's `XL & ~1 - product`,
  S_TRI_INIT's `XH + product` and S_AAINIT's three `X & ~1 + product` run in
  different states; they now share the S_AAINIT adder (the subtraction as the
  product's complement plus a carry in). Measured: a further 7,334 to 7,279
  ALUTs, map ALMs 4,777 to 4,748.

Together -159 ALUTs (2.1 % of the RDP), -117 map ALMs, registers, memory and
DSP unchanged. The new paths are short: the MLAB's read now has a 4-bit
address choice in front (it had a 3-bit one), and `mul_b`'s new source is the
same RAM output it already took in S_TRI_INIT. The edges' adder gains a
select in front of one operand and an XOR on the other; its operand `mul_p`
is a register.

Verification (Homebrew Verilator 5.052):

* `tools/area2/rdp_regress.sh`: `rdp_regress: 53 runs, 0 failed` (every run
  prints `RTL == model`), before and after the change.
* Cycle identity: the same 52 runs on the HEAD RTL and on the new RTL, each
  run's summary line compared (cycles, memory reads, writes, pixels): identical.
  24 further seeds (100 to 123, 300 commands each): identical lines, all
  `RTL == model`. Seed 7 with `TB_CMDT` (each command's clock): the 2,668-line
  logs are byte for byte the same.

### 3. COP0 in a RAM

About ten COP0 registers are only stored: written by `mtc0` or an exception
and read by `mfc0` (and the TLB write's `EntryLo` fields, and `eret`'s
`ErrorEPC`). In flip-flops they are ~300 registers plus their part of the
32-way `mfc0` select. A 16 x 32 RAM read at E with `rd` (one stage ahead of
M, where `mfc0` takes its value) gives the same timing; an `mtc0` followed at
once by an `mfc0` of the same register needs a bypass (a 4-bit compare and a
32-bit select). `Count`, `Compare`, `Status`, `Cause`, `EPC` and the
exception-written ones stay registers. This is the largest cycle-identical
CPU candidate, but it sits in the 93.75 MHz domain and this machine cannot
run the CPU lockstep bench (its external model is missing); only the SoC test
programs would check it.

### 4. The rest of the shared multiplier's select

After the prototype, `mul_a` still chooses among the rows' differences, the
row number, the texture rectangle's DsDx / DtDy, the loads' coordinates and
the span offset, and `mul_b` among the edges' slopes (and their `& ~7` and
`>> 2` forms), DaDe / DaDx, the image widths and the rectangle's offsets. The
same method applies: put sources that are used in one state behind a RAM
port that is free there, or swap operands so that a source joins a select it
already shares. Each source removed is worth 10 to 20 ALUTs.

### 5. Products in the DSP blocks

The synthesised netlist keeps `p_r` (the RSP's eight 34-bit products), the
RDP's perspective products (`ps3`, `pt3`) and others as plain registers. If
the fitter does not pack them into the DSP blocks' output registers (the fit
report's DSP section says), restructuring each `p <= a * b` so that nothing
but the register follows the multiplier would move up to ~600 registers out
of the logic. Check before doing anything.

### 6. The FPU

`fpu_n64` is 2,606 ALUTs for a multi-cycle single-precision unit. The large
selects (the result, the magnitude being normalised, the integer input) are
~500 ALUTs. A shared normaliser for the add, convert and divide paths could
save a few hundred ALUTs, but the CPU clock is the hardest to close and
`docs/mips.md` records that the FPU's paths have already broken it once.

### 7. `rdp_mem`'s write-back FIFO

`rf` is read combinationally (`rf_head`) and written at `p2`. To put it in
M10K, Quartus registers the read address and adds 118 pass-through registers
and a 113-bit select for the case where the head is the entry just written
(the FIFO was empty); `wf` gets 45 more. Asking for MLAB made it flip-flops
(measured, +865 registers). A version that holds the head in a register and
the three entries behind it in the RAM avoids the general bypass; it must
keep the head's timing exactly, so it needs the same cycle comparison as the
prototype.

### 8. TMEM: two copies are enough for the filter

The 3-point filter reads three texels a pixel, each from its own copy of TMEM
(`rdp_tmem`: four 512 x 16 memories and a TLUT copy, 5 M10K a copy). TMEM is
written only by loads, and a load runs only with the pixel pipeline empty
(S_LOAD waits for `all_quiet`). So port A of a true dual port copy can be the
load's write port during a load and a read port while drawing, and two such
copies give four read ports: the three texels, and one to spare. That saves 5
M10K and about nothing in ALMs (each copy's output select goes, a port's
address select comes). It is worth doing for the M10K budget of LOD, not for
logic. The TLUT copies are already one per texel and would follow the same
way.

### 9, 10. The LUT RAMs to M10K

* `e[]` / `ade[]` (2 MLABs, ~20 ALMs each): S_TRI_INIT reads `e` at the
  address it writes in the same clock and adds the product to it. An M10K read
  on the falling edge needs the address by mid-cycle and leaves half a cycle
  for the read, the 32-bit adder and the write. With the fit's slack near 0,
  this is the wrong trade for ~40 ALMs.
* The vector registers (16 MLABs, ~160 ALMs): `rsp_lutram` already registers
  its address a cycle ahead, which is what an M10K does, so the read timing
  would be the same. But the next computation's SEL runs in the cycle after
  the EX that wrote its register (`dep` releases it then), and it relies on
  the LUT RAM's new data at that edge. An M10K's mixed-port read during write
  gives the old word, so each copy would need a 128-bit bypass, which costs
  about what the MLABs do.

## What the target needs

The cycle-identical changes found here add up to roughly 300 to 600 fitted
ALMs (the prototype's ~60-90, plus candidates 3 to 7), each a day or more of
work and some in the CPU's clock domain. Together with candidate 1 (~220 to
450) that reaches the low end of the target. Reaching 1,500 means also
accepting a behaviour or performance trade (RSP lanes, an FPU cycle) or
taking area from outside these three blocks (the scan-out, the register
block).

For LOD itself, the DSP blocks are a second budget to watch: the RDP uses 39,
the RSP 8 and the CPU 4 of the device's 66, before the scan-out's. LOD's
fraction and the second texel set want multipliers, and the M10K freed by
candidate 8 is where a second set of texel reads would go.

## Round 2: candidates 4, 7, 3 and 5

The next candidates, each made cycle-identical, checked against the unchanged
RTL in the lockstep benches, measured with `ooc.sh` and kept only when it
measured as a saving. The reference for every comparison is a copy of `rtl/`
and `sim/` taken before these edits (HEAD with the prototype of candidate 2):
its benches were built in that copy, and its units synthesised from it.

`tools/area2/ooc.sh` now synthesises all four units: the RSP is instantiated
as `rtl/soc/mirlo_mips.sv` does (`MIRLO_RSP_LANES` lanes, MULW, `tr_e2`
connected), the video with `vi_il` and the SoC's 24-bit origin mask. It runs in
the Quartus container with `QUARTUS=/opt/intelFPGA_lite/23.1/quartus/bin`.
Where a synthesis result was not decisive, the unit was also fitted alone
(the `ooc.sh` project with every pin virtual, `quartus_fit`, one seed).

| # | Candidate | Kept | ALUTs | Registers | Memory ALUTs | Map ALMs | Fitted ALMs | Verification |
|---|---|---|---|---|---|---|---|---|
| 4 | RDP: the shared multiplier's operands as one select without a hold | kept | 7,279 to 7,169 (-110) | 6,094 (0) | 64 (0) | 4,748 to 4,701 (-47) | 4,996 to 4,943 (-53) | regress 53/0; 100 runs and their per-command clocks identical |
| 7 | RDP: `rdp_mem`'s FIFOs `rf` and `wf` as MLABs | reverted | -69 | -157 | +149 | -23 | +26 (`wf` alone: +16) | as 4 (identical) |
| 5 | RSP: the products' registers `p_r` where the fitter can pack them | kept | 6,464 to 5,930 (-534) | 1,968 (0; fitted 1,968 to 1,697) | 256 (0) | 4,446 to 4,196 (-250) | 4,816 to 4,554 (-262) | 145 runs identical, `tb_dma` PASS |
| 5 | RDP: its products | nothing to do | | | | | | the fit already packs them |
| 3 | CPU: COP0's stored registers in a LUT RAM | behind `C0RAM`, off | 7,532 to 7,530 (-2) | 3,094 to 2,901 (-193) | 0 to 32 | 4,425 to 4,389 (-36) | 4,760 to 4,745 (-15) | 15 SoC programs, an extra COP0 program and a game: identical |

Video (`n64_video`, unchanged, for the tool): 1,581 ALUTs, 1,104 registers, 4
DSP blocks, 666,272 block memory bits.

### 4. The shared multiplier's operands (kept)

The operand registers `mul_a` / `mul_b` were written in nine states, each at
its own steps, and held everywhere else: a 21- to 32-way select a bit, the
hold among its inputs. But every product is read exactly two clocks after its
operands were chosen (S_XL at step 2, S_TRI_INIT at 2..13, S_AAINIT at 2..4,
the span setup at 2..9, the texrect setup at 2 and 3, S_RECT_INIT and
S_LOAD_INIT at 2) and never later, and every one of those states moves `mi` on
each clock. So the hold is not needed: `rdp_top` now computes the operands in
one combinational block, by state, with the steps whose product nobody reads
left as don't-care, and loads the registers every clock. On top of that:

* the row differences y - YH, y - YM and y (the first row's image offset) are
  one subtractor, y minus a chosen row;
* AA_EN's start slopes `t_dx & ~7` are TRI_INIT's `t_dx` with bits 2..0
  cleared by the state, so the two states share one choice of three slopes;
* the texrect's DsDx / DtDy and its row / column distances are one choice
  each (`mi[0]`, and `mi[0]` against the flip);
* S_XL's operands are swapped (the subscanline step in `mul_a`, with the
  other wide operands), and an attribute that is off forces the span offset
  to 0 instead of DaDx.

Variants measured on the way: the select alone 7,201; with the S_XL swap
7,181, with the offset forced instead 7,184, both 7,169 (kept). Splitting
each operand into a narrow sign-extended part and a wide one measured worse
(7,222), and swapping TRI_INIT's step 2 as well too (7,217).

Verification: `tools/area2/rdp_regress.sh` prints `rdp_regress: 53 runs, 0
failed`. The same 52 runs, 24 more seeds (`tb_rdp -r 100..123 300`) and the 12
graphics task dumps under `build/triage/*/dumps` (each with the bench's fast
memory and with `TB_SDRAM=1 TB_CPU=30`), 100 runs in all, were run on the new
RTL and on the unchanged copy, each with `TB_CMDT`: every summary line and
every per-command clock log (170,750 lines) is byte for byte the same, and
every run says `RTL == model`.

### 7. `rdp_mem`'s FIFOs (reverted)

The bypass Quartus builds for the FIFOs in M10K cannot be avoided while the
head is read without a clock: an M10K registers its read address, its mixed
ports give the old word for the entry written at the same edge, and a head
register in front of the RAM only moves the 113-bit bypass to another place.
What does avoid it is an MLAB, whose read needs no clock. Writing the two
FIFOs in an always block of their own makes Quartus infer them as MLABs (the
round 1 attempt, row 12, had left the writes in the engines' block and got
flip-flops). Measured on top of candidate 4: 7,169 to 7,100 ALUTs, 6,094 to
5,937 registers, 1,028 block memory bits (four M10K) freed, but 149 memory
ALUTs (eight LABs as MLABs) added; map ALMs 4,701 to 4,678. The fit says
otherwise: 4,943 to 4,969 ALMs (+26), and `wf` alone in an MLAB +16. An MLAB
holds a whole LAB for a 4- or 16-entry FIFO, which is more than the bypass
costs. Reverted. If M10K blocks become the limit (LOD), this is a way to get
four of them back for about 20 to 30 ALMs.

### 5. Products in the DSP blocks

No synthesised netlist in the design registers a DSP block's inputs or
output: packing is the fitter's (Auto Packed Registers). So the fit reports
were read.

* **RSP: not packed before, now packed (kept).** `p_r` was written in
  `rsp_vu`'s large always block, under `sel_fire`. Synthesis gave every bit a
  flip-flop behind a hold select (a LUT a bit, no clock enable), and the
  fitter packed none of them into the DSP blocks. Moved to an always block of
  its own (`if (!rst && sel_fire) p_r <= p_n`, the same condition), each bit
  is a flip-flop with a clock enable and nothing else: synthesis drops 534
  ALUTs (rsp_vu 4,288 to 3,744), and the fitter packs all 272 of `p_r`
  (fitted registers 1,968 to 1,697, ALMs 4,816 to 4,554). A
  version with the multiplier and its register in a module of their own gave
  the same synthesis (5,927 ALUTs) and was not kept. Verification: 60 random
  programs (`gen_rand_task.py 1..60`) and the 12 task dumps through `tb_rsp`,
  with 8 lanes and with 4 lanes and MULW (`pm_r` moved with `p_r`), on the new
  and the unchanged RTL: 144 outputs byte for byte the same (instructions,
  cycles, RDP words), all `identical` to the model; `tb_dma`: PASS on both.
* **RDP: already packed.** The baseline fit packs `ps3` and `pt3` (48 bits
  each), the blender's `bo12a` / `bo13a`, `o10a` / `o11a`, `fsum`, the
  shared multiplier's partial products and more (1,008 packed registers,
  duplicates included). There is nothing to move.

The same pattern (a register written under a condition in a large block, made
a hold select instead of a clock enable) may cost logic elsewhere in `rsp_vu`
and the other units; it was not searched for here.

### 3. COP0 in a RAM (behind a parameter, off)

With the SoC's configuration (TLB_EN 0, TLB_RAM 1, MON 1) the registers only
mfc0 reads are Index's number, EntryLo0/1, PageMask, BadVAddr, Config's
writable bits, LLAddr, WatchLo/Hi and TagLo (Context is written in two parts,
EntryHi, Wired, EPC and ErrorEPC are read by other logic). `mips_core` has a
parameter `C0RAM` that puts them in a 32 x 32 LUT RAM: one write port takes
mtc0's word under the register's writable bits, LL's address, tlbp's index
and an exception's BadVAddr; mfc0 reads it at M by `m_insn`'s rd without a
clock, so the word written by the instruction ahead (LL followed at once by
mfc0 LLAddr) is the one read, as from the flip-flops, and no bypass is needed
(mtc0, tlbp and tlbr are serialising anyway). A flag a register gives the
reset value until the first write, and tlbr, which writes 0 to EntryLo0/1 and
PageMask without TLB_EN, clears theirs. Index's P bit stays a flip-flop.

Measured (CPU, `mips_sys` FAST XWB): with the RAM's word chosen inside the
32-way case, 7,604 ALUTs (+72); with the RAM's registers 0 in the case and the
RAM's word one choice after it, 7,530 ALUTs (-2), 2,901 registers (-193), 32
memory ALUTs, map ALMs 4,389 (-36), fitted 4,745 ALMs against 4,760 (-15, one
seed); without BadVAddr and LLAddr in the RAM, 7,558. The write port's choice
of sources costs about what mfc0's select loses. Far from the 80 ALUTs asked,
so `C0RAM` defaults to 0, the flip-flops as before (that build measures 7,539
ALUTs, 3,094 registers: the same registers, and ALUTs within the few that an
equivalent change of source moves).

Verification, with `C0RAM` 1 and with the final default: the 15 SoC programs
(`ROM=README.md tb_soc lang/mips/test/build/<p>.bin -c 30`) print output byte
for byte the same as the unchanged tree's (13 say PASS; `vi_test` prints no
verdict on either; `pi2_test` prints `FAIL ROM reads, KSEG1: got 00000040 want
00000000` on the unchanged tree too, with this ROM and with a ROM of zeros).
A COP0 program written for this outside the repository (reset values, each
register written and read back at once and later, Config's merge, LL then
mfc0 LLAddr at once, tlbp and tlbr; 196 values) prints the same on both, with
the same clock count. A 3D platformer for 300M cycles (`FB_N64=1 tb_soc
lang/n64rt/build/n64rt.bin -c 300 -e 60`): the five frame dumps and the log
(288 frames, the CPU at 80246ddc) are byte for byte those of the unchanged
tree, and of `obj_soc`, which gives the unchanged tree's too. Those SoC runs
also carry candidates 4 and 5.

### What round 2 gives

Kept: candidate 4 (RDP, -110 ALUTs, -53 fitted ALMs) and candidate 5 for the
RSP (-534 ALUTs, -271 registers, -262 fitted ALMs). Together about 315 ALMs
on the units fitted alone, before the whole design's fit. Candidate 5's
result suggests looking for the same hold selects in the rest of `rsp_vu`
(`s_r`, `t_r`, the accumulators) and in the RDP and CPU blocks before further
restructuring. Candidate 8 (TMEM in two true dual port copies) was not done:
it saves M10K, not logic.

## Round 3: the hold selects, the fit reports' control signals

What round 2's RSP result suggested: registers that synthesis builds as a
flip-flop behind a LUT that feeds the register its own output back (a hold
select in logic, no clock enable). Found systematically this time, from the
synthesised netlists (`quartus_cdb --vqm`): a script lists every register
whose D input is a LUT with the register's own Q among its inputs, and splits
them into LUTs that do nothing else (Q and at most two other inputs: a pure
hold select, a LUT that a clock enable would remove) and holds merged into a
LUT that is needed anyway (where removing the hold saves nothing). The RDP is
another agent's from this round on; it was surveyed but not changed.

The unit fits of round 2 turned out to move by up to 25 ALMs from one seed to
another for the same netlist (CPU baseline: 4,760, 4,750, 4,750 ALMs with
seeds 1, 2, 3), so every candidate here was fitted with three seeds and is
compared by the lowest and the mean of its three fits.

| Unit | Candidate | Kept | ALUTs | Registers (fit) | Map ALMs | Fitted ALMs, seeds 1 / 2 / 3 | Verification |
|---|---|---|---|---|---|---|---|
| CPU | baseline (C0RAM code removed) | | 7,532 | 3,094 (3,006 / 3,002 / 3,005) | 4,425 | 4,760 / 4,750 / 4,750 | |
| CPU | `ms_data` (the slow path's read) in its own block | reverted | 7,497 (-35) | 3,094 (3,006) | | 4,785 / 4,758 / 4,766 (mean +17) | 15 programs identical |
| CPU | the FPU's product in a register of its own (`mprod`) | kept | 7,483 (-49) | 3,112 (2,977 / 2,979 / 2,978) | 4,395 (-30) | 4,764 / 4,745 / 4,742 (lowest -8, mean -3) | 15 programs, 4 more, the 3D platformer identical |
| RSP | (round 2's `p_r` in place) | nothing left | 5,930 | 1,968 (1,697) | 4,196 | 4,554 | |
| RDP | survey only | | | | | | |

**C0RAM removed.** `mips_core.sv` is again the file as it was before round 2
(byte for byte); the CPU measures 7,532 ALUTs, 3,094 registers, as the
baseline.

**What the netlists hold.** Pure hold selects left after round 2: RSP 16
registers (control bits: `st_sig`, `x_wait`, ...; before round 2 there were
573, `p_r`, `s_r` and `t_r` among them: moving `p_r` out also made `s_r` and
`t_r`, written under the same `sel_fire`, clock enables); RDP 21 (counters
and control bits); CPU 41, of which 32 are `ms_data`. Holds merged into a
wider LUT: CPU 381 (the FPU's `mp`, `ir`, `zmag`, `ax`, `ay`, Count, LLAddr,
the multiplier's `md_ac`: registers that step or shift, whose next value
needs a LUT anyway), RDP 190 (`rdp_mem`'s `lbw_data` 32 and `m_cmd_addr` 30
the largest), RSP 22. The products: the CPU's multiplier already sits in its
DSP block (input and pipeline registers packed); the FPU's 24 x 24 product
went into `mp`, a register with four other sources.

* **`ms_data` (reverted).** Moved to a block of its own with the same write
  conditions, its 32 hold LUTs become clock enables: -35 ALUTs. But the fit
  is worse with every seed (4,785 / 4,758 / 4,766): the clock enables are
  three different signals over its 64 bits, and spreading them costs the
  packer what the LUTs saved.
* **The FPU's product (kept).** `mul.s` took the product into `mp[47:0]` in
  S_MUL and read it in S_NORM, the state after. Now `mprod` takes the product
  every clock with nothing but the multiplier in front of it, S_NORM reads it
  (the only reader: sqrt sets all of `mp` before it uses it), and `mp` loses a
  source. Synthesis -49 ALUTs (fpu_n64 2,606 to 2,563); the fitter packs all
  48 bits of `mprod` into the DSP block (fitted registers 3,002..3,006 to
  2,977..2,979); fitted ALMs 4,742 at best against 4,750, mean -3. A small
  saving, within the seeds' spread in the mean, kept for its ALUTs and
  registers.
* **RDP (survey).** `rdp_mem`'s `lbw_data` in a block of its own measured
  7,169 to 7,160 ALUTs and one fit 4,939 ALMs (round 2's rows-4 fit 4,943):
  too small to tell; left to the RDP's new owner, with candidate 8 (below).

Verification of the kept FPU change, against the unchanged tree's bench,
both run with the same ROM file: the 15 SoC programs byte for byte the same
(13 PASS, `vi_test` no verdict, `pi2_test` its usual FAIL on both), and
`fpu64_test`, `cvtl_test`, `ddiv_test` (built from a copy of `lang/`, PASS on
both) and the COP0 program of round 2, the same; the 3D platformer for 300M
cycles: the five frames and the log byte for byte the same.

**The control signals (item 3 of the round).** Registers by control inputs,
from the netlists:

| Unit | ena only | ena + sclr | ena + sload | ena + sclr + sload | sclr / sload without ena | none | distinct ena / sclr / sload nets |
|---|---|---|---|---|---|---|---|
| RSP | 1,143 | 553 | 159 | 33 | 15 | 65 | 37 / 8 / 16 |
| RDP | 1,313 | 995 | 847 | 102 | 763 | 2,074 | 125 / 68 / 41 |
| CPU | 2,004 | 188 | 107 | 39 | 146 | 610 | 63 / 6 / 10 |

A Cyclone V ALM register takes a clock enable, a synchronous clear and a
synchronous load together, so these registers cost no logic of their own;
what they can cost is packing, as a LAB's ten ALMs share its control signals.
The fit reports put a number on it, "ALMs unavailable due to LAB-wide signal
conflicts": RSP 0, CPU 1, RDP 6 (of 4,939). Nothing to gain there on the
units alone. Two figures from the same reports matter more: ALMs used for
registers only (no LUT) RSP 358, CPU 294..302, RDP 1,223; and every unit fit
says "Difficulty packing design: Low", with 431 (RSP), 480..511 (CPU) and 784
(RDP) ALMs "recoverable by dense packing". The units alone are fitted
loosely: in the full design at 97 %, the fitter packs densely, and an ALUT
saving counts for more than these fits show; the seed spread above is the
same looseness.

**Candidate 8 (TMEM in two copies, measured, not kept).** Started before the
RDP changed hands, measured and then reverted, so that the RDP's new owner
starts from round 2's file: copy 0 in true dual port (port A writes during a
load and reads texel 0 otherwise, port B reads texel 1), copy 1 written by
port A and read by port B (texel 2, no address choice). Cycle-identical:
`rdp_regress` 53 runs 0 failed, the 100 runs and their per-command clocks
identical. Measured: block memory bits 269,704 to 232,840 (5 M10K fewer),
ALUTs 7,169 to 7,205 (+36; the first version, with the choice on both
copies, +55), map ALMs 4,701 to 4,712. Not fitted. The cost is copy 0's port A
address choice (write address or texel 0's) and its second output select.

## LOD

Texture LOD in the RDP (docs/lod.md, "Implementation"): variant (c), the
neighbour's perspective divide, with ghosts for the missing neighbours, the
pixel's extra clocks made by where two RAMs are read (a primitive without the
LOD takes the clocks it took), and the console's perspective divide (its texel
coordinates match the console's to the subtexel) in place of the old one for
every pixel. Candidate 8 (two TMEM copies in true dual port)
is in, as the M10K budget asked.

Measured as round 2 was: `ooc.sh rdp` (here from a copy of the script whose
project directory is under `build/lodw`, since `projects_ooc_*` directories
were being removed by other runs), then the unit fitted with every pin
virtual (`quartus_fit`, seed 1) and a 16 ns clock for `quartus_sta`. The
reference is round 2's RDP (the tree before these edits).

| RDP | ALUTs | registers | memory ALUTs | DSP | block memory bits | fitted ALMs | RAM blocks | worst setup slack (unit) |
|---|---|---|---|---|---|---|---|---|
| round 2 | 7,169 | 6,094 | 64 | 39 | 269,704 | 4,955 | 55 | +1.018 ns |
| round 2 with the console's perspective divide only | 7,067 | 6,013 | 64 | 39 | 269,515 | | | |
| LOD, two TMEM copies | 7,325 | 6,313 | 64 | 39 | 231,341 | 5,097 | 48 | +1.251 ns |
| LOD, three TMEM copies | 7,324 | 6,313 | 64 | 39 | 268,205 | 5,092 | 53 | +0.835 ns |

So the LOD adds 142 fitted ALMs (the budget was 300), no DSP block, and
with the two TMEM copies 7 fewer RAM blocks. The two copies measure +1
ALUT in synthesis here and +5 fitted ALMs against three copies (within one
seed's noise; the area round measured +36 ALUTs on round 2's RDP), for 5
RAM blocks: kept. (These builds are the final RTL but for a texture
rectangle's next-line step, then kept to 17 bits and now exact as the model
has it: four constant bits of an adder's operand.) Where it went, by synthesis:

* **Saved.** The perspective divide: the console's (a 15-bit normaliser, a
  64 x 25 table in logic, an 11 x 10 product for the slope, two 16 x 17
  products) in place of a 31-bit normaliser, an M10K table and two 27 x 27
  products: -102 ALUTs, -81 registers, one M10K, and the two products are
  18 x 18 now (the fit puts them in two blocks of "two independent 18 x 18"
  where it had two 27 x 27 blocks: the same count). The tile RAMs read once a
  pass instead of for two tiles at once: their second copies (two M10K) and
  the `ph ? t1_* : t_*` selects of P4 to P8t go, against some 50 registers
  that carry the fields down.
* **Added.** The next line's S, T, W adders in `rdp_top` (32, 32 and 17 bits)
  and its DaDy registers (51); the issue's ghosts; P4's 17-bit values and
  range; L1 (two 18-bit subtracters, the magnitudes, a 14-bit compare); L2,
  L3 and the fraction's shifter; the P5 select between P3's S, T and
  `st_ram`'s; the combiner's two fraction inputs (one select a cycle, shared
  by the three channels: 13 and 14, or 0 and 6, are one input of each C
  select); the first cycle's alpha in the signed arithmetic.
* **Memory.** `sb_ram` (2 M10K) replaces the stages' delay registers that
  Quartus had made into RAM shift registers; `st_ram` and `lf_ram` (one M10K
  each, 16 deep). Net, without candidate 8, the LOD is about even in RAM
  blocks; candidate 8 saves its 5.
* **Timing.** The unit's worst setup path before was `ph8b -> texA9` (the
  YUV conversion's input select, rdp.md's "timing option kept in reserve"),
  +1.018 ns at the slow 0 C corner; it is +1.251 ns now. The first fit of the
  LOD build had +0.521 ns, on the old reciprocal table: Quartus had made the
  case statement an M10K ROM whose unregistered read fed P2's product; in
  logic (`AUTO_ROM_RECOGNITION OFF`) the path starts from registers.

Item 5 of the request, the per-bit hold multiplexer of round 2's RSP fix:
there is no candidate in the RDP. In the fit's DSP table every product whose
result goes straight into a register is packed (`rdp_top`'s `mul_p`, the
perspective divide's two products); the products that are not are followed
by logic before their register (the tile shift's subtraction, the combiners'
sums and clamps, the blender's sums), and none of their registers is written
under a hold. One thing seen there: the depth's correction to the first
covered sample, `z_dx[31:5] * ox` and `z_dy[31:5] * oy` with `ox`, `oy` of
0 to 3, takes two 27 x 27 DSP blocks for what two shift-and-adds do (some 60
ALUTs): a way to two DSP blocks if those become the limit.

Verification: docs/lod.md, "How it was checked" (the regress, seeds 100 to
160, 211 `TB_LOD=1` runs, the 79 dumps' per-command clocks byte for byte
against the RTL before the LOD).

## 32-bit colour images

32-bit colour images in the RDP (docs/cimg32.md, "Implementation"), measured as the LOD was: `ooc.sh rdp` from a copy
of the script whose project directory is under `build/c32`, then the unit fitted with every pin virtual
(`quartus_fit`) and a 16 ns clock for `quartus_sta`, here with three seeds since a fit moves by up to 25 ALMs from one
seed to the next. The reference is the tree before these edits (the LOD's RDP, `rtl/rdp` copied aside), synthesised
and fitted the same way: its 7,332 ALUTs and 4,976 to 4,992 ALMs are this run's figures for the LOD's table's 7,325 and
5,097 (a later edit to a texture rectangle's next-line step, and the fit's seed).

| RDP | ALUTs | registers | memory ALUTs | DSP | block memory bits | RAM blocks | fitted ALMs, seeds 1 / 2 / 3 | worst setup slack, seeds 1 / 2 / 3 |
|---|---|---|---|---|---|---|---|---|
| before | 7,332 | 6,313 | 64 | 39 | 231,341 | 48 | 4,992 / 4,982 / 4,976 | +1.119 / +0.595 / +0.828 ns |
| first build | 7,598 | 6,428 | 64 | 39 | 231,381 | 50 | 5,181 | +0.864 ns |
| kept | 7,535 | 6,367 | 64 | 39 | 231,381 | 48 | 5,124 / 5,119 / 5,114 | +1.149 / +0.909 / +1.005 ns |

So +132 to +138 fitted ALMs (the budget was 150), no RAM block, no DSP block. By entity, in synthesis: `rdp_top` itself
+94 ALUTs (the split, the colour base, the colour buffer's address, the drop), `rdp_mem` +91 (10-bit word indices in
both engines, a word a pixel in the write engine, the colour span's words in the read engine), `rdp_pix` +18 (the
colour buffer's read address, the memory colour and coverage selects, the 8-bit outputs after the dither). The 40
block memory bits more are the memory colour's shift register from P10 to the blender (9 bits wider, in the same
`altshift_taps` RAM block) and the write engine's `rf` (a bit wider); the RAM blocks are the same.

What the first build taught:

* **Two RAM blocks from packing.** With `cmsk` written at the colour buffer's own address (a pixel an entry) and
  `zmsk` at the depth's, the fit could no longer pack the two pass masks together: 50 blocks against 48. The pixel's
  pass bit now goes in `cnew`'s entry (bit 19, the entry written for every pixel), and `cmsk` keeps `zmsk`'s address.
* **The read request's columns.** `rq_x0` and `rq_x1` were registers loaded from three places (a rectangle's row, a
  triangle's span, the split's second half); the read engine now takes `xs` and `xe`, which hold the same values
  while the request waits. The read engine keeps the span's first and last pixel rather than three derived words. The
  second half's end is a select of `xb` and `aa_xe`, which still hold it, rather than a register.
* **The write word.** `cnew`'s 32-bit entry is laid out so that the write engine's 16-bit word select, `{[35:20],
  [17:2]}`, is the memory word as it is: no 27-bit select.

Together -63 ALUTs, -61 registers and -57 fitted ALMs (seed 1). On a build before the last two, a variant without the
split (its two cuts tied to 0) measured 61 ALUTs fewer, and one with the 32-bit images turned off (by a parameter it then
had) 190 fewer: the rest is the widened indices and the separate addresses, which stayed with them off.

Timing: the worst path is the same in every fit before and after, the YUV conversion's input select into `texA9`
(docs/rdp.md, "A timing option kept in reserve"), and it moves with the seed (+0.595 to +1.149 ns). The new logic's
endpoints, reported on the seed 1 fits: the split's compares into `xe` +3.8 ns and into `spl` +4.6 ns, `rq_cbase`
+3.1 ns (+2.8 before), the write engine's `w_idx` +3.0 ns (+3.1 before) and `lbr_addr` +3.3 ns (+3.5 before), the
`rf` to `wf` bypass +2.1 ns (+1.5 before).
