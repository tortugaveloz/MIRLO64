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

## Round 4: the blocks outside the three cores

The fit of main with texture LOD is about 18,160 of 18,480 ALMs, and 32-bit colour images add some 140 more: 300 to 500
fitted ALMs are wanted back, cycle for cycle. Rounds 1 to 3 worked in the RDP, the RSP and the CPU; this round looks at the
rest of the SoC, about 4,000 ALUTs: the register block, the scan-out, the RCP's small units, the SDRAM's arbiter and
controller, the CPU's port, the ports, the PSRAM, the cartridge slot.

**Method.** `tools/area2/ooc.sh` synthesises the smaller blocks alone now too (`ai`, `si`, `vi`, `arb`, `sdr`, `dpc`,
`cport`, `ports`, `psram`, `gbc`), and `soc`, `mirlo_mips` whole for its entity table; `glue` is `mirlo_mips` with the
CPU, the RSP and the RDP replaced by stubs (`tools/area2/glue_stubs.sv`: each output a shift register fed by the parity
of the core's inputs, so nothing around them is pruned or made constant), which synthesises the SoC's own logic in three
minutes where `soc` takes twenty. `OOC_ROOT` puts the projects elsewhere (here under `build/`); `FIT="1 2 3"` then fits the
unit alone with every pin virtual, once a seed. A unit alone fitted with its pins virtual is packed loosely and its
virtual pins cost ALMs of their own, so its fitted figure is larger than its share of the whole design; the differences
are what is compared. `glue` cannot be fitted that way (the SDRAM clock's DDIO output must drive a real pin).
To find where a block's ALUTs go, the synthesised netlist (`quartus_cdb --vqm`) was walked back from each register through
the LUTs of its block: each LUT charged to the registers whose input cone it is in. That is what pointed at the register
block's read (609 of its 1,167 LUTs), the scan-out's row address (96), the VI's origin compare (73) and the SDRAM
controller's timers.

Synthesis, each unit alone, before and after (ALUTs, registers, DSP blocks, block memory bits; RAM blocks from the fits):

| Unit | ALUTs | Registers | DSP | Block memory bits (RAM blocks) | Fitted ALMs, seeds 1 / 2 / 3 | Mean |
|---|---|---|---|---|---|---|
| `mirlo_regs` | 1,276 to 1,144 | 801 to 768 | 0 | 131,072 to 139,264 (16 to 17) | 1,568 / 1,566 / 1,569 to 1,446 / 1,448 / 1,454 | -118 |
| `n64_video` | 1,581 to 1,551 | 1,104 | 4 to 3 | 666,272 | 1,062 / 1,059 / 1,056 to 1,052 / 1,051 / 1,052 (the row adder alone) | -7 |
| `rcp_vi` (on the second-field origin fix, c98087e) | 251 to 210 | 72 | 0 | 512 | 211 / 210 / 207 to 187 / 190 / 193 | -19 |
| `rcp_ai` | 263 to 258 | 328 (fitted 280) | 1 | 0 | 255 / 258 / 256 to 250 / 250 / 250 | -6 |
| `rcp_si` | 172 to 150 | 58 | 0 | 512 (1 to 2) | 220 / 222 / 220 to 209 / 209 / 209 | -12 |
| `sdr_ctrl` | 333 to 293 | 186 to 168 | 0 | 0 | 340 / 338 / 338 to 302 / 300 / 300 | -38 |
| `cpu_port` | 166 to 121 | 251 | 0 | 1,072 | 311 / 311 / 311 to 288 / 289 / 289 | -22 |
| `mirlo_mips` itself (`glue`) | 570 to 536 | 113 | | | | |
| `glue` (all of the above in place) | 5,034 to 4,729 | 4,021 to 3,959 | 5 | 816,227 to 824,419 | | |
| unchanged: `sdr_arb`, `rdp_dpc` with `rcp_dpc_view`, `port_rdp` and `port_apf`, `psram`, `gb_cart` | 282, 199, 56, 81, 49 | 99, 160, 73, 62, 75 | | | | |

`mirlo_mips` whole (`soc`) measures 25,236 ALUTs, 14,661 registers before these changes.

### Kept

* **The register block's read-back from a block RAM.** The read is a 60-way choice of 32 bits, some 1,040 bits of
  data, 609 LUTs with its decode. Of those bits 280 are registers only the CPU writes (the bridge's slot, offset, length and
  address, SCRATCH, the scan-out's base, window, scale and enables, the monitor's CTL, MASK and PI2, the cartridge's CTRL,
  the audio's PLAYBACK_EN): they are now also written into a shadow (one M10K, a word a register at {bank, word}), and read
  back from it with their writable bits, the RAM starting as their reset values (the SoC is reset only at its start).
  The RAM's read is registered: an access's address stands from the cycle the CPU asks (`X_IDLE`) to the cycle of `r_req`,
  so the read registered at the first edge is the word in the second, which is where the old read took it. SCRATCH loses
  its flip-flops (it is only read back); the others still drive their outputs. The rest of the read is written as a word
  of a bank, then the bank (alone that was -14 ALUTs). -132 ALUTs, -33 registers, one M10K.
* **The device port's read data without a hold** (`mirlo_mips.sv`). `x_rdata` was loaded in ten states and held in
  between, the hold one more input of each bit's 20- to 30-way select. Only the cycle of `x_ack` reads it (`cpu_port`
  takes it with the ack, and a store's ack carries nothing), so it is now loaded at every cycle with what the state
  answers. -34 ALUTs (`glue`).
* **The SDRAM controller's timers to 3 bits.** The saturating counts of cycles since a bank's ACTIVATE, PRECHARGE, WRITE,
  READ were 4 bits; they are compared with thresholds of at most 6, so a count that saturates at 7 compares as the 4-bit
  one did. The width now follows the largest threshold (`CW`). -40 ALUTs, -18 registers.
* **The CPU port's read address without an adder.** `a_addr[25:2] + issued` (a 24-bit adder in front of the arbiter's
  select) is `{a_addr[25:5], a_addr[4:2] | issued}`: a line is asked for at its first word (`{pa[31:5], 5'd0}`), a single
  word with `issued` 0. -45 ALUTs.
* **The VI's second-field origin compared with one subtracter.** `wd == org_b + 2 WIDTH || wd == org_b + WIDTH` (two 23-bit
  adders and two compares) is `wd - org_b` against the two row sizes, and the second-field rule's `wd != org_b` is that
  difference against 0. -41 ALUTs (on main, before that rule: 201 to 167).
* **The PIF RAM with a port each for the CPU and the SI's DMA.** It had one write port behind a choice of address and data
  and one read port behind a choice of address; the two writers never write in one cycle, and each reads at its own
  address. -22 ALUTs, one M10K more (32 bits in true dual port are two).
* **The AI's multiplier registers free to be its DSP block's.** The operands (taken in S_MUL0) and the product (S_MUL1)
  are now taken at every clock: S_MUL1 sees the operands S_MUL0 took, S_MUL2 the product. -5 ALUTs; the fit packs them.
* **The scan-out: the woven frame's second-field row from the row adder** (`rowp + 320` and `rowp + the steps` are one
  adder with the 320 among its operands; for a 32-bit picture the step is 640 words, a second constant in the same select) **and the row's last word held as `vi_nw - 1`** (no subtracter at each word):
  -19 ALUTs. **The short lines' end from four constants**: `SHORT_S + (wv ? 2 : 1) * (pal ? 50 : 9) - 1` was a product of
  two selects, which synthesis gave a DSP block: -11 ALUTs, one DSP block.

### Not kept

* The interact words written a bit at a time (each bit: the Pocket's word, its field, or the CPU's): -2 ALUTs.
* The scan-out's reads in flight as one counter in place of `issued` and `got`: not cycle-identical. `inflight =
  16'(issued - got)` is taken in 16 bits, and both count every read of the frame, so in the cycles after `issued` wraps
  at 1,024 and before `got` does the sum reads near 64 Ki: `room` and `low` drop for those cycles (the request is held off,
  the urgency lost). One counter gives the true count, and a 3D platformer's log changed (one interrupt fewer in 300M
  cycles). The wrap is left as it is: fixing it changes the scan-out's timing.
* The PSRAM's data lines' register in a block of its own: +2 ALUTs, +1 fitted ALM.
* The AA filter's three products in DSP blocks: they already are.
* For the three cores, the study's remaining candidates were looked at again and left: `e` / `ade` to M10K (`e` is read
  and written at one address in a clock, its read address would be needed a clock ahead, or a falling-edge read in half
  a cycle); `rdp_mem`'s FIFOs (they are M10K already; `wf`'s bypass cone is 170 LUTs but most of it is its write data's
  select, which stays; an MLAB measured worse in round 2); COP0 in RAM (round 2: -15); the RSP's vector registers in M10K
  (a 128-bit bypass a copy).

### The whole design's fit

`tools/area2/fit.sh`, Quartus Prime 23.1std.1 Lite, one compile at a time; main is `e17dce2` (the D-cache snoop in),
compiled the same way from a copy of its tree, and the branch was this round on `e17dce2` (before it moved onto the second-field
origin fix, which adds 50 ALUTs to the VI on main, 43 with this round's subtracter); `tools/area2/closure.py` for the four corners.

| Seed | ALMs, main | ALMs, this branch | Difference | Worst, main | Worst, this branch |
|---|---|---|---|---|---|
| 13 | 17,879 | 17,563 | -316 | +0.090 (hold, sys): closes | -0.475 (setup, cpu), sys -0.050 |
| 5 | 17,842 | 17,624 | -218 | -0.306 (setup, sys) | +0.064 (hold, sys): closes |
| 9 | 17,883 | 17,625 | -258 | -0.768 (setup, sys) | -0.028 (hold, cpu) |
| mean | 17,868 | 17,604 | -264 | | |

RAM blocks 280 to 282 of 308 (the shadow, the PIF RAM's second port), DSP blocks 56 to 55, registers 16,763 to 16,843
(seed 13). A build of this branch with the PSRAM variant of "Not kept" in it (two ALUTs more) fitted at 17,730 with seed
13 and closed (+0.104): the fit of a design this full moves by a hundred ALMs or more for a change of a few LUTs, so it is
the mean that says what the round gives, about 260 ALMs. Closing still depends on the seed, as it does for main (one of the
three here for each). The paths that miss are inside the cores, not in what this round touched: with seed 13 the CPU's
`e_insn` into `m_res` (setup, -0.475) and, in sys, the RSP's `x_am` / `t_r` into its vector registers' LUT RAM, which is
also main's miss with seed 9 (-0.768): the sys domain's critical path is that write.

### Verification

Against a bench built from unchanged main (Verilator 5.052; the toolchain's outputs built in this tree):

* The SoC programs `cpu_test`, `int64_test`, `libc_test`, `n64win_test`, `tlbram_test`, `mon_test`, `mi_test`,
  `vi_test`, `ai_test`, `si_test`, `viaa_test`, `pi2_test`, `hires_test`, `vwin_test`, `tlbmon_test`, `tlbgame_test`,
  `tlbpg_test` and `dma_coh_test` (`-c 30`, `VGA=1`, a frame dump each frame: `-e 1`; `vi32_test` is not in this tree):
  every output text, cycle count and frame dump, 334 files, byte for byte the same (12 PASS; `cpu_test` its geometry
  RAM's and bus error count's FAILs, `pi2_test` its ROM reads', on both; `vi_test`, `viaa_test`, `hires_test` and
  `vwin_test` print no verdict: their pictures are the frame dumps, the same).
* The 3D platformer for 300M cycles (`FB_N64=1 VGA=1 EXCSTAT=1 -e 60`): the ten frame dumps and the log (288 frames,
  the CPU at 80246ddc) byte for byte the same.
* `sim/vi_filter`'s regress: the 44 runs' pad hashes the same.
* Each changed module side by side with its unchanged version on random inputs (`sim/eqcheck/eq.sh`: a Verilator top
  instantiating both, every output compared at every cycle, the outputs that only mean something under the SoC's
  protocol compared where they do: the register block's read in the cycle after a read, the CPU port's address where it
  is valid with line addresses aligned, the PIF RAM's two outputs where they are read, the scan-out with a memory that
  returns the words it asked for, woven and PAL frames among them): 2 to 4 million cycles each, no difference. A
  deliberately broken copy of the register block, the SDRAM controller, the AI and the scan-out was caught each time, and
  the CPU port's check, run first with unaligned line addresses, caught the assumption it rests on.
* On the second-field origin fix (the branch rebased onto a82eaed), against a bench built from a82eaed: `rcp_vi` side by
  side with a82eaed's (`EXTRA=vi.extra`: origins a row apart, the field turning over; 4 million cycles three times, no
  difference; broken copies of the zero test and of a row compare caught); `vi_test`, `hires_test`, `vwin_test`,
  `viaa_test`, the 3D platformer for 300M cycles, a title with woven 640 x 480 screens and a PAL flight game for 250M
  cycles (4 s, a frame dump every fifth frame): 425 files byte for byte the same (the woven title's include 38 woven
  640 x 480 frames; the flight game's first 4 s are 640 x 240).

## Timing round

Goal: timing margin, not area. At 97 to 98 % fill main closed at one seed of
three (13, 5, 9: worst +0.090, -0.306, -0.768 ns), and two paths came back
whatever the seed: on the CPU's clock (93.75 MHz, 10.667 ns) the instruction
register into M's result, on `clk_sys` (62.5 MHz, 16 ns) the RSP's operands
into its vector registers' LUT RAM.

### The two paths as the analyser shows them

Detailed reports (`report_timing -setup -detail full_path`, slow corners) of
the fits already on disk: main at seeds 5 and 9, round 4's branch (same CPU
and RSP sources as main) at seed 13.

* RSP, main seed 9, slow 0 C, slack -0.768: `rsp_vu|x_am[0]` to
  `rsp_lutram:ma`'s data input, 12 logic levels, 16.57 ns of data path. The
  segments: the adder's subtract select (`x_am` and the signs, fanout 19,
  1.75 ns), the 16-bit adder (2.2 ns to its sum), the sum's zero (two levels,
  2.5 ns), then the clip's select: the flag choice `zero = pre ? fr : live`,
  `le_f`, `pk`, `tt` (fanout 16), the bit select, the result select and the
  write data select (one level each, 0.6 to 1.9 ns with routing), and the LUT
  RAM's data input (1.54 ns of routing, 0.77 ns cell). Seed 5's version (+0.151)
  starts from `t_r` and is the same chain. With eight lanes VCL, VCH and VCR
  always take the adder's flags of the cycle before (`x_pre` was set exactly
  for them, EX waits a cycle), so the live adder in front of `le_f` was a path
  no instruction uses, and the longest one into the register write.
* CPU, round 4 seed 13, slow 0 C, slack -0.475: `e_insn[4]` to `m_res[25]`, 6
  levels: the decode of `e_insn[5:0]` (sllv / srlv / srav: is the amount rs's)
  with 2.73 ns of routing in front of it (`e_insn` fans out across E), the
  amount select `e_sh` (fanout 33), two levels of the barrel shifter, the
  reversal for `sll`, the result kind select, `m_res`. The same placement's
  path from the forwarding selects through `e_sh` had +0.284: the decode and
  its route were the 0.76 ns.
* The CPU's other recurring path: E's branch decision into `rd_target`, where
  the fetch goes after the delay slot (main seed 5 -0.078, round 4 seed 13
  -0.206 at 85 C, the CPU alone -0.330): the forwarded operand, its compare
  (`e_baz` / `e_eq`, three to four levels), `e_taken` (two levels), then
  `e_taken` selecting between the target and pc + 8 in 32 selects (fanout 34,
  1.2 to 1.7 ns of routing) in front of the register.

### What changed (three commits, each cycle-identical)

1. `mips_core.sv`: `e_k_shv`, registered with `e_insn` as it leaves D, says
   that the shift's amount is rs's; `e_sh` is one select after rs's forwarding.
   One register.
2. `rsp_vu.sv`: the clips' flags (`cout_c`, `b16_c`, `zero_c`, `ones_c`) are
   `fl_r` alone with one pass and this cycle's adder with two, where one flag
   (`x_pre`) chose between them per instruction; `x_pre` is gone. The compares,
   VADD and the rest still read this cycle's adder. ALUTs 5,930 to 5,860.
3. `mips_core.sv`: as the branch leaves E, its target (`rd_tt`), pc + 8
   (`rd_tn`) and whether it was taken (`rd_tk`) are registered; `rd_tgt`, the
   choice between them, is made behind the registers, where `f_nx` reads it
   (paths from there had 2.8 to 3.2 ns of slack). `e_taken` now ends in three
   single flip-flops (`rd_tk`, `rd_fix`, `rd_annul`). 33 registers more.

Measured on the way and not kept: the clips' and compares' results made as two
16-bit candidates of s and t chosen by the late bit (accl = c_l ? sum : sel ?
x : y), and `eq` taken from s == t beside the adder: the RSP alone +365 ALUTs,
+316 fitted ALMs; the same choice made on the 4-bit table `tt` instead: +9
ALUTs, and its unit fit slower (+0.212 ns) than change 2 alone.

### The units alone

`ooc.sh` with every pin virtual, `quartus_fit` seed 1, a 16 ns clock for the
RSP and 10.667 ns for the CPU, worst setup slack in the slow corners (85 C /
0 C). One seed: a unit's placement moves its worst path by a few tenths.

| Unit | ALUTs | registers | fitted ALMs | worst setup, 85 C / 0 C | the path named above |
|---|---|---|---|---|---|
| RSP, main | 5,930 | 1,968 | 4,543 | +0.061 / -0.043 (`x_am` into the LUT RAM) | the same |
| RSP, change 2 | 5,860 | 1,967 | 4,537 | +0.883 / +0.642 (the scalar unit's `e_ins`) | write into the LUT RAM +1.315 / +1.645 |
| CPU, main | 7,564 | 3,258 | 4,843 | -0.006 / -0.330 (`e_fa_w` into `rd_target`) | into `m_res` +0.847 / +0.856 |
| CPU, change 1 | 7,560 | 3,259 | 4,798 | -0.059 / +0.055 (`ms` into the I-cache's address) | `rd_target` +0.330 / +0.292 |
| CPU, changes 1 and 3 | 7,563 | 3,292 | 4,774 | +0.042 / +0.180 | `rd_tk` not in the worst 300; `m_res` +0.581 / +0.698 |

### The whole design's fit

`tools/area2/fit.sh`, Quartus Prime 23.1std.1 Lite, one compile at a time.
Main is round 4's compiles of `e17dce2`. Worst setup per clock over the four
corners (`closure.py`); "worst" is every analysis.

| Seed | ALMs | Setup cpu / sys / sys2x | Hold cpu / sys | Worst | Closes |
|---|---|---|---|---|---|
| main 13 | 17,879 | +0.456 / +0.305 / +0.885 | +0.113 / +0.090 | +0.090 | yes |
| main 5 | 17,842 | -0.078 / -0.306 / +0.446 | +0.038 / +0.101 | -0.306 (setup, sys: the RDP's `w0` into `l_n`) | no |
| main 9 | 17,883 | +0.287 / -0.768 / +0.200 | +0.163 / +0.039 | -0.768 (setup, sys: the RSP's write) | no |
| changes 1, 2: 13 | 18,051 | +0.180 / +0.563 / +0.886 | +0.128 / +0.048 | +0.048 | yes |
| changes 1, 2: 5 | 17,815 | +0.112 / +0.744 / +0.235 | +0.149 / +0.012 | +0.012 | yes |
| changes 1, 2: 9 | 17,821 | +0.538 / +0.321 / +0.240 | +0.114 / +0.119 | +0.114 | yes |
| changes 1-3: 13 | 17,859 | +0.308 / +0.391 / +0.791 | +0.038 / +0.092 | +0.038 | yes |
| changes 1-3: 5 | 17,809 | +0.133 / +0.275 / +0.246 | +0.032 / +0.096 | +0.032 | yes |
| changes 1-3: 9 | 17,864 | +0.390 / +0.623 / +0.796 | -0.014 / +0.011 | -0.014 (hold, fast 0 C: `x_rdata` into `cpu_port`'s `c_xdata`) | no |

Every setup analysis is positive in all six branch fits. The ALMs move by
-62 to +172 seed for seed (the fit's spread at this fill; changes 1-3:
-20, -33, -19). The paths named above, in the branch's fits (slow 0 C / 85 C):

* the RSP's write into its LUT RAM: +1.13 to +2.01 ns (main seed 9: -0.768);
  `clk_sys`'s worst is now elsewhere: the CPU port's crossing (`rq_addr` into
  `a_addr`, `wp` into `wp_s`), the RDP's `w0` into `l_n` and `other_h` into
  the texel registers, the RSP's scalar unit;
* `e_insn` into `m_res`: not among the worst; `m_res`'s worst is +0.37 to
  +1.09 (from `w_res` or `e_rb`, the shifter's data);
* `rd_target`: with change 3 its paths end in `rd_tk`, worst +0.133 at seed 5
  (`m_insn` through the FPU's condition into the branch decision of a
  `bc1t` / `bc1f` at E), +0.322 behind `m_res`.

The CPU's worst is spread over the D-cache's address (`m_addr`, `m_insn`,
`m_c.x64` into the RAMs), the FPU's unpack (`st.S_UNPY` into `ax.f`) and that
branch decision, +0.13 to +0.54 ns: no single path dominates any more. The one
miss is a hold of 14 ps on a crossing from `clk_sys` into the CPU's clock, a
path no change touched (round 4's branch missed seed 9 by the same kind of
hold, -0.028); main's seed 5 miss on `clk_sys` is the RDP's `w0` into `l_n`
(load tile's word count, `rdp_top.v`), outside this round's scope, which the
branch's fits place with +0.71 to +2.38.

### Verification

Against a bench built from unchanged main (`e17dce2`, Verilator 5.052):

* RSP (change 2): `tb_rsp` with 8 lanes and with 4 lanes on
  `gen_rand_task.py` 1..60 and the 95 gfx dumps under `build/triage/*/dumps*`
  and `build/lod_dumps/*/`: 155 runs each, all `identical` to the model, every
  output byte for byte the reference's; `tb_dma` PASS on both, the same output;
  `tb_dpc` (`DPC=1`) on 20 dumps: all "the same commands", the same output.
* CPU (changes 1 and 3; the benches also carry change 2): the SoC programs
  of `soc_regress.sh` and `dma_coh_test` (`-c 30`, `VGA=1`, a frame dump each
  frame): 334 files, output text, cycle counts and frames byte for byte the
  reference's (12 PASS; `cpu_test`'s three FAILs and `pi2_test`'s nine on
  both). A 3D platformer for 300M cycles (`FB_N64=1 VGA=1 EXCSTAT=1 -e 60`):
  the ten frame dumps and the log (288 frames, the CPU at 80246ddc) byte for
  byte the same, with changes 1 and 2 and again with all three.

## Round 5: the RDP's route-through LUTs and adders

The RDP alone (`ooc.sh rdp`, every pin virtual, `FIT="1 2 3"`, from a copy of main `5fa9918`): 7,608 ALUTs, 6,400
registers, 48 RAM blocks, 39 DSP blocks, fitted 5,250 / 5,252 / 5,259 ALMs. Each candidate below was measured on its own, on
the tree before it, with the same seeds; a unit's fit moves by up to 15 ALMs from one seed or one unrelated edit to the next
(a candidate that measured a saving on one tree and a loss on the next is reverted), so only the differences of several
ALMs in both seeds are counted.

**Where the fitted ALMs go that the synthesis count does not show.** The fit report counts "route-through" ALUTs (1,468 in
the unit): a LUT that carries a signal to a register. The post-fit netlist (`quartus_cdb --vqm` after the fit; a script
that lists the single-input LUTs that feed a flip-flop by register name) says which registers have them, and it is the
registers with **two sources that are not in their own ALM**. `cur[]` took its first value from the setup adder
(`e + product`) and its pixel step from its own adder (`cur + adx`): the fitter put the register in one adder's ALM and
a route-through LUT behind the other, one for each of 256 bits (170 of them), and a mux. The same for `ax_*`, `exh`,
`row_off`, `vval` / `vgx` / `vde`, S and T. Removing the second source is worth more than its mux in the synthesis count:
the first such change added 8 ALUTs in synthesis and saved 61 ALMs in the fit.

Kept (each cycle for cycle; ALUTs by `quartus_map`, ALMs by the fit, seeds 1 / 2 / 3 where run):

| Change | ALUTs | Fitted ALMs |
|---|---|---|
| `rdp_mem`: one adder for the write command's word address (the colour span's 8-bit and other words, the depth span's, the hidden group's) | -91 | |
| `rdp_top`: a load's 64-bit word count as ceil((n << size) / 16), one adder (checked for every n and size) | -28 | |
| `rdp_pix`: the decal tolerance's lower bound from the subtraction's borrow, not a second compare | -30 | |
| `rdp_mem`: a load's read count in one adder (two carries as one addend), TMEM's word step in one adder | -32 | |
| `rdp_mem`: the write engine's end-of-word check shares the counter's increment | -9 | |
| these five | 7,608 to 7,416 | 5,250 / 5,252 to 5,139 / 5,133 |
| `rdp_top`: a triangle's first attribute values through the pixel step's adder (`0 + the value` in place of `cur + adx`) | +8 | -61 / -50 |
| `rdp_top`: the AA edges `ax_h/m/l` and `exh` the same way | +4 | -36 (seed 1) |
| `rdp_top`: `row_off` kept as the pixel count (`row_h`), doubled where used; one adder, its first value `0 + product` | -11 | -22 (seed 1) |
| `rdp_pix`: S and T come in as the 16 bits the pipeline takes (the choice of the half is made in `rdp_top`; 64 registers fewer) | -7 | -35 (seed 1) |
| `rdp_top`: a texrect's first S and T through the same adder as a triangle's attributes | -48 | -27 / -38 |
| `rdp_top`: `vval`, `vgx`, `vde` loaded from one choice of halves | +23 | -48 (seed 1) |
| all | 7,377 | 4,881 / 4,892 / 4,900 |

So the RDP alone is 231 ALUTs, 60 registers and 359 to 369 fitted ALMs (mean -363, 7 %) smaller, with the same RAM blocks and
DSP blocks.

Timing, from the same fits (`quartus_sta`, every clock at 16 ns, the slow corners): worst setup slack of main +0.78 to +1.27 ns
(seeds 2 and 3), of this branch +1.27 ns (seed 3, 0 C) and +1.64 ns (85 C); the worst path is the same one in both, the
texture decode into `tex9` / `texA9` (rdp.md's "timing option kept in reserve"). The paths that the changes make (`mul_p`
and `e` through two adders into `cur`) have 2.8 ns of slack in the slow 0 C corner, the other new ones 4.8 ns or more.

Not kept (measured):

* The pure delays of the pixel pipe (the blender's P / M and the copy mode's texel, two clocks, 90 bits) in a block RAM instead
  of registers: 152 registers fewer in the fit and 28 / 41 fitted ALMs more. A register that only copies another costs no
  ALM that the fitter does not fill with another LUT's.
* The rectangle's corners as incrementers with a carry in (the `+ 3 >> 2` is that already): +1 ALUT. The three-way choice
  of `aa_x` by hand: +4. One mux for the write FIFO's colour word (the fill is the same in the 8-bit case): +7.
* The load address and the frame's row address in one adder (`crow`): +12 ALUTs, -5 ALMs (seed 1). The read engine's load
  address from the span's adder (no `w_next`) with the load's counters through the adder as the others: +18 ALUTs, -8 ALMs (seed 1).
* The triangle command's clear of `adx` moved into the attributes' load (an attribute that is off is written 0 in S_VSET):
  -16 / -29 ALMs on the tree before the changes above, +14 / +11 on the last one (-20 ALUTs): noise, reverted.
* `rdp_mem`'s `rf` and `wf` bypasses (119 and 45 registers, a 114-bit and a 36-bit select, ~170 ALUTs with the write data's
  choice) are left: the head is read the clock after the entry that makes the FIFO non-empty is written, which a block
  RAM does not give (round 2).

Verification (Verilator 5.052; the reference is a bench built from main, `sim/rdp/build_rtl.sh` with `RDP_DIR` the copy of
`rtl/rdp`): the 62 random runs of `rdp_regress.sh` and 119 more (seeds 100 to 290: plain, `TB_LOD`, `TB_C8`, `TB_C32`,
`TB_CC`, the SoC's memory with `TB_STALL`), 181 in all, each run on the reference and on the new RTL with `TB_CMDT`:
every summary line and every command's clock byte for byte the same, and every run says `RTL == model`; `rdp_regress.sh`
itself: 63 runs, 0 failed. The load word count was checked for every texel count and size against the old expression.

## Round 6: the RSP's adders and the FPU's data blocks

The RSP and the FPU, measured as above (`tools/area2/ooc.sh`, the unit alone, the same seeds, a snapshot of main). Each
change is cycle for cycle the same as before.

### The RSP (8 lanes)

Each trick uses an adder that is idle where the logic it replaces was needed. All apply to 8 lanes; the 4-lane build keeps
the old logic.

| Change | ALUTs | Fitted ALMs (mean of 3 seeds) |
|---|---|---|
| main | 6,045 | 4,729.7 |
| the lanes' `-t` from the adder that the clips and VABS do not use (a new adder mode: 0 - t, in VABS and in the second cycle of VCH / VCL / VCR) | -107 | 4,645.7 |
| VSAR reads the accumulator through the multiply adder (adds 0), in place of a three-way choice of registers per lane | -113 | 4,558.7 |
| VABS's saturation from the adder's `-t` (`s[15] && t[15] && lo[15]`), not a 16-bit compare of `t` | -35 | 4,509.3 |

Total: -255 ALUTs and -220 fitted ALMs; registers, DSP blocks and block RAM are the same. The worst path of the unit is the
vector registers' write data into the LUT RAM, in both trees; no path got longer.

Not kept: the scalar unit's decode after the pair mux (-36 ALUTs, but the IMEM-to-`e_c` slack falls from +1.9 to +0.6 ns in a
unit that is among the worst of the whole design); `x_wd` as one 32-bit choice plus a wide one (-3: Quartus does it already);
sharing the `+4` / `+8` PC adders (0: shared already); the DMA length's decrement as a mux (-5); the dither as one subtract
per tap (+87); removing the `hold` register (-14 to -40 ALMs for 128 registers).

### The FPU

The saving is in the data blocks: `fpu_n64`'s registers had each a priority chain of sources, which `MUX_RESTRUCTURE OFF`
(the CPU's setting) keeps as chains. Each is now a one-hot select, an AND-OR and one enable, in a block of its own, and the
adds share one adder.

| Change | CPU ALUTs | Fitted ALMs (mean of 3 seeds) |
|---|---|---|
| main | 7,499 | 4,709.7 |
| one adder `x + (y ^ inv) + cin` for the add's sum, the negations of `cvt.s.w` / `cvt.s.l` and the rounded integer | 7,385 | 4,716.7 |
| `wd_e` / `wd_o` without the zero defaults, one choice for the `mtc1` / `lwc1` word | 7,330 | 4,649.7 |
| sqrt's exponent as `e[7:1] + 68` | 7,306 | |
| the iteration and the one-bit left shifts through the normaliser's barrel | 7,309 | |
| `zmag` (nine sources) one-hot | 7,209 | 4,550.7 |
| `ir` one-hot | 7,162 | 4,533.7 |
| `res` one-hot (13 flags) | 7,011 | 4,445.7 |
| `zexp` one-hot; the mul and div exponents from one adder | 6,890 | 4,387.0 |
| two timing fixes: the carry-in as the low bit of one 33-bit sum; the data blocks' enables not gated by the core's flush | 6,845 | 4,363 |

The CPU alone goes from 4,710 to 4,349 fitted ALMs (mean of 5 seeds, -361, 7.7 %), 7,499 to 6,845 ALUTs, with the same
DSP blocks and block RAM. The one-hot series misses the CPU's clock by 1.6 to 2.1 ns until the last two changes; with them the
FPU's endpoints have +0.01 to +1.0 ns, and the unit's worst path is not the FPU's. No FPU instruction changes its cycle count.
Internal state that is not read before it is written again (`zmag`, `ir`, `zexp`, `res` after a flush) is written in states
where the old code left it alone.

Not kept: `ax` and `ay` one-hot (+24 ALUTs and +41 ALMs on the CPU although -81 on the FPU alone); a staged-sticky right
shifter (-5); a nibble-priority leading-zero count (+27); dropping the flush of the double-to-float result (+56); the register
write data as an AND-OR (-15 ALUTs, +68 ALMs fitted alone); left shifts through the right shifter on the reversed value (+14).

Verification: lockstep against the reference model with the FPU's trace compared between a bench built from main and one from
the branch, per cycle (retired pc, instruction, register values, FPU writes, FCR31, cycle count): 232 runs on 52 ROMs, half
with 64-bit operations (`gen_rand_fpu_rom.py w64`), memory latency 0, 3 and 5, FAST on and off; the 33 SoC test programs on
both, output and cycle counts identical.

### The whole design

With the CPU's 64-bit division and unaligned doubleword accesses (+77 and +122 ALMs on the CPU alone), the RDP of round 5,
and the two changes above, `tools/area2/fit.sh` gives 17,507 to 17,575 ALMs (95 %) over four seeds, against 18,197 to 18,213 on
main (98 %), and every one of the 128 timing analyses is at or above zero in each (the worst +0.039 to +0.092 ns). Without
the FPU's changes the same tree fits at 17,740 to 17,799, and one seed of four misses.

## Round 7: the console's rules in the RDP (docs/rdp_exact.md, rules 2, 4 and 6's copy mode)

The RDP alone (`ooc.sh rdp`, every pin virtual, `FIT="1 2 3"`, from main `cb446a5`): 7,377 ALUTs, 6,340 registers, 39 DSP blocks,
fitted 4,881 / 4,892 / 4,900 ALMs (mean 4,891), worst setup slack of the 16 ns unit +1.265 ns (seed 3). Each step is the tree
as committed, measured the same way:

| step | ALUTs | registers | DSP | fitted ALMs, seeds 1 / 2 / 3 | mean (against main) |
|---|---|---|---|---|---|
| copy mode by groups, a rectangle's LOD next line | 7,539 | 6,349 | 39 | 5,001 / 5,008 / 4,991 | 5,000 (+109) |
| and the shade and Z at the first covered sample, the dz code | 7,829 | 6,360 | 37 | 5,132 / 5,140 / 5,142 | 5,138 (+247) |
| and the span's start (first version: `dif` in registers, the copy mode's mask twice) | 7,977 | 6,652 | 37 | 5,245 / 5,241 / 5,249 | 5,245 (+354) |
| the same, `dif` a MLAB with one write port, the copy mode's mask shared | 7,892 | 6,452 | 37 | 5,181 / 5,179 / 5,186 | 5,182 (+291) |

Worst setup slack of the last: +1.10 ns (seed 3), the same path as main's (the texture decode into `texA9`). Where it went:

* **Shade and Z at the first covered sample** (+131 ALMs, +290 ALUTs): the shade's `(A >> 14) * 4 + ox * DaDx' + oy * DaDy'` for four channels and
  the Z's of 22 bits. A first version with `*` of the 2-bit factors took 8 more DSP blocks and missed timing by up to 0.6 ns at the 16 ns
  unit clock (the shade from the mask to `shade0` in one stage); as shifts and adds it takes none (and Z's two DSP blocks are
  gone: 39 to 37) and the slack is +0.84 ns. DaDy of the four shade attributes is 52 registers.
* **The span's start** (+54 ALMs after the sharing): three 32-bit subtractors for `dif`, the adders of the start value, the x fraction's
  24 x 9 product (in logic: Quartus puts a multiplier this small there); `exh` and its adder are gone. A row's setup takes one clock
  more (the setup state: 8.9 to 9.7 million clocks on the 138 captured tasks), the whole run of them 95,115,501 to 95,126,383 clocks (+0.01 %: the
  row's step of `e[]` starts with the setup's last clock now).
* **Copy mode and the rectangle's next line** (+109 ALMs before the sharing of the mask logic): the pixel's place in its group
  goes down the pipe (15 registers), the texel's add and the 10-bit mask. A first estimate in rdp_exact.md was 20 ALUTs.

Verification: `rdp_regress.sh` 80 runs, 0 failed (63 before: the new ones are `TB_COPY` and `TB_LOD` with `TB_RECT`); the 138 captured
tasks RTL == model, also per command; with the model built without the rules each of the new seeds fails (the benches see them).


The whole design (`tools/area2/fit.sh`), the same seeds, main `cb446a5` against this tree: seeds 1 / 2 / 3, main 17,422 / 17,333 / 17,363 ALMs,
with the rules 17,788 / 18,094 / 17,833 (+532 on average: more than the unit's +291, as a design at 94 to 96 % of the device spends more ALMs on
placing). Fifteen seeds with the rules: 17,786 to 18,125 ALMs; the 128 timing lines are all at or above 0 on four of them (7, 10, 14 and 15; 17,841
to 18,063 ALMs) and miss by 0.001 to 0.5 ns on the others, in the CPU's 93.75 MHz domain or in `clk_sys`; main misses on two of its seeds 1 to 3
(-0.001 and -0.129 ns). (Seeds 1, 2, 4, 5, 9 and 13 miss in the CPU's domain only; seeds 3, 6 and 8 in `clk_sys` by 0.002 to 0.03 ns, 3 in setup and 6 and 8 in hold; 11 and 12 also in a third clock. The paths themselves were not traced.)

## Round 8: the depth image's own dz in the RDP (docs/rdp_exact.md, rule 8)

The depth compare's tolerance and the blender's coverage weights use the depth image's own dz, which the RDP writes with each depth (the dz
code's low two bits in the hidden plane, at the depth image's addresses) and reads back: a hidden-plane word of the depth span read and written
for each 16 pixels, as the colour span's. The RDP alone (`ooc.sh rdp`, `FIT="1 2 3"`), the tree of round 7 (`8454ce2`) against this one:

| | ALUTs | registers | DSP | block RAMs (whole design) | fitted ALMs, seeds 1 / 2 / 3 | mean |
|---|---|---|---|---|---|---|
| round 7 | 7,892 | 6,452 | 37 | 231 | 5,181 / 5,179 / 5,186 | 5,182 |
| the depth image's own dz | 8,058 | 6,512 | 37 | 234 | 5,289 / 5,282 / 5,303 | 5,291 (+109) |

The estimate in rdp_exact.md (200 to 300 ALMs) was high: the second hidden buffer, its address arithmetic and the write engine's merge are shared with
the colour image's (the merge is one logic for both passes, which select their lanes and their hidden bits), and the tolerance's decode replaces
the old step's priority encoder (`z_tol`, `zt_m`) and the word's floor (7 flip-flops). The new pieces: the depth image's row base (`rzb`, 20 flip-flops
and an adder), the hidden pair read and the memory's code (`rawdz`, `texp`, the two shifts: 30 flip-flops down the blender's two stages), two
adders of the read engine for the depth span's first and last group, a phase flag, and the 36-bit depth lane (4 flip-flops a stage of the write
engine's queue). Three block RAMs: the pixels' copy of the depth image's raw hidden buffer (256 x 32), the write engine's copy twice as deep
(512 x 32, the depth image's groups in the upper half); the depth buffer of the new results is 36 bits wide in the same four blocks.

Cycles, on the 138 captured tasks (the RDP alone, `tb_rdp`): the bench's memory 95,133,674 to 96,770,887 (+1.7 %); the SDRAM controller's rules
(`TB_SDRAM=1`: a row open per bank, precharge and activate, turns) 91,983,477 to 97,401,049 (+5.9 %). The command channel is the limit in a
span of two images: a word of two pixels is one read and one write of each image, and the hidden words add one read and one write of each a group
of eight words (36 against 34 commands a 16 pixels, +5.9 % where it limits). On the SDRAM the depth image's rows of the hidden plane
are two more row changes a span (in the bench's layout the hidden words of the two images lie in one bank): without the depth image's hidden reads (the
compare then wrong) the first 40 tasks cost +2.6 % in place of +6.1 % (a row's pixels wait for them, behind the colour image's), and without its hidden
writes the first 93 cost +4.1 % in place of +5.6 %. A mitigation that was not tried: writing a span's hidden words in a batch at its end (one row change, not two a group), which would help the
colour image's hidden writes as much.

The whole design (`tools/area2/fit.sh`, seeds 1 to 15, this tree): 17,652 to 17,803 ALMs (mean 17,731), against 17,786 to 18,125 (mean 17,875) of round 7's
fifteen: the same seeds fit smaller (seeds 1 / 2 / 3: 17,735 / 17,702 / 17,716 against 17,788 / 18,094 / 17,833), which the RDP's +109 ALMs do not explain
in themselves (the fitter's packing moves a design this full by more than that from one run to another; it is not a saving to count on). The 128
timing lines are all at or above 0 on six seeds (4, 5, 6, 8, 14 and 15; 17,652 to 17,767 ALMs, worst slack +0.021 to +0.075 ns) against four of round 7's;
the nine others miss by 0.004 to 0.6 ns: in the CPU's 93.75 MHz domain, in setup (seeds 1, 2, 7, 10 to 13) or by hold (0.010 to 0.089 ns in seeds 1, 2, 3, 7 and 9). None
of the RDP's new paths is among the failing ones (the worst is the CPU's `e_c.alu` to `m_res`; the hold of seed 1 is `c5_line` to `u6_line` of the LOD, untouched), so
there was nothing in the compare to pipeline.

Verification: `rdp_regress.sh` 93 runs, 0 failed (80 before: the new ones are `TB_ZDZ`, also with the SDRAM's timing, a width whose rows share
hidden groups and 32-bit images); the 138 captured tasks RTL == model, with the SDRAM's timing too; `run.sh 50`: the new `zdz` suite (1,300
primitives, the oracle against the model with the rule: 0 words differ; 106,113 colour words without it) and four minimal streams
(`memdz_*`). Found by the benches while it was made: the read engine must hold a span's words back until the hidden words are all asked for (the
words of a 32-bit image's span were asked for before the depth image's groups, whose data then came back as the words); the second half of a
32-bit image's wide span reads the depth image's groups only after the first half's write, as they may share one. With the model built without
the rule each `TB_ZDZ` seed fails; and with the oracle's model made without one detail of the rule (the doubling, the floor of 16 >> exponent, the
coplanar code, the weights' shifts, the shift of the interpenetrating coverage, the hidden bits or the pixel's own dz in the tolerance) `zdz`
differs in 3 to 426 of its 1,300 primitives in each case.

### The noise LFSR free-running

The RDP's noise generator steps on every clock, as the console's does, instead of once for each pixel with coverage (docs/rdp.md, "Noise"): a pixel
takes the bits the register holds when it reaches the stage that uses them. The second copy for the combiner's NOISE (`nlfsr`, 23 flip-flops and its
enable) and the five-stage delay of the colour dither's bits (`nz1` to `nz5`, 45) are gone. The per-pixel LFSR stays behind `RDP_LFSR_PIXEL`, which
the lockstep benches and `rdp_regress.sh` build (`sim/rdp/build_rtl.sh`); the model is unchanged. This change alone, the RDP alone (`ooc.sh rdp`): 5,279 /
5,281 / 5,297 fitted ALMs against 5,289 / 5,282 / 5,303 (-6 on average), 27 registers and 5 ALUTs fewer.

The whole design with the depth image's own dz and the free-running LFSR (seeds 1 to 15): 17,667 to 17,765 ALMs (mean 17,711; the dz alone 17,731: inside
the fitter's noise). The 128 timing lines are all at or above 0 on four seeds (2, 3, 7 and 13; worst slack +0.003 to +0.056 ns) and the other eleven miss by
0.001 to 0.41 ns: setup in the CPU's domain (seeds 4, 5, 6, 8, 9, 11, 12 and 15), hold (0.006 to 0.053 ns in seeds 1, 5, 10, 12 and 14) and, in seed 15 also,
the SDRAM PHY's `a_q` (setup, -0.207 ns). No RDP path is among them. (Six of the previous fifteen close, four of round 7's: the number moves
with the seed, not with these changes, whose paths are not the failing ones.)

Verification: `rdp_regress.sh` 108 runs, 0 failed (93 before: the new ones run the free-running build, `obj_free`, with `TB_FREE` checking that
the LFSR steps on every clock, `TB_NONOISE` streams, in which nothing reads the noise, RTL == model, and `TB_NOISE` streams, the noise colour dither alone, in
which the colour words differ from the model's by a step of a 5-bit channel at most and nothing else does).

## Round 9: the RDP's area, to make room (docs: rdp.md, rdp_exact.md)

The aim was to free logic in the RDP, with its cycles, its results and its modes as they are. Ten changes, each a commit; the RDP alone
(`tools/area2/ooc.sh`), two ways: `rdp` (rdp_top with every port an output of the unit, as before) and `rdpw` (new: the ports as
`rtl/soc/mirlo_mips.sv` connects them -- `load_count` and `unknown_ops` are not read, nor are the address bits above the RDRAM's -- so
that what the SoC prunes is not counted: 106 ALMs of the 5,295). Main `79f93bf`: rdp 8,135 ALUTs, 6,496 registers, 5,295 ALMs (synthesis
estimate), 37 DSP blocks; rdpw 7,990 / 6,336 / 5,189.

**How far the numbers can be trusted.** The same netlist gives the same count, and renaming a signal or swapping the operands of a sum
changes nothing (checked: 0). But any edit that changes the netlist moves the estimate by more than its own effect: the 'ninth bit of the
shade' (24 registers fewer, 64 RAM bits fewer) measured -18 ALMs in rdp and +31 in rdpw on one tree, -4 and +6 on another, and
in unit fits (rdpw, 16 ns clock, seeds 1 / 2 / 3, the seeds themselves 5 to 10 ALMs apart) +14 ALMs; it was dropped. So the table
gives the synthesis count of each commit on the one before, and the kept-or-not was decided by fits: leave-one-out, three seeds a tree
(the change taken out of the tree it is in, the mean against the mean of the tree with it).

| commit | rdp ALUTs / regs / ALMs | rdpw ALUTs / regs / ALMs | DSP | fits: taking it out |
|---|---|---|---|---|
| main | 8,135 / 6,496 / 5,295 | 7,990 / 6,336 / 5,189 | 37 | |
| fill colour read from its register in `rdp_mem` | 8,130 / 6,464 / 5,281 | 7,960 / 6,304 / 5,166 | 37 | |
| hidden words' row bases from the row adders | 8,068 / 6,464 / 5,253 | 7,950 / 6,304 / 5,161 | 37 | +23 |
| COMBINED cleared where registered | 8,077 / 6,464 / 5,259 | 7,933 / 6,304 / 5,162 | 37 | +25 |
| low five bits of the shade's and W's iterators | 8,052 / 6,414 / 5,245 | 7,873 / 6,254 / 5,127 | 37 | |
| filter edges B - A, C - A; weights crosswise | 7,973 / 6,414 / 5,189 | 7,805 / 6,254 / 5,081 | 37 | |
| shade's step to the first covered sample, DSP | 7,752 / 6,414 / 5,060 | 7,646 / 6,254 / 5,006 | 41 | (with Z's: -81) |
| Z's step to the first covered sample, DSP | 7,713 / 6,414 / 5,062 | 7,524 / 6,254 / 4,935 | 43 | +34 |
| a load's request not copied in `rdp_mem` | 7,664 / 6,397 / 5,027 | 7,545 / 6,237 / 4,956 | 43 | +8 |
| write-back FIFO keeps the pass's word | 7,651 / 6,359 / 5,006 | 7,539 / 6,199 / 4,943 | 43 | (-31, before and after) |

Total, synthesis: rdp 8,135 to 7,651 ALUTs (-484), 6,496 to 6,359 registers, 5,295 to 5,006 ALMs (-289), 37 to 43 DSP blocks, 152 RAM bits fewer;
rdpw 7,990 to 7,539 ALUTs, 6,336 to 6,199 registers, 5,189 to 4,943 ALMs (-246). Fitted (rdpw alone, seeds 1 / 2 / 3, ALMs): main 5,226 /
5,219 / 5,218 (mean 5,221); the series 4,986 / 4,986 / 4,979 (mean 4,984): **-237 ALMs (-4.5 %)**, registers 5,932 to 5,796, six more DSP
blocks. The unit's worst setup slack (the slow corner, 16 ns): +1.4 ns against +1.1 ns, the same path (the texture decode into
`texA9` / `tex9`, from `other_h[9]`); to `shade0` +3.2 ns, to `zc0` +4.5 ns, to the iterators `cur` +2.7 ns; the fast corner's hold +0.06 ns.

What each does: the first two remove a register copy and two 10-bit adders that duplicate wider ones (a register that only copies
another costs a half ALM in the fit: the fitter puts a feeder LUT in front of it, some 900 of them in the unit); the third moves a 36-bit AND
into the register's clear; the fourth finds five bits of five 32-bit iterators that nothing reads (the shade's and W's DaDx have zeros there:
adx, the adders and cur lose them); the fifth is a rewrite of the filter that does not change its number (the sum of two products does not
care which edge has which weight: the lower triangle swaps the 6-bit weights in place of 64 bits of texel); the DSP commits are rule 4's
`ox * DaDx' + oy * DaDy'` as the sum of two products of a block a shade channel and two for Z (the sum of two 2-bit-by-13-bit products is
a block; timing of this one missed by 0.6 ns in round 7 with each product in a block on its own and the choice of the offsets in front of it; here
it has +3 ns); the last two drop what a load and the write-back's FIFO carried twice (the FIFO is read as a RAM with a bypass: 38 registers
and their select fewer).

**Not kept** (measured, unit synthesis in rdpw unless said): the ninth bit of the shade (above: fits +14 ALMs); the shared multiplier's
first operand in 16 bits (`mul_a`: 14 registers fewer; synthesis 0 / +9 ALMs, fits -6 on average of seeds spread by 26: left out); the texture
coordinate's clamp, mirror and mask on 17 bits in place of 19 (provably exact: 22 bits signed is the most the coordinate takes; fits 0);
the scalar choices of the combiner's C operand once for the three channels (-14 ALUTs, -3 ALMs); the RDP's reciprocal table in a block RAM
(its 22 ALUTs: +13 ALMs, -7 ALUTs); the shift registers of rdp_pix as registers (`AUTO_SHIFT_REGISTER_RECOGNITION OFF`: -77 ALUTs, +489 registers,
+38 ALMs); the pixel results written to the new line buffers straight from the pipe's output registers, without the 71 registers of
rdp_top's own in front of the RAMs (cycle-identical: the RAMs' input registers take the data a clock earlier; fits 4,997 / 4,999 / 5,000
against 4,999 / 4,997 / 5,006: -2 ALMs for a bigger change); the TLUT's texel decoded by the same function as the others
(-4 ALUTs, -19 ALMs, a level deeper on the TLUT path).

**Where the area is, measured by replacing it with wires** (rdpw, a tree of the series' middle): the decode of the three texels 338
ALUTs; the combiner's two cycles about 750 ALMs on main (cycle 1 432, cycle 0 324: the arithmetic itself is DSP blocks and 35 ALMs); the texture
coordinate's clamp, mirror and mask 217 ALUTs; the perspective result's range and window (`dv17`, `tx16`) 143; the blender's operand
selects 115; the texture LOD 259 ALUTs (185 ALMs); a load's word count (`load_words`) 70 ALUTs; the depth compares 32.

**Ideas not done** (estimates by the ablations above): the load word count over the clocks of S_LOAD_INIT, in the shared adder (-40 ALMs);
the texture address's four adders (`taddr`) as two bases and two offsets, then four sums (-15 ALMs, the path from the row multiplier gets
a second adder); the three edge words `t_xh`, `t_xm`, `t_xl` (96 registers behind 96 feeders) in the staging RAM (-45 ALMs; the sequencer's
timing of the reads changes); the eight attribute iterators `cur` as DSP accumulators (the adder, its 32-bit choice of operand and the 32
registers of each in a block; not tried: Quartus does not infer an accumulator without a product).

Verification: `rdp_regress.sh` 133 runs, 0 failed; on the 138 captured tasks (the bench's memory, and the SDRAM controller's timing), and the
133 configurations, each run's summary line (commands, cycles, reads, writes, pixels, RTL == model; with the SDRAM controller's rows,
turns and the other master's accesses) identical to main's: 408 runs; `TB_CMDT` (the clock each command word is taken) identical on 35
tasks and 14 random streams (also with the SDRAM controller, 8-bit and 32-bit images, copy mode, LOD, `TB_ZDZ`); the model and the oracle are
not touched, and `sim/rdp_oracle/run.sh 20` runs as before.
