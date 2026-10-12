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
| `rtl/rsp/rsp_lutram.v` | a 32-word LUT RAM: one element of the vector registers |

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
Both bits of a pair together leave the flag as it is. The single-step flag is
stored and read back; it does not change how the unit runs.

BREAK sets halt and broke, and raises the SP interrupt (`irq`) if interrupt on
break is set. After reset the RSP is halted.

### DMA

A transfer is rows of 8-byte units: the length word gives the row's length
less one (bits 11:0, taken up to a multiple of 8 bytes), the number of rows
less one (bits 19:12) and the bytes skipped in RDRAM between rows (bits 31:20,
of which the multiple of 8 is kept). It starts from the addresses last written
to SP_MEM_ADDR and SP_DRAM_ADDR; the SP pointer wraps inside its 4 KiB. After a
transfer SP_RD_LEN and SP_WR_LEN read the skip, a count of 0 and a length of
`0xFF8`.

The engine moves one 32-bit word per request on the RDRAM port (`r_valid`,
`r_we`, `r_addr`, `r_wdata`, `r_ready`, `r_rvalid`, `r_rdata`):

* into SP memory: a read request each cycle the memory accepts one; each word
  that comes back is written to DMEM or IMEM the cycle after it arrives;
* out of DMEM: DMEM is read 8 bytes at a time, one pair of words ahead, so a
  write request goes out each cycle the memory accepts one;
* out of IMEM: the two words of a pair are read one after the other on IMEM's
  second port, a pair every three cycles.

In the SoC the RDRAM port is port 3 of the SDRAM arbiter, and the 8 MiB of
RDRAM is SDRAM at `0x4380_0000`.

Who starts the transfer decides how it is sequenced:

* The microcode's MTC0 to SP_RD_LEN / SP_WR_LEN stays in E until its transfer
  is complete, so the microcode never observes its own DMA as busy.
* The host's write returns at once. Its length word goes into pending
  registers and SP_DMA_FULL is set; the transfer starts when the engine is idle
  (the next cycle, if it already is). The address registers are double: a
  write to SP_MEM_ADDR or SP_DRAM_ADDR while a DMA is running or queued belongs
  to the next transfer, and reads give the running transfer's pointers. A
  further length write while SP_DMA_FULL is set replaces the queued command.

A DMA into or out of DMEM shares DMEM's port with the program, and the DMA
has priority. A transfer in takes the port in each cycle it writes a word; a
transfer out takes it for the whole transfer. While the DMA has the port,
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
| D | decode; the register file (two copies) is read with the arriving instruction |
| E | forwarding, the ALU, branches and jumps, the DMEM address, the coprocessor instructions |
| M | load data |
| W | register write |

Branches and jumps resolve in E. The delay slot, then in D, always runs; the
instruction being fetched is squashed when the branch is taken. A load followed
by an instruction that uses its result stalls one cycle. A load or store
addresses 1, 2 or 4 bytes at any byte address in one DMEM access.

A coprocessor instruction goes out on `c_valid` / `c_ins` with the values of
rs and rt and holds E until `c_done`. `rsp_top` answers MFC0 / MTC0 itself and
passes the rest to the vector unit.

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

Not implemented: LHV, LFV, LWV, SHV, SFV, SWV, VRNDP, VRNDN, VMULQ, VMACQ and
the reserved COP2 functions. They change nothing and set `unimpl`.

### Four lanes

The original hardware operates on all eight 16-bit elements of a vector
register in one step (eight lanes); MIRLO 64 covers the same eight elements
with 4 lanes, as follows.

A lane is one signed 17 x 17 multiplier, one 16-bit adder / subtractor that
serves every add, subtract, compare and clip, the 48-bit accumulator adder,
the clamps and the flag logic (`lane_ex`). The unit has four of them. Next to
the four lanes stand four multiply-accumulate units (`lane_mul`): a
multiplier, the accumulator adder and the clamp, with none of the add, compare
or clip logic.

The storage is always eight elements wide. Each element of the register file
is three copies of a 32-word LUT RAM (`rsp_lutram`), read in the cycle: copy A
at vs and copy C at vt, addressed straight from the instruction word, for the
operands of a computation; copy B for stores, MFC2, MTC2 and load writes that
merge. The accumulator and the flags are registers for all eight elements. The
element broadcast of vt (each element its own, one element of each pair, one
of each half, or a single element for the whole register) is also formed for
all eight elements at once.

A computational instruction runs in two stages behind the scalar pipeline:
SEL takes operands and multiplies, EX a cycle later does the accumulator, the
clamps and the flags.

| Instruction | How the eight elements are covered | Cycles |
|---|---|---|
| multiplies | one pass: elements 0-3 on the four lanes, elements 4-7 on the four multiply-accumulate units | SEL, EX |
| every other computation (adds, logic, compares, clips, VABS, VSAR, VMOV) | two passes on the four lanes: elements 0-3, then elements 4-7 | SEL, SEL + EX, EX |
| VRCP, VRCPL, VRCPH, VRSQ, VRSQL, VRSQH | two passes as above for the accumulator; the ROM lookup runs alongside | SEL, SEL + EX, EX |

In detail, for a two-pass instruction:

1. First SEL (the instruction is in E): all eight elements of vs and of the
   broadcast vt are read. Elements 0-3 go through the multipliers into the EX
   registers; elements 4-7 are kept in `s_hi` / `t_hi`. `done` is raised in
   this cycle, so the instruction occupies E for one cycle.
2. Second SEL: the kept operands of elements 4-7 take the four lanes. In the
   same cycle the first pass's EX updates the accumulator and flags of elements
   0-3 and keeps its four results in `hold`.
3. Second EX: accumulator and flags of elements 4-7, and the register write.

vd is written once, with the last pass, all eight elements together (elements
0-3 from `hold`, elements 4-7 straight from the second EX), so vd may be the
same register as vs or vt. The reciprocals and VMOV write one element of vd.

Because the instruction has left E after its first SEL, the scalar unit goes
on while the passes finish. What follows waits only when it needs something
that is still busy:

* another computation waits for the second SEL, which occupies the four lanes,
  and for the write of a register it reads. Independent non-multiply
  computations therefore start every two cycles, independent multiplies every
  cycle;
* a reciprocal waits for the EX of any instruction before it (the divide
  registers are serial);
* MTC2, CFC2, CTC2, LTV and STV wait until nothing is under way; a store or
  MFC2 waits for a pending write of the register it reads; a load of another
  register starts under a running computation.

The register file has one write port. When the last pass's write would fall
in the cycle a load writes its register, the load has the port and vd is
written one cycle later, whole, from `hold`; a single-element write (a
reciprocal, VMOV) waits a cycle instead.

### Reciprocals

The input (16 bits, or 32 with the half a preceding VRCPH / VRSQH left) is
taken in the first SEL, normalised the cycle after, which gives the ROM's
address, and the ROM's word is shifted back in the last pass's EX. VRCPH and
VRSQH store the input's high half and return the high half of the previous
result.

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

MFC2, MTC2, CFC2 and CTC2 take one cycle.

## Memories

IMEM is 1024 words with two ports: the fetch, and the DMA (its writes on a
transfer in, its reads on a transfer out). Only a DMA writes IMEM.

DMEM is 4 KiB in `rsp_dpram.sv`: four banks of 256 words, each an `altsyncram`
in M10K, true dual port with byte enables. Bank k holds bytes 4k to 4k + 3 of
every 16-byte line, and each bank has its own row address, so one access on
port A reads or writes any 16 bytes that start at a word boundary. Read data
comes back the next cycle, rotated so that the vector unit finds its
register's bytes in place and the scalar unit its 1 to 4 bytes in the top
word. In simulation the same module is one array of 1024 words, which the
testbenches read.

Port A carries one access per cycle, in this order of priority: the DMA
engine, the vector unit (when the instruction in E is its own), the scalar
unit. Port B is the RDP's XBUS read.

The vector registers are LUT RAM (MLAB), as described above. The scalar
register file is two copies of a 32-word memory with synchronous read.

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
RSP through the CPU's device port, already on `clk_sys`.

Several details of the RTL exist for this clock:

* the host's register access is registered before it is performed;
* the DMA counts requests by rows instead of multiplying rows by row length;
* a vector load or store scales its offset first, so the address is one adder
  after rs;
* DMEM's 16-byte rotations are done in two steps, by words and then by bytes;
* the ROM, IMEM and DMEM reads are registered, and the reciprocal spreads over
  the cycles of its two passes.

In steady state the scalar unit completes one instruction per cycle. The
cycles above that come from the load-use stall, taken branches, vector
computations that follow each other, an MTC0 that waits for its DMA, and
`m_stall`.

## Verification

Everything specific to the RSP is in `sim/rsp`.

| File | What it is |
|---|---|
| `rsp.c`, `rsp.h` | the reference model in C: the scalar unit, COP0 with the DMA and the SP / DPC registers, the vector unit. `rsp_step` runs one instruction, `rsp_run` runs to BREAK. The RDP is not modelled: complete commands are passed to a callback. It also counts instructions by kind |
| `rsp_task.c`, `Makefile` | `rsp_task`: runs task dumps on the model and reports instructions per kind, DMA traffic and RDP commands; `-o` writes the RDP command words, the RDRAM and DMEM after the task |
| `tb_rsp.cpp`, `build_rtl.sh` | the RTL against the model in lockstep, in Verilator |
| `gen_rand_task.py` | a random program as a task dump |
| `tb_dma.cpp`, `build_dma.sh` | the host side of the DMA and the semaphore, on `rsp_top` alone |
| `perf_kinds.py` | sums a `PERF` log of `tb_rsp` by instruction or by family, optionally next to a second log |
| `rom_ucode_scan.py` | finds the microcode in a ROM image (also inside Yaz0 / Yay0 / MIO0 blocks) and lists its instructions against what the RTL implements |
| `make_div_rom.py` | the reciprocal ROM |

The model is the contract: the RTL must have the same state after each
instruction. The model covers slightly more than the RTL (it has LHV, SHV and
SWV, and gives the reserved functions a result).

### Lockstep bench

`MULW=1 sim/rsp/build_rtl.sh` builds `sim/rsp/obj_rsp/tb_rsp` with the vector
unit as the SoC instantiates it (the reciprocal ROM must have been generated
first). `tb_rsp task.bin ...` loads each task as the
console does, the header and the boot microcode by DMA through the host
registers, and then steps the model once for every instruction that leaves E.
It compares:

* the PC of every instruction;
* each scalar register write, when it reaches W;
* the vector registers, the accumulator and the flags, as each vector
  instruction's effects are complete (`tr_v`). Where a register write that was
  put off lands under the next computation's first pass, the registers are
  compared at once and the accumulator and flags with the following
  instruction (`tr_vp`);
* DMEM, every 64 instructions (`DEEP`) and at the BREAK;
* at the end, the whole RDRAM, the RDP's command words, and that the RTL has
  halted.

`RAM_LAT` sets the latency of an RDRAM read and `RAM_STALL` the percentage of
cycles in which the memory accepts no request. `KINDS=1` prints where the
cycles go by kind of instruction and `PERF=<file>` writes every instruction's
cycles. The summary line gives cycles per instruction and a time computed at
62.832 MHz. The exit status is non-zero on any mismatch.

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

`sim/rsp/build_dma.sh` builds `obj_dma/tb_dma` (4 lanes, `MULW` set). It
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
