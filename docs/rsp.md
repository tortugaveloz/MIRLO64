# The RSP

`rtl/rsp` is the RSP of MIRLO 64: the processor that runs the graphics and
audio microcode an N64 program brings with it. The microcode is not replaced or
translated; it executes instruction by instruction, and the graphics commands
it produces go to the RDP (`rtl/rdp`, [rdp.md](rdp.md)). The RTL and its
reference model (`sim/rsp/rsp.c`) are written from the public documentation of
the RSP.

* [Structure](#structure)
* [Interface to the system](#interface-to-the-system)
* [Scalar unit](#scalar-unit)
* [Vector unit](#vector-unit)
* [Memories](#memories)
* [Reciprocal ROM](#reciprocal-rom)
* [Clock and timing](#clock-and-timing)
* [Verification](#verification)

## Structure

| File | Contents |
|---|---|
| `rtl/rsp/rsp_top.sv` | IMEM, DMEM, the SP registers (COP0), the DMA engine, the reciprocal ROM; instantiates the two units |
| `rtl/rsp/rsp_su.sv` | the scalar unit: the RSP's MIPS subset on a five-stage pipeline |
| `rtl/rsp/rsp_vu.sv` | the vector unit: COP2 and the vector loads and stores (LWC2, SWC2) |
| `rtl/rsp/rsp_dpram.sv` | DMEM's memory: four banks of block RAM |
| `rtl/rsp/rsp_lutram.v` | a 32-word LUT RAM, 16 bytes wide with a write enable a byte: one copy of the vector registers |

`rtl/rsp.qip` lists them for Quartus. The SoC instantiates `rsp_top` once, in
`rtl/soc/mirlo_mips.sv`.

## Interface to the system

### Host port and registers

`rsp_top` has a register port for the game CPU (`h_we`, `h_rd`, `h_addr`,
`h_wdata`, `h_rdata`, `h_ready`). In the SoC it is mapped at `0x2000_0000`;
the CPU's access is first taken into registers and then performed.

| Offset | `h_addr` | Register | Notes |
|---|---|---|---|
| `0x00` | 0 | SP_MEM_ADDR | bits 12:3; bit 12 selects IMEM |
| `0x04` | 1 | SP_DRAM_ADDR | bits 23:3 |
| `0x08` | 2 | SP_RD_LEN | a write starts a DMA from RDRAM into DMEM / IMEM |
| `0x0C` | 3 | SP_WR_LEN | a write starts a DMA from DMEM / IMEM into RDRAM |
| `0x10` | 4 | SP_STATUS | see below |
| `0x14` | 5 | SP_DMA_FULL | a DMA is queued behind the running one |
| `0x18` | 6 | SP_DMA_BUSY | a DMA is running or queued |
| `0x1C` | 7 | SP_SEMAPHORE | a read returns the bit and sets it; a write clears it |
| `0x20` | 8 | SP_PC | writable while halted; a read gives the next instruction to run |
| `0x40`-`0x4C` | - | DPC_START, DPC_END, DPC_CURRENT, DPC_STATUS | held outside `rsp_top`, see below |

The microcode reaches the same registers with MFC0 / MTC0 (registers 0-7 the
SP ones, 8-11 the four DPC ones). When both write in the same cycle the
microcode goes first: `h_ready` is low while an MTC0 is in the scalar unit's E
stage, and the host's write waits.

SP_STATUS reads `{signals 7..0 [14:7], interrupt on break [6], single step [5],
0 [4], DMA full [3], DMA busy or full [2], broke [1], halt [0]}`. A write uses
the console's clear / set pairs: bit 0 clears halt and bit 1 sets it, bit 2
clears broke, bits 3 / 4 clear / set the SP interrupt, 5 / 6 single step,
7 / 8 interrupt on break, and from bit 9 on a clear / set pair for each signal.
Both bits of a pair together leave the flag as it is. With single step set the
RSP halts again as an instruction (or a pair) leaves E.

BREAK sets halt and broke, and raises the SP interrupt (`irq`) if interrupt on
break is set. After reset the RSP is halted.

### DMA

A transfer is rows of 8-byte units: the length word gives the row's length
less one (bits 11:0, taken up to a multiple of 8 bytes), the number of rows
less one (bits 19:12) and the bytes skipped in RDRAM between rows (bits 31:20,
of which the multiple of 8 is kept). It starts from the addresses last written
to SP_MEM_ADDR and SP_DRAM_ADDR; the SP pointer wraps inside its 4 KiB.
SP_RD_LEN and SP_WR_LEN read the running transfer's skip, the rows left less one
and the bytes left in its row less 8, as they count down; after a transfer a
count of 0 and a length of `0xFF8`, and 0 after reset.

The engine moves one 32-bit word per request on the RDRAM port (`r_valid`,
`r_we`, `r_addr`, `r_wdata`, `r_ready`, `r_rvalid`, `r_rdata`):

* into SP memory: a read request each cycle the memory accepts one; each word
  that comes back is written to DMEM or IMEM the cycle after it arrives;
* out of DMEM: DMEM is read 8 bytes at a time into a FIFO of two pairs, ahead
  of the pair being sent, so a write request goes out each cycle the memory
  accepts one;
* out of IMEM: the two words of a pair are read one after the other on IMEM's
  second port, a pair every three cycles.

In the SoC the RDRAM port is port 3 of the SDRAM arbiter, and the 8 MiB of
RDRAM is SDRAM at `0x4380_0000`.

The microcode's MTC0 to SP_RD_LEN / SP_WR_LEN and the host's write are taken
the same way, as on the console: the write returns at once, its length word
goes into pending registers and SP_DMA_FULL is set; the transfer starts when
the engine is idle (the next cycle, if it already is). The address registers
are double: a write to SP_MEM_ADDR or SP_DRAM_ADDR while a DMA is running or
queued belongs to the next transfer, and reads give the running transfer's
pointers. A further length write while SP_DMA_FULL is set replaces the queued
command. The microcode goes on while its transfer runs and waits for it
itself, on SP_DMA_BUSY or SP_DMA_FULL, as the console's microcode does.

A DMA into or out of DMEM shares DMEM's port with the program, and the DMA
has priority. A transfer in takes the port in each cycle it writes a word; a
transfer out in each cycle it reads a pair of words, into a FIFO of two pairs
ahead of the one being sent -- one cycle in two at most, so the microcode's
loads and stores go on between them. While the DMA has the port,
`m_stall` makes a scalar or vector load or store wait in E (each beat of LTV /
STV likewise). The stall comes from a register, known one cycle ahead, which
is why an arriving word is written a cycle after it arrives.

### DPC registers and XBUS

The DPC registers are not inside `rsp_top`. A microcode write to DPC_START,
DPC_END or DPC_STATUS leaves the module as a strobe (`dpc_start_we`,
`dpc_end_we`, `dpc_status_we`, `dpc_wdata`), and the four values it reads with
MFC0 come in as inputs. In the SoC `rtl/rdp/rdp_dpc.sv` holds the registers and
fetches the commands, `rtl/soc/rcp_dpc_view.sv` gives DPC_STATUS its layout,
and the CPU's own writes to `0x2000_0040`, `0x44` and `0x4C` are merged with
the microcode's strobes (a CPU write is taken in a cycle the microcode is not
writing a register). [rdp.md](rdp.md) describes that side.

When the RDP takes its commands from DMEM (XBUS) it reads them through
`dx_addr` / `dx_data`, DMEM's second port: one word, the cycle after its
address.

### Interrupt

`irq` goes to the game CPU's interrupt logic in `mirlo_mips.sv`. While the
CPU's monitor level does not own the interrupts it is ORed into the CPU's
interrupt line; while it does, `irq` reaches the CPU through the SoC's monitor
registers (`mon_sp`), which hold it as the SP bit of the MI. The runtime takes
it as IP2 (`N64RT_IM_SP`, `lang/n64rt/n64rt_os.c`).

`halted` and `unimpl` (a vector instruction that is not implemented has run;
it stays set until reset) are status outputs; the SoC routes both to a debug
word (`dbg_aud_state`).

### The runtime

`lang/n64rt/n64rt_rsp.c` is the runtime's side (`lang/n64rt/Makefile` builds
with `RSP=1 OWN=1`). It uses the registers at `0x2000_0000` directly.

* With the program's own libultra running, its loads and stores of the SP
  registers (`0x0404_0000`-`0x0404_001F`), SP_PC (`0x0408_0000`) and the DPC
  registers trap to the CPU's monitor level and are forwarded to the hardware
  registers (`n64rt_exc.S`, `rsp_rd` / `rsp_wr` in `n64rt_io.c`). The program
  therefore loads and starts its tasks itself, by DMA, as on the console.
* The CPU has no direct port into DMEM or IMEM. An access to
  `0x0400_0000`-`0x0403_FFFF` is done by `n64rt_sp_mem`: with the RSP halted,
  8 bytes are moved by DMA through two words of RDRAM, and SP_MEM_ADDR /
  SP_DRAM_ADDR are put back afterwards.
* `n64rt_gfx_init` fills DMEM by DMA with the first 4 KiB of the cartridge
  image, its first `0x40` bytes cleared: what the console's boot leaves there.
* `rsp_start` is the task path of `n64rt_io.c` (`n64rt_gfx_task`,
  `n64rt_audio_task`): the task header, its pointers made physical, goes by
  DMA to DMEM `0xFC0`, the boot microcode by DMA to IMEM, SP_PC is set to 0 and
  halt is cleared with interrupt on break set. `rsp_poll` takes the task's end
  from SP_STATUS; besides the interrupt, the timer polls every 0.5 ms while a
  task is under way. A graphics task is asked to yield with signal 0 and
  reports it with signal 1; a yielded task resumes from its yield data. A yield
  can be held back for `n64rt_yield_wait_ms` (0 by default): a graphics task
  that ends within that time is not stopped.

## Scalar unit

`rsp_su.sv` implements the RSP's integer instruction set with a 12-bit PC:

* SLL, SRL, SRA, SLLV, SRLV, SRAV, JR, JALR, BREAK, ADD, ADDU, SUB, SUBU, AND,
  OR, XOR, NOR, SLT, SLTU;
* BLTZ, BGEZ, BLTZAL, BGEZAL, J, JAL, BEQ, BNE, BLEZ, BGTZ;
* ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI;
* LB, LH, LW, LBU, LHU, LWU, SB, SH, SW;
* MFC0, MTC0, and COP2 / LWC2 / SWC2, which are passed to the vector unit.

There are no exceptions, no multiply or divide and no 64-bit operations; ADD,
ADDI and SUB behave as their unsigned forms. An opcode outside this list does
nothing.

The pipeline has five stages:

| Stage | Work |
|---|---|
| F | the PC goes to IMEM, which is synchronous: the word arrives the next cycle |
| D | decode; the register file (two copies, LUT RAM) is read with the arriving instruction: its address is registered, the word is read in E |
| E | forwarding, the ALU, branches and jumps, the DMEM address, the coprocessor instructions |
| M | load data |
| W | register write |

Branches and jumps resolve in E. The delay slot, then in D, always runs; the
instruction being fetched is squashed when the branch is taken -- the cycle of
nothing after a taken branch's delay slot that the console has too. A load or
store addresses 1, 2 or 4 bytes at any byte address in one DMEM access.

### Issue

This is the fast RSP: it keeps no rule of the console's timing beyond what
the results need. A load followed by an instruction that reads its value
stalls one cycle (and one after CFC2 and MFC2, whose value comes in M too); every other
result is forwarded. The operand an instruction takes is chosen by registers set a cycle ahead, for the instruction that will
be in E: the result in M, the one in W, or the register file, which is written at the end of W's cycle and read a cycle
after (E's own registers are read again while it waits). A branch's condition and a load or store's address are made from
each of the three sources and chosen, so that the choice is not in front of the compare or the adder. What the vector unit's instructions wait for it keeps
itself (a register a computation, a load or MTC2 has yet to write). The
console's own cycle counts, which `sim/rsp/rsp_time.c` models, are longer:
`tb_rsp CYCLES=1` shows by how much (slots issued early).

A coprocessor instruction goes out on `c_valid` / `c_ins` with the values of
rs and rt and holds E until `c_done`. `rsp_top` answers MFC0 / MTC0 itself and
passes the rest to the vector unit.

Two instructions issue together, as on the console, when one is a vector
computation and the other, next to it in either order, an instruction of the
scalar unit: an ALU op, a scalar load or store, a branch, jump or BREAK (only
second), and a vector load (LBV to LUV) or store
(SBV to SUV, SHV, SFV), MFC2, MTC2, CFC2 or CTC2. The rule is the console's: no
pair when the first writes a vector or control register the second reads or
writes (a one-lane op's lane field, or VNOP's vd behind MTC2, is taken for a
register, as on the console; a reserved function is taken to read vs and vt
and write vd, which the console's rule does not count, so that a pair's result
is the two in order); MFC0 and MTC0 do not pair (they are done in E, whatever the vector unit does). D's instruction must
not be a delay slot (a taken branch's target at an odd word pairs too, which the console's does not).
IMEM's second port gives D the word after its own; the pair enters E
together, the scalar instruction takes E's path as any other and the
computation goes to the vector unit on `c_*` in the same cycle (`c_pr`; the
scalar unit's own on `c_lins`, a load's or store's base on `c_a`), and the
fetch goes on two words further. The vector unit starts both in the same
cycle or neither. Both of D's words are decoded and the pair's choice picks
among them last, so the rule is not in front of E's decode. Whether two words can be a pair, by the rule alone, is not
worked out at the fetch: it is made as the second word is written into IMEM (`pair_mem` in `rsp_top.sv`: one bit a word,
true if it and the word before, written just before it by the same transfer, are a pair), and read with the second port's
word. A word written alone, and the word after a transfer's last, have none: a pair lost, never a wrong one. What is left
at the fetch is the unit's state (D valid, the second word read by the port, running, no branch in E).

A pair's load (or MTC2) writes its register the cycle after; if that is the cycle of a computation's write, the
computation's goes a cycle late, from `hold` (`wp`), with its byte enables, whatever its width.

BREAK stops the unit as it leaves E: the instruction after it is not executed,
and `pc_next` names it. While halt is set nothing enters E.

## Vector unit

`rsp_vu.sv` holds the state the instruction set defines: 32 registers of eight
16-bit elements, a 48-bit accumulator per element, VCO and VCC (16 bits each),
VCE (8 bits) and the divide registers.

It implements:

* the multiplies VMULF, VMULU, VMUDL, VMUDM, VMUDN, VMUDH and their
  accumulating forms VMACF, VMACU, VMADL, VMADM, VMADN, VMADH;
* VADD, VSUB, VADDC, VSUBC, VABS, VSAR;
* VAND, VNAND, VOR, VNOR, VXOR, VNXOR;
* VLT, VEQ, VNE, VGE, VMRG, VCL, VCH, VCR;
* VRCP, VRCPL, VRCPH, VRSQ, VRSQL, VRSQH, VMOV, VNOP, VNULL;
* MFC2, MTC2, CFC2, CTC2;
* the loads LBV, LSV, LLV, LDV, LQV, LRV, LPV, LUV, LTV and the stores SBV,
  SSV, SLV, SDV, SQV, SRV, SPV, SUV, STV, at any address and element.

SHV and SFV (the packed stores of every second and every fourth byte) take one
cycle; SFV only with an element that is a multiple of 4. Not implemented:
LHV, LFV, LWV, SWV, SFV with another element, VRNDP, VRNDN, VMULQ and VMACQ.
They change nothing and set `unimpl` (no microcode in the ROMs scanned with
`rom_ucode_scan.py` has them).

### Lanes and the pipeline

The vector unit operates on all eight 16-bit elements of a vector register at once (eight lanes; `LANES` is a parameter
of the module's ports only: the unit is built for eight). A lane is one signed 17 x 17 multiplier, one 16-bit adder /
subtractor that serves every add, subtract, compare and clip, the 48-bit accumulator adder, the clamps and the flag
logic. The storage is eight elements wide: the register file is three copies of a 32-word LUT RAM (`rsp_lutram`), each a
whole register wide and written a byte at a time (a load or MTC2 writes its bytes over a register's and reads nothing).
Copy A is read at a computation's vs, C at its vt, and D at the register a store or MFC2 reads (or the one STV has
reached), so that a pair's computation and store each have theirs. Each memory registers its read address itself, a cycle
ahead: the scalar unit gives the addresses of the instruction that E holds next. The accumulator and the flags are
registers for all eight elements. The element broadcast of vt is formed for all eight elements at once.

A computation goes through SEL (in E: the operands, from the registers, into `s_r` and `t_r`) and then, by its kind:

| Kind | Instructions | After SEL |
|---|---|---|
| multiply | VMULF .. VMADH | MUL (the products, in the DSP blocks, into `p_r`), EX1 (the product into the 48-bit accumulator, and whether the sum is above or below what a word holds), EX2 (the clamp, and vd) |
| fast | VMRG, VMOV, the logic operations, VSAR, VRCPH, VRSQH | EX2 at once: there is no adder in them |
| add | VADD, VSUB, VADDC, VSUBC, the reserved functions | EX1 (the adder: its sum and flags into `lo_r` and `fl_r`, and whether the result is the clamp's constant), EX2 (the result, the accumulator's low word, the flags, vd) |
| compare, clip, reciprocal | VLT .. VNE, VCH, VCL, VCR, VABS, VRCP, VRCPL, VRSQ, VRSQL | EX1 as an add, and in it the choice among s, t and -t (`pk_r`); a clip's EX1 has a second cycle in which the adder makes -t, and a reciprocal's is its normalisation; then EX2 |

EX2 takes a computation a cycle, in the order of SEL, so vd, the accumulator and the flags are written in order and
nothing is forwarded: a multiply reads and writes the accumulator in its EX1 and the rest in EX2, which are two cycles or
more apart for neighbours, and the flags are written in EX2 and read in EX2 and, for a carry (VADD, VSUB) or a compare, in
EX1, where the next computation therefore does not start while another is in its EX1. A computation that uses s and t again
in EX2 (a compare, a clip, VABS, a reciprocal) keeps SEL from replacing them. What can start behind what is in MUL and EX1:
a fast one when both are empty; a multiply unless EX1 holds a computation that uses s and t, or is kept another cycle; any
other when MUL is empty, the first is not busy with s and t, and (if it reads the flags in EX1) no other computation is in
EX1. A computation that reads the register another is writing (in MUL, EX1 or EX2) waits for its write.

The cost of that, against the unit before it had stages, is in the cycles of the task dumps: a multiply's vd is written 3
cycles after its SEL (it was 1), the others' 2 (or 1, the fast ones), and a computation that uses s and t in EX2 holds SEL a
cycle. The functions' decode is registered with the instruction (`cdecode`, `ydecode`), made from the instructions entering
E, and the register file's write enable, a clip's or a compare's choice, the clamp's constant and a multiply's clamp flags
are made in EX1, so that EX2 is a selection.

### Reciprocals

The input (16 bits, or 32 with the half a preceding VRCPH / VRSQH left) is
taken in SEL; EX1's first cycle makes its absolute value and its second normalises it, which gives the ROM's address; EX2
shifts the ROM's word back, and the write of vd is put off a cycle (`wp`) as the result is late in the cycle. VRCPH and
VRSQH store the input's high half and return the high half of the previous result: they are fast, and the divide registers'
state after each reciprocal instruction (the high half kept, whether it is waiting) is set in SEL, in program order, so that
nothing is forwarded.

The reserved COP2 functions give the console's result: vd 0, the
accumulator's low word vs + vt (the lanes' adder, as VADDC), the flags kept.

### Loads, stores and moves

Every load and store except LTV and STV is one DMEM access in the
instruction's first cycle: the bytes it moves lie within the 16 bytes that
start at the word of its address (LQV / SQV up to the end of the 16-byte block,
LRV / SRV from the block's start, a packed load within the 16 bytes from its
8-byte block). DMEM's bytes and the register's bytes are a rotation of each
other, and byte enables select what is written. A load's data arrives the cycle
after its address and is written into the register then; a load that leaves
half of an element unchanged reads the register on copy B for the merge.

LTV and STV move sixteen bytes between DMEM and the eight registers of vt's
group, one byte per cycle.

MFC2, MTC2, CFC2 and CTC2 take one cycle in E. MFC2's bytes are read in E (copy D) and its value goes to the scalar unit the
cycle after, in M, as CFC2's does (the instruction behind that reads the register waits a cycle); MTC2 writes its bytes the
cycle after, as a load does. CFC2 and CTC2 start when no computation is under way (the flags are final then); CTC2 writes
the flag register the cycle after. A load's byte mask is made in the cycle of its write from the address's low bits, the
element and the size, which are kept (`lw_o`, `lw_el`, `lw_op`), and whether it leaves half of a lane unchanged (a store or
MFC2 does not start under such a write) from the parities of the mask's ends.

A store is not written in the cycle it is in E: its address, byte enables and data, before the rotation to DMEM's bytes, are
taken into a buffer (`sw_*`, `rsp_top.sv`) and written the cycle after, from registers. A load waits for that cycle; a store
does not (the buffer takes the next as the first is written), and the DMA's cycles are waited for by both.

## Memories

IMEM is 1024 words with two ports: the fetch, and the DMA (its writes on a
transfer in, its reads on a transfer out) -- or, with no DMA of IMEM's, the
word after the fetch's, for a pair. Only a DMA writes IMEM.

DMEM is 4 KiB in `rsp_dpram.sv`: four banks of 256 words, each an `altsyncram`
in M10K, true dual port with byte enables. Bank k holds bytes 4k to 4k + 3 of
every 16-byte line, and each bank has its own row address, so one access on
port A reads or writes any 16 bytes that start at a word boundary. Read data
comes back the next cycle, rotated so that the vector unit finds its
register's bytes in place and the scalar unit its 1 to 4 bytes in the top
word. In simulation the same module is one array of 1024 words, which the
testbenches read.

Port A carries one access per cycle, in this order of priority: the DMA
engine, a store waiting in the store buffer, the vector unit (when the instruction in E is its load or store), the scalar
unit. Port B is the RDP's XBUS read.

The vector registers and the scalar register file are LUT RAM (MLAB), with their read addresses registered.

## Reciprocal ROM

The ROM is 1024 words of 16 bits in `rsp_top.sv`: 512 reciprocal entries,
then 512 inverse-square-root entries. Its read is registered. An address is
the nine bits after the leading one of the normalised input; for a square root
the lowest of them is replaced by the parity of the shift.

`sim/rsp/make_div_rom.py` computes the table, with the same formulas as
`make_roms` in `sim/rsp/rsp.c`, and writes it one word per line:

```
python3 sim/rsp/make_div_rom.py rtl/rsp/rsp_div_rom.hex
```

`rtl/rsp/rsp_div_rom.hex` is generated, not kept in the repository. The ROM is
loaded with `$readmemh` from the `DIV_ROM` parameter; for the bitstream,
`tools/mips_inits.py` writes the file's absolute path into the generated
`rtl/soc/mirlo_mips_init.svh` (`MIPS_INIT_RSP_DIV`). The testbench build
scripts pass the path themselves.

## Clock and timing

The whole RSP is on one clock with a synchronous reset. In the SoC that clock
is `clk_sys`, the system clock shared with the SDRAM, the RDP and the scan-out:
62.5 MHz (`SYS_HZ` in `target/pocket/core_top.sv`; [mips.md](mips.md) has the
clocks). The game CPU runs on its own clock; its register accesses reach the
RSP through the CPU's device port, already on `clk_sys`. A SoC built with
`RSP_OC` puts the RSP on the CPU's clock, 93.75 MHz, with the edges the
setting leaves out removed by a clock gate; the host port, the DMA and the DPC's
strobes and registers then cross to `clk_sys` in `rtl/soc/rsp_xing.sv`, and DMEM's
port b (the RDP's XBUS reads) stays on `clk_sys` (`DUALCLK`) -- see
[mips.md](mips.md), "The RSP's clock".

The RSP is built to meet the CPU's clock (93.75 MHz, 10.667 ns) as well, so that it can run on it. Run alone in an empty
device (`tools/area2/ooc.sh rsp`, `CLK_NS=10.667 STA=1`) a fit of the RSP before the work below had a worst setup slack of
-4.6 to -5.2 ns in the slow corners (fmax about 65 MHz); the paths, all of them mostly routing, were the vector registers'
write data, the scalar unit's ALU and addresses, IMEM's address (the pair rule), DMEM's address and data, the branch compare.
What was done for them:

* the fetch address is a choice between registers; the pair rule is a bit made when IMEM was written;
* the vector unit's decode is registered with the instructions (from the instructions entering E), the computations are in
  stages (above), a compare's choice, the clamp's constant and a multiply's clamp flags are made in EX1, a reciprocal's
  result is written a cycle late, and the register file's write enable and a load's mask are made from registers;
* the scalar register file is LUT RAM, the operands are chosen by registers set ahead and a branch's condition and an
  address are made from each source;
* a store is written from a buffer, the address of the next line is made beside the address, and the scalar unit has a
  left shifter of its own.

Other details of the RTL exist for the clock in general:

* the host's register access is registered before it is performed;
* the DMA counts requests by rows instead of multiplying rows by row length;
* a vector load or store scales its offset first (registered with the instruction), so the address is one adder after rs;
* the ROM, IMEM and DMEM reads are registered.

In steady state the scalar unit completes one instruction per cycle, two in a
pair. The cycles above that come from the load-use stall, taken branches
(forward ones; and backward ones not taken), a register a computation reads
before the one before has written it (a multiply's vd is written 3 cycles after its SEL), a computation that holds SEL for its
EX1, the vector unit's one register write port (a computation's write and a load's in the same cycle put the
computation's off a cycle, `wp`), a clip's or a reciprocal's second EX1 cycle, a load after a store, LTV and STV (a byte a
cycle), an MTC0 that waits for its DMA, and `m_stall`.

## Verification

Everything specific to the RSP is in `sim/rsp`.

| File | What it is |
|---|---|
| `rsp.c`, `rsp.h` | the reference model in C: the scalar unit, COP0 with the DMA and the SP / DPC registers, the vector unit. `rsp_step` runs one instruction, `rsp_run` runs to BREAK. The RDP is not modelled: complete commands are passed to a callback. It also counts instructions by kind |
| `rsp_task.c`, `Makefile` | `rsp_task`: runs task dumps on the model and reports instructions per kind, DMA traffic and RDP commands; `-o` writes the RDP command words, the RDRAM and DMEM after the task; `-t` the cycles the task takes on the console (`rsp_time.c`) and where they go |
| `rsp_time.c`, `rsp_time.h` | the console's timing beside the model: in which cycle the N64 issues each instruction, by the pipeline's rules as the n64brew wiki gives them (pairs, latencies, the store after a load, the branch's bubble); the DMA on a timeline of the console's rate |
| `tb_rsp.cpp`, `build_rtl.sh` | the RTL against the model in lockstep, in Verilator |
| `gen_rand_task.py` | a random program as a task dump |
| `tb_dma.cpp`, `build_dma.sh` | the host side of the DMA and the semaphore, on `rsp_top` alone |
| `perf_kinds.py` | sums a `PERF` log of `tb_rsp` by instruction or by family, optionally next to a second log |
| `rom_ucode_scan.py` | finds the microcode in a ROM image (also inside Yaz0 / Yay0 / MIO0 blocks) and lists its instructions against what the RTL implements |
| `make_div_rom.py` | the reciprocal ROM |

The model is the contract: the RTL must have the same state after each
instruction. The model covers slightly more than the RTL (it has LHV and SWV).

### Lockstep bench

`sim/rsp/build_rtl.sh` builds `sim/rsp/obj_rsp_8/tb_rsp` with the vector
unit as the SoC instantiates it (the reciprocal ROM must have been generated first). `tb_rsp task.bin ...` loads each task as the
console does, the header and the boot microcode by DMA through the host
registers, and then steps the model once for every instruction that leaves E.
It compares:

* the PC of every instruction (a pair leaving E together: the model runs
  the two, in order);
* each scalar register write, when it reaches W;
* the vector registers, the accumulator and the flags, whenever the vector unit
  is quiescent (`tr_v`: nothing under way and nothing starting), against the model's state after the last vector
  instruction that left E -- the unit is a pipeline, and its registers, accumulator and flags are not all written in the
  cycle an instruction is finished -- and at the end; `VWIN=1` lists the instructions since the last quiescence when it
  differs, `VDRAIN=a,b` makes the unit take one computation at a time for the instructions a to b (to find which pair of
  instructions goes wrong), `VTRACE=n` prints the unit's stages for a few cycles at instruction n;
* DMEM, every 64 instructions (`DEEP`) and at the BREAK, with no DMA running
  or queued (the model does a transfer whole at its length's write; the RTL's
  runs behind the microcode, which reads SP_DMA_BUSY and SP_DMA_FULL as the
  RTL has them);
* at the end, the whole RDRAM, the RDP's command words, and that the RTL has
  halted.

`CYCLES=1` holds the RTL to the console's timing as well (`rsp_time.c`), cycle
by cycle: a slot (an instruction, or a pair) that leaves E before the console's
rules let it, a cycle in which E waits with nothing in those rules to wait for
(a cycle the DMA holds DMEM's port excepted), and a slot paired where the
console's is not or the other way round are counted by kind of instruction and
the first few printed; the timing model then follows the RTL.

`RAM_LAT` sets the latency of an RDRAM read and `RAM_STALL` the percentage of
cycles in which the memory accepts no request. `KINDS=1` prints where the
cycles go by kind of instruction and `PERF=<file>` writes every instruction's
cycles. The summary line gives cycles per instruction and a time computed at
62.5 MHz. The exit status is non-zero on any mismatch.

The inputs are of two kinds:

* Task dumps of microcode tasks captured from commercial software: a 16-byte
  header (`RSPT` and the task header's RDRAM address) followed by the 8 MiB of
  RDRAM at the moment the task starts. They are written by `sim/mips_soc` run
  with `RSPDUMP=<dir>` on a runtime built with `RSPDUMP=1`. They contain
  the software's data and are not part of the repository.
* `gen_rand_task.py SEED OUT.bin [N]`: straight-line programs that fill DMEM
  and the vector registers with random data and then run random instructions
  of every kind the RTL implements, with any element, address and alignment.
  Captured tasks exercise a small part of what the instructions can do; the
  random programs are for the rest.

### DMA bench

`sim/rsp/build_dma.sh` builds `obj_dma/tb_dma` (8 lanes). It
checks, and prints `PASS` or `FAIL`:

* a second host DMA written while the first runs is queued, SP_DMA_FULL and
  the STATUS bits read accordingly, and both transfers are correct;
* a third command while one is queued replaces it;
* SP_SEMAPHORE reads 0, 1, 1 and a write clears it;
* a host DMA into DMEM, and one out of it, while the RSP runs a scalar load /
  store loop: no access of the program is lost and no DMA word is wrong;
* the same with a loop of vector loads and stores (LQV, SQV, LTV, STV),
  compared against a run without a DMA.

### With the rest of the system

* `sim/rdp` (`DPC=1 build_rtl.sh`, `tb_dpc`): the RSP together with
  `rdp_dpc.sv` against the model; the command words the fetch hands on must be
  the model's, in order, with random memory latency and back-pressure.
* `sim/mips_soc`: the whole SoC. `lang/mips/test/rsp_replay.c` replays one
  task dump there, started the way the runtime does it, and times the RSP to
  its BREAK and the RDP to its SYNC FULL. `lang/mips/test/rcpcpu_test.c`
  checks the CPU's side of the SP and DPC registers: the semaphore, the DMA
  queue, and the CPU's writes of the DPC registers.
