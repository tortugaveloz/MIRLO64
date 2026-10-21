// Mirlo-N64's CPU: a MIPS that executes an N64 ROM's code as it is.
//
// The reference is sim/n64/cpu.c; sim/mips checks this against it
// instruction by instruction while both run a ROM.
//
//  - The VR4300's instructions with 32-bit addresses: MIPS II, the TLB,
//    ERET, ldc1/sdc1 of the FPU's register pairs, and its 64-bit integers
//    by levels, what a level leaves out being a Reserved Instruction for
//    the runtime to do on the registers it finds:
//      W64      registers of 64 bits; ld, sd, lwu, dmfc0/dmtc0; the branches
//               on all 64 bits. An and / or / xor / nor / slt or a trap
//               whose operand is not a 32-bit value is left out (D finds it).
//      W64_ALU  a unit at M: dadd(i)(u), dsub(u), and those and / or / xor /
//               nor / slt
//      W64_SH   (in the unit) the d-shifts, a byte or a bit a cycle
//      W64_MD   (with the unit) dmult(u): four products of the operands'
//               halves on the DSP block the 32-bit multiply has, summed a
//               word a time (8 cycles in the background, as the VR4300's);
//               HI and LO of 64 bits
//      W64_DIV  (with W64_MD) ddiv(u), a bit a cycle on a divider twice as
//               wide: left out (a Reserved Instruction), the runtime divides
//    Without W64_DIV, a ddiv of two words and a ddivu of two unsigned words
//    are done on the 32-bit divider; another is a Reserved Instruction.
//    Never in the core: ldl, ldr, sdl, sdr, lld, scd, a trap on a 64-bit
//    operand. COP0 register 7 (MCTL)
//    reads the levels in its bits 31 (ALU), 30 (SH), 29 (MD), 28 (DIV), 0 on
//    a core before them. Without W64_MD HI and LO
//    are 32 bits and the runtime keeps a 64-bit product or quotient itself:
//    it sets COP0 register 7's bit 0, and mfhi / mflo / mthi / mtlo are
//    Reserved Instructions until the next multiply or divide clears it.
//    Without W64 every 64-bit instruction is one and registers are 32 bits.
//  - The 64 bits are beside the pipeline, not in it. A register's upper half
//    is kept as its difference from the lower half's sign (hx = upper ^
//    {32{lower[31]}}): zero for a 32-bit value. E computes 32 bits, as ever.
//    What makes or needs a 64-bit value runs at M in the unit, a "late"
//    result like a load's. An hx is read from the register file only -- an
//    instruction waits in D while one of its registers is still on its way
//    from the unit or a 64-bit load -- so nothing of it is forwarded.
//  - The TLB, three ways. TLB_EN: the VR4300's 32 entries (tlbwi, tlbwr,
//    tlbp, tlbr; searched one a cycle, see below). Neither: none -- a mapped
//    access is the runtime's to do (with N64WIN it arrives as an address
//    error). TLB_RAM: translation tables of the runtime's making, in block
//    RAM -- one for data and one for fetches (an instruction and what it
//    reaches may share an index: with one table neither could stay), each of
//    512 entries, one for a 4 KiB page by its number's low 9 bits
//    (va[20:12]): {valid, dirty, global, ASID, va[31:21], the physical page
//    pa[28:12]}. A mapped access or fetch reads its page's entry: one that is
//    valid, of this page and this ASID (EntryHi's, unless global) translates
//    it; anything else is the TLB's exception (TLBL or TLBS: code 2 or 3; a
//    store to an entry that is not dirty: Mod, code 1) with BadVAddr the
//    address and EntryHi its page pair, as the VR4300 leaves them, for the
//    runtime to fill the entry from what the game has mapped -- a page of
//    any size, 4 KiB of it at a time -- and return to the instruction.
//    With MON a fault that goes to the monitor level leaves BadVAddr and
//    EntryHi as the game has them (a page the game's wired entry maps misses
//    the tables where the VR4300's TLB never would: an exception handler
//    that runs from a mapped page must not have a miss on its first fetch
//    replace the BadVAddr it is about to read): the address is COP0 register 22's,
//    which only reads, and the monitor level writes BadVAddr by mtc0 for the
//    exception it delivers to the game. An
//    entry is written through COP0 register 23: a write stores the entry of
//    EntryHi's page (the pair's page by the value's bit 4) and ASID:
//      bits 28-12 pa[28:12]    bit 4 va[12]    bit 3 the table (1: fetches)
//      bit 5 cacheable (a fetch from the page may use the I-cache: FPG)
//      bit 2 global    bit 1 dirty (a store is allowed)    bit 0 valid
//    and it reads 0x5442_0000, with bit 3 set when the last TLB exception
//    was a fetch's (its entry is the fetch table's) and bit 0 set on a core
//    with MON: 0 on a CPU without the tables. The VR4300's own TLB
//    instructions do nothing there -- but with MON, while the game runs
//    (MCTL.GAME), tlbwi, tlbwr, tlbr and tlbp are a Reserved Instruction for
//    the monitor level, which keeps the 32 entries itself and does them as
//    the VR4300 would (Index's P bit, which only the monitor level can write
//    by mtc0, for a tlbp that finds none). The monitor level also writes, by
//    mtc0 and while ERL is set, Cause's BD and ExcCode and Context's BadVPN2:
//    a page no entry of the game's maps is a TLB exception of the game's, which
//    the monitor delivers (the CPU's own EPC / Cause / vector work, done by
//    software: lang/n64rt/n64rt_os.c, tlb_deliver). With N64WIN code fetched
//    from a mapped page is the game's, as KSEG0's. A fetch from a mapped page runs through the I-cache when its
//    fetch-table entry has the cacheable bit and FPG gave the CPU its page registers (see "The page registers"
//    above F): two registers keep the last two such pages, so the table is read (four cycles) only when a fetch
//    changes to a third page; any write to COP0 23 empties them.
//  - Five stages: F (I-cache), D (decode, registers, branches -- a delay slot
//    costs nothing), E (ALU), M (D-cache, the slow path, COP0/COP1,
//    exceptions), W (write-back). Forwarding into E from M and W; a branch
//    in D waits for a result still in E, a load's for one cycle.
//  - Caches: 16 KiB I, 8 KiB D, direct mapped, 32-byte lines, physically
//    tagged; KSEG0 RDRAM only. The D-cache writes through an 8-entry write
//    buffer: memory is always current for the engines that read it (the RSP
//    firmware, MRDP, the audio core). A read goes out only with the buffer
//    empty. ld, sd, ldc1 and sdc1 to cached memory are two words in two
//    cycles, through the cache and the write buffer as any other. Uncached,
//    device and TLB-mapped accesses (but a fetch from a cacheable mapped page,
//    FPG) take the slow path (one single-word access; two for a 64-bit one).
//  - Memory is word-native (sim/n64/n64.h): big-endian byte k of a word is
//    lane 3 - k (BIG; little-endian: lane k).
//  - COP1 instructions run at M in fpu_n64 (single precision, doubles
//    through float); the operations it does not implement are Reserved
//    Instructions.
//  - FAST: the same five stages with their longest paths cut, for a clock
//    half as fast again as the rest of the SoC's.
//      F  the I-cache's word goes into D before its tag is compared: one of
//         another line is dropped there a cycle later (d_fmiss), the line is
//         read and the fetch made again.
//      D  a branch is not decided there. The fetch follows a guess -- a
//         jump's target, a backward branch taken, a forward one not, a
//         register jump not -- and the branch is decided at E on forwarded
//         operands, as any other instruction's (it no longer waits in D for
//         them). A wrong guess is put right in the cycle after (rd_res,
//         rd_fix): what was fetched behind the delay slot is dropped, two
//         cycles lost; a likely branch not taken drops its delay slot there.
//      M  a load takes the D-cache's word before its tag is compared: one of
//         another line is not written back, the line is read and the
//         instruction run again from its fetch (rp_pend), which costs
//         nothing more -- the line takes longer to come than the instruction
//         to return. A load in a delay slot cannot be fetched again without
//         its branch: it waits a cycle for its tag instead (m_w1).
//    Nothing the caches' tags say reaches the pipeline's advance.
`default_nettype none

module mips_core #(
    parameter int TLB_N = 32,               // the VR4300's; in a RAM, so the count costs no logic
    // 0: no TLB -- as if it never held a valid entry: a mapped access takes
    // the refill exception, TLB writes are dropped, tlbp never finds one. For
    // games that map nothing -- NOT SM64: Goddard's title-screen head maps
    // 0x04000000 (n64sim: 200,897 mapped accesses in the first 3 G cycles).
    parameter bit TLB_EN = 1,
    // the translation table in block RAM, filled by software (see the top);
    // not with TLB_EN
    parameter bit TLB_RAM = 0,
    // the cached span of KSEG0 from physical 0, as a power of two: the N64's
    // RDRAM (8 MiB)
    parameter int CACHE_LOG2 = 23,
    // native Mirlo (rtl/soc/mips_sys.sv): no KSEG0/KUSEG/KSEG2 --
    // every address is physical but KSEG1's (the boot ROM, 0xBFC0_0000);
    // SDRAM at 0x4000_0000 (64 MiB) is cached, everything else uncached; the
    // exception vector with BEV 0 is EXC_BASE + 0x180. MIRLO's RISC-V map.
    parameter bit FLAT = 0,
    parameter logic [31:0] EXC_BASE = 32'h8000_0000,
    // byte order: 1 big-endian (the N64: byte k of a word is lane 3 - k),
    // 0 little-endian (native Mirlo, as its other cores: lane k)
    parameter bit BIG = 1,
    // native Mirlo (FLAT) running an N64 game's code as it is: KSEG0 and KSEG1
    // 0x8000_0000 / 0xA000_0000 + 8 MiB are the N64's RDRAM, at N64_RDRAM in
    // SDRAM (KSEG0 cached, KSEG1 not). An instruction fetched from KSEG0's
    // window is the game's: its loads and stores outside the window -- the
    // RCP's registers, the cartridge, PIF RAM -- take an address error for
    // Mirlo's runtime to emulate (BadVAddr holds the address).
    parameter bit N64WIN = 0,
    parameter logic [31:0] N64_RDRAM = 32'h4380_0000,
    // MON: a game that runs its own libultra (N64WIN, FLAT). COP0 register 7's
    // bit 1 (MCTL.GAME) then sends the exceptions a console would take to the
    // game's own handler, 0x8000_0180, and keeps the rest -- a device access,
    // an instruction the core does not have, a translation fault -- for the
    // runtime in a monitor level of its own: ERL (Status bit 2), with
    // ErrorEPC to come back to, neither EXL nor EPC nor Cause touched, so it
    // can interrupt the game's handler too. irq_mon takes that level (not
    // masked by the game: only by ERL itself). MCTL (reg 7) reads:
    // [0] hilo_soft, [1] GAME, [2] the monitor's entry was in a delay slot
    // (ErrorEPC is the branch), [7:3] its cause: an exception's code, 31 an
    // irq_mon. The runtime's own vector, EXC_BASE + 0x180, is the monitor's.
    parameter bit MON = 0,
    // MON: the MI's block (0x0430_0000..: physical bits [28:20] = 0x043) and, PIW, PI_STATUS (0x0460_0010) are the
    // device port's (rtl/soc/rcp_mi.sv: the MI and the PI engine in hardware) for the game's word loads and stores,
    // once the runtime has set MCTL's bit 8 (bit 9 says the write is meant): no address error for the runtime.
    // Every other access to the RCP's registers is still the runtime's. COP0 7 reads bit 8 as the runtime set it
    // (0 on a CPU without the parameter).
    parameter bit MI = 0,
    parameter bit PIW = 0,
    // MI: and the VI's block (0x0440_0000..: rtl/soc/rcp_vi.sv) the same way
    parameter bit VIW = 0,
    // MI: and the AI's block (0x0450_0000..: rtl/soc/rcp_ai.sv)
    parameter bit AIW = 0,
    // MI: and the SI's registers (0x0480_0000..: rtl/soc/rcp_si.sv) and PIF RAM (0x1FC0_07C0..0x1FC0_07FF: only the compare of
    // physical bits [28:20] = 0x1FC and [10:6] = 0x1F, the window's 64 bytes repeated every 2 KiB) the same way
    parameter bit SIW = 0,
    // MI: and the cartridge's bus (rtl/soc/rcp_mi.sv's PI2: the PI's engine does the access): domain 2, 0x0800_0000..0x0FFF_FFFF
    // but its 0x0801_xxxx (a FlashRAM's command register: the runtime's), and the ROM, 0x1000_0000..0x12FF_FFFF
    parameter bit PI2W = 0,
    // PI2W: and the ROM's last 16 MiB too, 0x1300_0000..0x13FF_FFFF (a ROM of up to 64 MiB: mirlo_mips.sv)
    parameter bit ROM64W = 0,
    // the VR4300's 64-bit integer registers and instructions (see above)
    parameter bit W64 = 1,
    parameter bit W64_ALU = 0,
    parameter bit W64_SH = 0,
    parameter bit W64_MD = 0,
    // (with W64_MD) ddiv and ddivu, a bit a cycle on a divider twice as wide: left out (a Reserved
    // Instruction, the runtime's) unless this is set -- dmult(u) are the DSP's either way
    parameter bit W64_DIV = 0,
    // for a clock half as fast again (docs/mips.md): what the caches hold is
    // used before its tag is looked at, and a branch's compare no longer
    // reaches the next fetch
    parameter bit FAST = 0,
    // the caches' lines, as powers of two: 512 of the I-cache's (16 KiB) and
    // 256 of the D-cache's (8 KiB) are the VR4300's; more of them are block RAM
    // and a few address bits (the tags stay as wide: pa[28:14], pa[28:13])
    parameter int IC_LOG2 = 9,
    parameter int DC_LOG2 = 8,
    // the write buffer is outside (rtl/soc/cpu_port.sv, where the core's clock
    // is its own): every store leaves by st_* as it is done -- one for the
    // SDRAM as the write buffer would have taken it, or (st_dev) a device's,
    // which then waits for st_done -- and the memory port only reads. What is
    // outside keeps the order: a read is of memory as every store before it left it
    parameter bit XWB = 0,
    // TLB_RAM with FAST (FLAT, N64WIN): two page registers on the fetch side -- the last two pages of the fetch
    // table that were cacheable (the entry's C bit, COP0 23's bit 5) -- and a fetch from such a page runs through
    // the I-cache like KSEG0's (see "The page registers" above F). 0: a mapped fetch is always the slow path.
    parameter bit FPG = 0,
    // the D-cache drops a line another master writes into (FLAT, the SDRAM's 64 MiB): sn_t turns over for each of
    // those writes, sn_line is its line (pa[25:5]); see "The snoop" below the cache RAM writes
    parameter bit SNOOP = 0,
    parameter bit SIM_COUNT_RETIRE = 0      // Count/Random advance per executed instruction (co-simulation)
) (
    input  wire         clk,
    input  wire         rst,
    input  wire  [31:0] reset_pc,
    // memory: physical N64 addresses, word-native data
    output logic        mem_req,
    output logic [31:0] mem_addr,
    output logic        mem_we,
    output logic [31:0] mem_wdata,
    output logic [3:0]  mem_wstrb,
    output logic        mem_line,           // an 8-word line read
    output wire         mem_wrun,           // a write of the write buffer's run (the next may follow it at once)
    // XWB: the stores
    output wire         st_valid,
    output wire         st_dev,
    output wire  [31:0] st_addr,
    output wire  [31:0] st_data,
    output wire  [3:0]  st_strb,
    input  wire         st_full,            // none is taken (st_valid is not given then)
    input  wire         st_done,            // the device's store is done
    input  wire         mem_ack,            // the request is taken (a write is done)
    input  wire         mem_rvalid,
    input  wire  [31:0] mem_rdata,
    input  wire         irq_rcp,            // Cause.IP2 (the MI)
    input  wire         irq_mon,            // MON: the monitor level's interrupt
    // SNOOP: another master's write (sampled here: from another clock, each value lasts a cycle of this one at least),
    // and the hold on those writes while the lines to drop pile up
    input  wire         sn_t,
    input  wire  [20:0] sn_line,
    output logic        sn_hold,
    // retire trace (co-simulation)
    output logic        tr_retire,
    output logic [31:0] tr_pc,
    output logic [31:0] tr_insn,
    output logic        tr_wr,
    output logic [4:0]  tr_rd,
    output logic [63:0] tr_val,
    output logic        tr_exc,
    output logic        tr_exc_int,
    output logic [31:0] tr_exc_pc,
    output logic [4:0]  tr_exc_code,
    output logic        tr_fwe_even, tr_fwe_odd,    // the FPU registers it wrote (pair tr_fwidx)
    output logic [3:0]  tr_fwidx,
    output logic [31:0] tr_fweven, tr_fwodd,
    output logic [31:0] tr_fcr31,
    // debug (Mirlo-N64's on-screen display)
    output logic [31:0] dbg_pc, dbg_cause, dbg_status, dbg_epc,
    output wire         hwx_on              // MCTL bit 8: the game's access to the RCP registers the SoC has is the SoC's (no trap)
);

localparam bit XALU = W64 && W64_ALU;        // M's 64-bit unit
localparam bit XSH  = XALU && W64_SH;        // its shifter
localparam bit XMD  = XALU && W64_MD;        // dmult(u); HI and LO of 64 bits
localparam bit XDIV = XMD && W64_DIV;        // ddiv(u)
localparam bit PG   = FPG && TLB_RAM && FAST && FLAT && N64WIN;     // the fetch page registers

// =============================================================== decode
typedef enum logic [4:0] {
    A_NONE, A_ADD, A_SUB, A_AND, A_OR, A_XOR, A_NOR, A_SLT, A_SLTU, A_LUI, A_SLL, A_SRL, A_SRA, A_LINK,
    A_MFHI, A_MFLO
} alu_t;
typedef enum logic [3:0] {
    M_NONE, M_LB, M_LBU, M_LH, M_LHU, M_LW, M_LWL, M_LWR, M_LL, M_FL, M_LWU, M_LD
} load_t;
typedef enum logic [3:0] {
    S_NONE, S_SB, S_SH, S_SW, S_SWL, S_SWR, S_SC, S_FS, S_SD
} store_t;
typedef enum logic [3:0] {
    B_NONE, B_EQ, B_NE, B_LEZ, B_GTZ, B_LTZ, B_GEZ, B_C1F, B_C1T, B_J, B_JR
} br_t;
typedef enum logic [3:0] {
    MD_NONE, MD_MULT, MD_MULTU, MD_DIV, MD_DIVU, MD_MTHI, MD_MTLO, MD_DMULT, MD_DMULTU, MD_DDIV, MD_DDIVU
} md_t;

// the 64-bit arithmetic (at M)
typedef enum logic [3:0] {
    X_NONE, X_ADD, X_SUB, X_AND, X_OR, X_XOR, X_NOR, X_SLT, X_SLTU, X_SLL, X_SRL, X_SRA, X_MFHI, X_MFLO
} x_t;

typedef struct packed {
    logic        wr;
    logic [4:0]  rd;
    alu_t        alu;
    x_t          x64;
    logic        b_imm, imm_zero;
    logic        use_rs, use_rt;
    load_t       ld;
    store_t      st;
    br_t         br;
    logic        likely;
    md_t         md;
    logic [2:0]  trap;           // 1 ge 2 geu 3 lt 4 ltu 5 eq 6 ne
    logic        trap_imm;
    logic        sys, brk, ri;
    logic        mfc0, mtc0, eret, tlbr, tlbwi, tlbwr, tlbp, cache;
    logic        cop1, cop1_gpr, cop1_cond, cop1_64;   // (cop1_64: dmfc1, a 64-bit result)
    logic        late;           // its result comes out of M
} ctl_t;

// the COP1 operations fpu_n64 implements (moves, S/D arithmetic, conversions
// between S, D and W, compares)
function automatic logic fpu_implements(input logic [31:0] i);
    logic [4:0] rs; logic [5:0] fn;
    rs = i[25:21]; fn = i[5:0];
    case (rs)
    5'h00, 5'h01, 5'h02, 5'h04, 5'h05, 5'h06: return 1'b1;            // mfc1 dmfc1 cfc1 mtc1 dmtc1 ctc1
    5'h10, 5'h11: return fn <= 6'h0F || fn == 6'h24 || fn == 6'h25 || fn >= 6'h30 ||            // (.l: to a word, sign-extended)
                         (fn == 6'h20 && rs == 5'h11) || (fn == 6'h21 && rs == 5'h10);
    5'h14: return fn == 6'h20 || fn == 6'h21;                         // cvt.s.w cvt.d.w
    5'h15: return fn == 6'h20 || fn == 6'h21;                         // cvt.s.l cvt.d.l
    default: return 1'b0;
    endcase
endfunction

function automatic ctl_t decode(input logic [31:0] i);
    ctl_t c;
    logic [5:0] op, fn;
    logic [4:0] rs, rt, rd;
    op = i[31:26]; fn = i[5:0]; rs = i[25:21]; rt = i[20:16]; rd = i[15:11];
    c = '0;
    case (op)
    6'h00: begin
        c.use_rs = 1; c.use_rt = 1; c.rd = rd; c.wr = 1;
        case (fn)
        6'h00, 6'h04: c.alu = A_SLL;
        6'h02, 6'h06: c.alu = A_SRL;
        6'h03, 6'h07: c.alu = A_SRA;
        6'h08: begin c.wr = 0; c.br = B_JR; end
        6'h09: begin c.br = B_JR; c.alu = A_LINK; end
        6'h0C: begin c.wr = 0; c.sys = 1; end
        6'h0D: begin c.wr = 0; c.brk = 1; end
        6'h0F: c.wr = 0;
        6'h10: c.alu = A_MFHI;
        6'h11: begin c.wr = 0; c.md = MD_MTHI; end
        6'h12: c.alu = A_MFLO;
        6'h13: begin c.wr = 0; c.md = MD_MTLO; end
        6'h14, 6'h38, 6'h3C: begin c.x64 = X_SLL; c.late = 1; end        // dsllv, dsll, dsll32
        6'h16, 6'h3A, 6'h3E: begin c.x64 = X_SRL; c.late = 1; end
        6'h17, 6'h3B, 6'h3F: begin c.x64 = X_SRA; c.late = 1; end
        6'h2C, 6'h2D: begin c.x64 = X_ADD; c.alu = A_ADD; end            // dadd, daddu (the lower word is E's, the upper M's)
        6'h2E, 6'h2F: begin c.x64 = X_SUB; c.alu = A_SUB; end
        6'h18: begin c.wr = 0; c.md = MD_MULT; end
        6'h19: begin c.wr = 0; c.md = MD_MULTU; end
        6'h1A: begin c.wr = 0; c.md = MD_DIV; end
        6'h1B: begin c.wr = 0; c.md = MD_DIVU; end
        6'h1C: begin c.wr = 0; c.md = MD_DMULT; end
        6'h1D: begin c.wr = 0; c.md = MD_DMULTU; end
        6'h1E: begin c.wr = 0; c.md = MD_DDIV; end
        6'h1F: begin c.wr = 0; c.md = MD_DDIVU; end
        6'h20, 6'h21: c.alu = A_ADD;
        6'h22, 6'h23: c.alu = A_SUB;
        6'h24: begin c.alu = A_AND;  end
        6'h25: begin c.alu = A_OR;   end
        6'h26: begin c.alu = A_XOR;  end
        6'h27: begin c.alu = A_NOR;  end
        6'h2A: begin c.alu = A_SLT;  end
        6'h2B: begin c.alu = A_SLTU; end
        6'h30: begin c.wr = 0; c.trap = 3'd1; end
        6'h31: begin c.wr = 0; c.trap = 3'd2; end
        6'h32: begin c.wr = 0; c.trap = 3'd3; end
        6'h33: begin c.wr = 0; c.trap = 3'd4; end
        6'h34: begin c.wr = 0; c.trap = 3'd5; end
        6'h36: begin c.wr = 0; c.trap = 3'd6; end
        default: begin c.wr = 0; c.ri = 1; end
        endcase
        if (fn == 6'h00 || fn == 6'h02 || fn == 6'h03 || fn == 6'h38 || fn == 6'h3A || fn == 6'h3B ||
            fn == 6'h3C || fn == 6'h3E || fn == 6'h3F) c.use_rs = 0;
        if (fn == 6'h10 || fn == 6'h12) begin c.use_rs = 0; c.use_rt = 0; end
        if (fn == 6'h08 || fn == 6'h09 || fn == 6'h11 || fn == 6'h13) c.use_rt = 0;
        if (c.x64 != X_NONE && (!XALU || (!XSH && (c.x64 == X_SLL || c.x64 == X_SRL || c.x64 == X_SRA)))) begin c = '0; c.ri = 1; end
        if ((!XMD && (fn >= 6'h1C && fn <= 6'h1F)) || (!XDIV && fn == 6'h1F)) begin c = '0; c.ri = 1; end   // (ddiv without XDIV: d_cx)
    end
    6'h01: begin
        c.use_rs = 1;
        case (rt)
        5'h00, 5'h02, 5'h10, 5'h12: c.br = B_LTZ;
        5'h01, 5'h03, 5'h11, 5'h13: c.br = B_GEZ;
        5'h08: c.trap = 3'd1; 5'h09: c.trap = 3'd2; 5'h0A: c.trap = 3'd3; 5'h0B: c.trap = 3'd4;
        5'h0C: c.trap = 3'd5; 5'h0E: c.trap = 3'd6;
        default: c.ri = 1;
        endcase
        c.likely = rt[1] && !rt[3];
        c.trap_imm = rt[3]; c.b_imm = rt[3];
        if (rt[4]) begin c.wr = 1; c.rd = 5'd31; c.alu = A_LINK; end
    end
    6'h02: c.br = B_J;
    6'h03: begin c.br = B_J; c.wr = 1; c.rd = 5'd31; c.alu = A_LINK; end
    6'h04, 6'h14: begin c.br = B_EQ;  c.use_rs = 1; c.use_rt = 1; c.likely = op[4]; end
    6'h05, 6'h15: begin c.br = B_NE;  c.use_rs = 1; c.use_rt = 1; c.likely = op[4]; end
    6'h06, 6'h16: begin c.br = B_LEZ; c.use_rs = 1; c.likely = op[4]; end
    6'h07, 6'h17: begin c.br = B_GTZ; c.use_rs = 1; c.likely = op[4]; end
    6'h08, 6'h09: begin c.alu = A_ADD;  c.b_imm = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0A: begin c.alu = A_SLT;  c.b_imm = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0B: begin c.alu = A_SLTU; c.b_imm = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0C: begin c.alu = A_AND;  c.b_imm = 1; c.imm_zero = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0D: begin c.alu = A_OR;   c.b_imm = 1; c.imm_zero = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0E: begin c.alu = A_XOR;  c.b_imm = 1; c.imm_zero = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0F: begin c.alu = A_LUI;  c.b_imm = 1; c.wr = 1; c.rd = rt; end
    6'h18, 6'h19: begin                                                   // daddi, daddiu
        if (XALU) begin c.x64 = X_ADD; c.alu = A_ADD; c.b_imm = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
        else c.ri = 1;
    end
    6'h10: begin
        // (dmfc0, dmtc0: the registers are 32 bits)
        if (rs == 5'h00 || (W64 && rs == 5'h01)) begin c.mfc0 = 1; c.wr = 1; c.rd = rt; c.late = 1; end
        else if (rs == 5'h04 || (W64 && rs == 5'h05)) begin c.mtc0 = 1; c.use_rt = 1; end
        else if (rs == 5'h10) begin
            case (fn)
            6'h01: c.tlbr = 1;  6'h02: c.tlbwi = 1; 6'h06: c.tlbwr = 1; 6'h08: c.tlbp = 1;
            6'h18: c.eret = 1;
            default: c.ri = 1;
            endcase
        end else c.ri = 1;
    end
    6'h11: begin
        if (rs == 5'h08) begin c.br = rt[0] ? B_C1T : B_C1F; c.likely = rt[1]; end
        else if (!fpu_implements(i)) c.ri = 1;                            // dmfc1, dmtc1, .l, ...
        else begin
            c.cop1 = 1;
            if (rs == 5'h00 || rs == 5'h01 || rs == 5'h02) begin c.cop1_gpr = 1; c.wr = 1; c.rd = rt; c.late = 1; c.cop1_64 = rs == 5'h01; end
            if (rs == 5'h04 || rs == 5'h05 || rs == 5'h06) c.use_rt = 1;   // (dmtc1: its upper half too, m_st_hx)
            if (rs == 5'h06 || rs >= 5'h10) c.cop1_cond = 1;
        end
    end
    6'h20: begin c.ld = M_LB;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h21: begin c.ld = M_LH;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h22: begin c.ld = M_LWL; c.use_rs = 1; c.use_rt = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h23: begin c.ld = M_LW;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h24: begin c.ld = M_LBU; c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h25: begin c.ld = M_LHU; c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h26: begin c.ld = M_LWR; c.use_rs = 1; c.use_rt = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h27: if (W64) begin c.ld = M_LWU; c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end else c.ri = 1;
    6'h37: if (W64) begin c.ld = M_LD;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end else c.ri = 1;
    6'h3F: if (W64) begin c.st = S_SD;  c.use_rs = 1; c.use_rt = 1; end else c.ri = 1;
    6'h28: begin c.st = S_SB;  c.use_rs = 1; c.use_rt = 1; end
    6'h29: begin c.st = S_SH;  c.use_rs = 1; c.use_rt = 1; end
    6'h2A: begin c.st = S_SWL; c.use_rs = 1; c.use_rt = 1; end
    6'h2B: begin c.st = S_SW;  c.use_rs = 1; c.use_rt = 1; end
    6'h2E: begin c.st = S_SWR; c.use_rs = 1; c.use_rt = 1; end
    6'h2F: begin c.cache = 1; c.use_rs = 1; end
    6'h30: begin c.ld = M_LL;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h31, 6'h35: begin c.ld = M_FL; c.use_rs = 1; end
    6'h38: begin c.st = S_SC;  c.use_rs = 1; c.use_rt = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h39, 6'h3D: begin c.st = S_FS; c.use_rs = 1; end
    default: c.ri = 1;
    endcase
    if (c.rd == 5'd0) c.wr = 0;
    return c;
endfunction

// =============================================================== helpers
function automatic logic [31:0] sx8 (input logic [7:0]  v); return {{24{v[7]}},  v}; endfunction
function automatic logic [31:0] sx16(input logic [15:0] v); return {{16{v[15]}}, v}; endfunction
// (FLAT with N64WIN and the TLB: the N64 game's mapped addresses are those
// below 0x2000_0000, where Mirlo's own map has nothing -- SM64's 0x0400_0000 --
// and, with MAP_HI, 0x6000_0000 - 0x7FFF_FFFF, where it has nothing either once
// the monitor owns the RCP and there is no audio core: a game can map its code
// at 0x7000_0000 and run from there.)
// (and 0x5000_0000 - 0x5FFF_FFFF: nothing of this system is there, and a game can map its code
// there and jump to it. 0x2000_0000 - 0x4FFF_FFFF stay physical: the RSP, the SDRAM.)
// (and KSEG2, 0xC000_0000 - 0xDFFF_FFFF, and KSEG3 up to 0xEFFF_FFFF, which the VR4300 maps as it maps KUSEG: nothing of
// this system is there either, and a game can map its memory there -- the RDRAM through 0xE000_0000, its data written and
// read there byte by byte. 0xF000_0000 - 0xFFFF_FFFF stay physical: the registers, the monitor RAM.)
localparam bit MAP_HI = TLB_RAM && MON;
function automatic logic map_hi(input logic [31:0] va);
    return MAP_HI && (va[31:29] == 3'b011 || va[31:28] == 4'b0101 || va[31:29] == 3'b110 || va[31:28] == 4'b1110);
endfunction
function automatic logic kseg01(input logic [31:0] va);
    return FLAT ? !((TLB_EN || TLB_RAM) && N64WIN && (va[31:29] == 3'b000 || map_hi(va))) : va[31:30] == 2'b10;
endfunction
// N64WIN: the N64's RDRAM, through KSEG0 (cached) or KSEG1
function automatic logic n64win(input logic [31:0] va);
    return N64WIN && va[31:30] == 2'b10 && va[28:23] == 6'd0;
endfunction
function automatic logic [31:0] kphys(input logic [31:0] va);
    if (n64win(va)) return {N64_RDRAM[31:23], va[22:0]};
    return (FLAT && va[31:29] != 3'b101) ? va : {3'b000, va[28:0]};
endfunction
// a translation's physical address: the N64's, so with N64WIN in the RDRAM's place in SDRAM
function automatic logic [31:0] tphys(input logic [31:0] pa);
    return (FLAT && N64WIN) ? {N64_RDRAM[31:23], pa[22:0]} : pa;
endfunction
// an instruction of the N64 game's (fetched from KSEG0's window)
// (TLB_RAM: and from a page the game mapped)
function automatic logic n64code(input logic [31:0] pc);
    return N64WIN && (pc[31:23] == 9'h100 || (TLB_RAM && FLAT && (pc[31:29] == 3'b000 || map_hi(pc))));
endfunction
// a physical address the D-cache may hold (FLAT: pa == va there)
function automatic logic pa_cached(input logic [31:0] pa);
    return FLAT ? pa[31:26] == 6'b010000 : (pa >> CACHE_LOG2) == 32'd0;
endfunction
function automatic logic cacheable(input logic [31:0] va);
    return FLAT ? va[31:26] == 6'b010000 || (n64win(va) && !va[29])
                : va[31:29] == 3'b100 && (va[28:0] >> CACHE_LOG2) == 29'd0;
endfunction

// =============================================================== state
logic [31:0] rf_lo [32];
// no reset: MLABs (the hardware's registers are undefined at power-up; the
// co-simulation starts both machines from zero)
initial for (int i = 0; i < 32; i++) rf_lo[i] = 0;
// read synchronously (a Cyclone V MLAB registers its read address): the
// address is that of the instruction entering D, or D's own while it waits;
// the write landing on the same edge is bypassed from rf_x*
logic [31:0] rfq_a, rfq_b;
logic        rf_xv; logic [4:0] rf_xrd; logic [31:0] rf_xlo;
logic [31:0] hi_r, lo_r;
// W64: the registers' upper halves, as hx (see the top)
logic [31:0] rfq_ah, rfq_bh, hi_hx, lo_hx;
logic        rfq_aw, rfq_bw;                // (read with them) the register's hx is not zero
logic        rf_xw, hi_wide, lo_wide;       // the value written last cycle / HI / LO may have an hx
logic        hilo_soft;                     // COP0 7: HI and LO are the runtime's (mfhi, mflo trap)
logic        mon_game, mon_bd, mon_mi;              // MON: MCTL.GAME; the monitor's entry was in a delay slot
logic        mon_mem4;                              // MON: MCTL bit 10 -- the game's RDRAM is 4 MiB (no Expansion Pak)
logic [4:0]  mon_code;                      // MON: its cause
function automatic logic [31:0] hx32(input logic [63:0] v); return v[63:32] ^ {32{v[31]}}; endfunction
function automatic logic [63:0] v64(input logic [31:0] hx, input logic [31:0] lo); return {hx ^ {32{lo[31]}}, lo}; endfunction

logic [31:0] c0_index, c0_entrylo0, c0_entrylo1, c0_context, c0_pagemask, c0_wired, c0_badvaddr,
             c0_count, c0_entryhi, c0_compare, c0_status, c0_cause, c0_epc, c0_config, c0_lladdr,
             c0_watchlo, c0_watchhi, c0_taglo, c0_errorepc;
logic        count_frac;
logic [4:0]  c0_random;                     // decrements every step, from 31 down to Wired
wire  [4:0]  rnd_dec = (c0_random <= c0_wired[4:0]) ? 5'd31 : c0_random - 5'd1;
wire  [4:0]  rnd_now = SIM_COUNT_RETIRE ? rnd_dec : c0_random;   // (the model steps before executing)
logic        ll_bit;
logic        fcc;

// =============================================================== TLB
// The entries live in a RAM and are searched one per cycle (the scanner,
// shared by F and M; tlbp and tlbr use it too). Mapped accesses are rare
// (SM64: 200 k in 6,000 M cycles), and each of F and M keeps the last page it
// translated (a micro-TLB), so a search only follows a change of page.
typedef struct packed {
    logic [18:0] vpn;                        // EntryHi[31:13], the page-mask bits clear
    logic [7:0]  asid;
    logic [11:0] mask;                       // PageMask[24:13]
    logic        g;
    logic [30:0] lo0, lo1;                   // EntryLo[31:1]
} tlbe_t;
tlbe_t       tlb_ram [TLB_N];
logic        tlb_we; logic [4:0] tlb_waddr; tlbe_t tlb_wdata;
logic [4:0]  tlb_raddr; tlbe_t tlb_q;
localparam tlbe_t TLBE_RESET = '{vpn: 19'h40000, default: '0};                      // a kseg0 VPN: never matches
initial for (int i = 0; i < TLB_N; i++) tlb_ram[i] = TLBE_RESET;
always_ff @(posedge clk) begin
    if (tlb_we) tlb_ram[tlb_waddr] <= tlb_wdata;
    tlb_q <= tlb_ram[tlb_raddr];
end

// an entry's translation of va: {valid, dirty, pa}, and the page (for a micro-TLB)
typedef struct packed {
    logic        v, d, c;                    // (c: the table's C bit; TLB_EN's entries: 0)
    logic [19:0] vpage;                      // va[31:12] with the offset bits clear
    logic [12:0] om;                         // the page offset's bits above 11 (va[24:12])
    logic [19:0] pfn;                        // pa[31:12], the offset bits clear
    logic        g;
    logic [7:0]  asid;
} tlbx_t;
function automatic tlbx_t tlb_xlat(input tlbe_t e, input logic [31:0] va);
    tlbx_t x;
    logic [12:0] om, pb;
    logic [30:0] lo;
    om = {1'b0, e.mask};                                     // (page - 1) >> 12
    pb = {1'b0, e.mask} + 13'd1;                             // the bit choosing the odd page
    lo = (va[24:12] & pb) != 0 ? e.lo1 : e.lo0;
    x.v = lo[0]; x.d = lo[1]; x.c = 1'b0;                    // EntryLo bits 1, 2
    x.om = om;
    x.vpage = va[31:12] & ~{7'd0, om};
    x.pfn = lo[24:5] & ~{7'd0, om};                          // EntryLo[25:6]
    x.g = e.g; x.asid = e.asid;
    return x;
endfunction
function automatic logic tlb_match(input tlbe_t e, input logic [31:0] va, input logic [7:0] asid);
    return ((va[31:13] ^ e.vpn) & {7'h7F, ~e.mask}) == 0 && (e.g || e.asid == asid);
endfunction
// a micro-TLB: the last page translated
typedef struct packed { logic valid; tlbx_t x; } utlb_t;
function automatic logic utlb_hit(input utlb_t u, input logic [31:0] va, input logic [7:0] asid);
    return u.valid && ((va[31:12] ^ u.x.vpage) & ~{7'd0, u.x.om}) == 0 && (u.x.g || u.x.asid == asid);
endfunction
// {hit, valid, dirty, pa}
function automatic logic [34:0] utlb_res(input tlbx_t x, input logic [31:0] va);
    return {1'b1, x.v, x.d, {x.pfn, 12'd0} | {7'd0, va[24:0] & {x.om, 12'hFFF}}};
endfunction

typedef enum logic [1:0] { SC_IDLE, SC_RUN, SC_DONE } sc_t;
typedef enum logic [1:0] { SCM_LOOKUP, SCM_PROBE, SCM_READ } scm_t;
sc_t         sc;
scm_t        sc_mode;
logic        sc_for_m;                       // M's request (else F's)
logic [4:0]  sc_k;                           // the entry in tlb_q
logic [31:0] sc_va;
logic [7:0]  sc_asid;
logic        sc_hit;
logic [4:0]  sc_idx;
tlbx_t       sc_x;
tlbe_t       sc_e;                           // tlbr's entry
utlb_t       utlb_f, utlb_m;

// TLB_RAM: the table. Its lookups go through the scanner's request and
// answer (one entry to read instead of 32 to search).
typedef struct packed {
    logic        v, d, g, c;                 // c: cacheable (a fetch may run through the I-cache)
    logic [7:0]  asid;
    logic [10:0] tag;                        // va[31:21]
    logic [16:0] pfn;                        // pa[28:12]
} tre_t;
logic        tr_lastf;                       // the last TLB exception was a fetch's
logic [31:0] mon_badv;                       // (MON) the address of the last translation fault taken by the monitor level: COP0 22
logic [8:0]  tr_raddr, tr_waddr;
logic        tr_we_d, tr_we_i;
tre_t        tr_wdata, tr_qd, tr_qi;
generate if (TLB_RAM) begin : g_tr
    logic [$bits(tre_t)-1:0] tr_d [512], tr_i [512];
    initial for (int i = 0; i < 512; i++) begin tr_d[i] = '0; tr_i[i] = '0; end
    always_ff @(posedge clk) begin
        if (tr_we_d) tr_d[tr_waddr] <= tr_wdata;
        tr_qd <= tr_d[tr_raddr];
    end
    always_ff @(posedge clk) begin
        if (tr_we_i) tr_i[tr_waddr] <= tr_wdata;
        tr_qi <= tr_i[tr_raddr];
    end
end else begin : g_notr
    always_comb begin tr_qd = '0; tr_qi = '0; end
end endgenerate

// =============================================================== caches
localparam int IC_LINES = 1 << IC_LOG2, DC_LINES = 1 << DC_LOG2;
localparam int IW = IC_LOG2 + 3, DW = DC_LOG2 + 3;      // a word's address in them: pa[IW+1:2], pa[DW+1:2]
logic [31:0] ic_data [IC_LINES * 8];
// The tag: {valid, pa[28:14]}. With the page registers (PG) a line's index comes from the virtual address, so the physical bits
// between the page offset and the tag's first -- pa[13:12], which the index no longer says -- are in it too: {valid, pa[28:12]}.
localparam int TLO = PG ? 12 : 14;           // the tag's lowest physical address bit
localparam int ITW = 28 - TLO + 1;           // its width without the valid bit
logic [ITW:0] ic_tag  [IC_LINES];
logic [7:0]  dc_b0 [DC_LINES * 8], dc_b1 [DC_LINES * 8], dc_b2 [DC_LINES * 8], dc_b3 [DC_LINES * 8];   // byte lanes
logic [16:0] dc_tag  [DC_LINES];            // {valid, pa[28:13]}
logic [IW-1:0] ic_raddr; logic [31:0] ic_q; logic [ITW:0] ic_tq;
logic [DW-1:0] dc_raddr; logic [31:0] dc_q; logic [16:0] dc_tq;
// writes: driven combinationally below (the write happens at the edge that ends
// the cycle, so a read issued in the next cycle sees it)
logic        ic_we;  logic [IW-1:0] ic_waddr;  logic [31:0] ic_wdata;
logic        ict_we; logic [IW-4:0]  ict_waddr; logic [ITW:0] ict_wdata;
logic        dc_we;  logic [DW-1:0] dc_waddr;  logic [31:0] dc_wdata; logic [3:0] dc_wbe;
logic        dct_we; logic [DW-4:0] dct_waddr; logic [16:0] dct_wdata;
logic        sn_go; logic [DW-4:0] sn_idx;  // (SNOOP) a line to drop, by the tags' write port when the core leaves it
logic        sn_kill;                        // (SNOOP) the line being read in was written meanwhile: it comes in invalid
logic        slow_is_d;
logic        slow_d_own;                     // P_SLOW was granted to M's slow path (else to F's)

logic        ic_ren;                         // (FAST: the read is kept while F's instruction waits for D)
always_ff @(posedge clk) begin
    if (ic_ren) begin
        ic_q  <= ic_data[ic_raddr];
        ic_tq <= ic_tag[ic_raddr[IW-1:3]];
    end
    if (ic_we)  ic_data[ic_waddr] <= ic_wdata;
    if (ict_we) ic_tag[ict_waddr] <= ict_wdata;
end
always_ff @(posedge clk) begin
    dc_q  <= {dc_b3[dc_raddr], dc_b2[dc_raddr], dc_b1[dc_raddr], dc_b0[dc_raddr]};
    dc_tq <= dc_tag[dc_raddr[DW-1:3]];
    if (dc_we) begin
        if (dc_wbe[0]) dc_b0[dc_waddr] <= dc_wdata[7:0];
        if (dc_wbe[1]) dc_b1[dc_waddr] <= dc_wdata[15:8];
        if (dc_wbe[2]) dc_b2[dc_waddr] <= dc_wdata[23:16];
        if (dc_wbe[3]) dc_b3[dc_waddr] <= dc_wdata[31:24];
    end
    if (dct_we) dc_tag[dct_waddr] <= dct_wdata;
end

// =============================================================== write buffer
localparam int WB_N = 8;
logic [31:0] wb_addr [WB_N];
logic [31:0] wb_data [WB_N];
logic [3:0]  wb_strb [WB_N];
logic [3:0]  wb_rp, wb_wp;
logic [3:0]  wb_cnt;                         // (FAST: wb_wp - wb_rp, kept)
wire  [3:0]  wb_n = FAST ? wb_cnt : wb_wp - wb_rp;
wire         wb_empty = XWB ? 1'b1 : FAST ? wb_cnt == 4'd0 : wb_rp == wb_wp;
wire         wb_full  = XWB ? st_full : wb_n == 4'(WB_N);
// It is emptied in runs, not a store at a time: on the one SDRAM a write
// among another port's reads costs the bus its turn both ways (and often a
// row), whatever follows it. A run starts with five stores waiting, when the
// oldest has waited 32 cycles, or when the port is wanted for anything else
// (a refill, a device access: those still come after every store before them).
logic [5:0]  wb_age;
wire         wb_go = wb_n >= 4'd5 || wb_age[5];

// =============================================================== pipeline registers
logic [31:0] f_pc;
logic        f_ok;                           // the RAMs hold f_pc's line this cycle
logic        ds_next;                        // the next instruction into D is a delay slot
logic        rd_pend, rd_taken, rd_annul;    // its branch's decision, when it left D before it
logic [31:0] rd_target;

logic        d_valid, d_dly, d_fexc;
logic        d_fmiss;                        // FAST: D's word is of another line than its pc's
// FAST: the branch that left E last cycle (rd_res) -- rd_target is where the
// fetch goes after its delay slot, rd_annul says the slot is dropped, rd_fix
// that the fetch went elsewhere. rd_pend then keeps the two for a delay slot
// that is still to come. e_bp: the fetch followed E's branch as taken
logic        rd_res, rd_fix, e_bp;
logic        d_k_cti, d_k_pred;              // FAST: D's instruction is a branch; guessed taken
logic        rp_pend, rp_m;                  // FAST: a load took another line's word: again from its pc -- W's by now, or (rp_m) M's still
logic [31:0] d_fill_pa;
logic [31:0] d_pc, d_insn;
logic [4:0]  d_fexc_code;

logic        e_valid, e_dly, e_fexc, e_serial;
ctl_t        e_c;
logic [31:0] e_pc, e_insn, e_ra, e_rb;     // operands as captured in D
logic [31:0] e_ra_hx, e_rb_hx;
logic        e_aw, e_hxeq;                  // (FAST: a branch's) rs's hx is not zero; rs's and rt's are equal
logic [4:0]  e_rs, e_rt, e_fexc_code;

logic        m_valid, m_dly, m_exc, m_serial, m_dev, m_hwx;
logic [31:0] m_a_lo;                         // rs (mthi/mtlo, mult/div)
ctl_t        m_c;
logic [31:0] m_pc, m_insn, m_res, m_addr, m_st_lo, m_badv;
logic [31:0] m_a_hx, m_st_hx;
logic        m_ph;                           // a cached 64-bit access: its second word
logic [4:0]  m_exc_code;

logic        w_valid, w_serial;
logic        w_lm;                           // FAST: W's is the load that took another line's word: not written
ctl_t        w_c;
logic [31:0] w_pc, w_insn, w_res, w_hx;

// =============================================================== D
ctl_t d_c, d_c_r;                            // (FAST: decoded as it leaves F, d_c_r)
always_comb d_c = FAST ? d_c_r : decode(d_insn);
wire [4:0] d_rs = d_insn[25:21], d_rt = d_insn[20:16];
wire w_ok = w_valid && !(FAST && w_lm);
wire w_wr = w_ok && w_c.wr;
wire m_wr = m_valid && m_c.wr;
// (FAST: what E holds may be a delay slot its branch has just annulled -- e_kill, below)
wire e_kill;
wire e_live = e_valid && !e_kill;
wire e_wr = e_live && e_c.wr;

// the regfile, with W's write bypassed
wire w_rs = w_wr && w_c.rd == d_rs, w_rt = w_wr && w_c.rd == d_rt;
wire x_rs = rf_xv && rf_xrd == d_rs, x_rt = rf_xv && rf_xrd == d_rt;
wire [31:0] rf_a  = d_rs == 0 ? 32'd0 : w_rs ? w_res  : x_rs ? rf_xlo : rfq_a;
wire [31:0] rf_b  = d_rt == 0 ? 32'd0 : w_rt ? w_res  : x_rt ? rf_xlo : rfq_b;
// a branch reads now: M's (not late) result forwarded
wire m_rs = m_wr && !m_c.late && m_c.rd == d_rs && d_rs != 0;
wire m_rt = m_wr && !m_c.late && m_c.rd == d_rt && d_rt != 0;
wire [31:0] br_a = m_rs ? m_res : rf_a, br_b = m_rt ? m_res : rf_b;
// hx: the register file's, or zero for a value still in the pipeline (what
// can have one makes its readers wait: haz_wide)
function automatic logic wideprod(input ctl_t c);
    return (XALU && c.x64 != X_NONE && c.x64 != X_SLT && c.x64 != X_SLTU) || c.ld == M_LD || c.ld == M_LWU || c.cop1_64;   // (a compare's result is 0 or 1)
endfunction
wire e_wp = e_live && e_c.wr && wideprod(e_c), m_wp = m_valid && m_c.wr && wideprod(m_c);
wire w_wp = w_valid && w_c.wr && wideprod(w_c), x_wp = rf_xv && rf_xw;
wire inflight_rs = (e_live && e_c.wr && e_c.rd == d_rs) || (m_valid && m_c.wr && m_c.rd == d_rs) || w_rs || x_rs;
wire inflight_rt = (e_live && e_c.wr && e_c.rd == d_rt) || (m_valid && m_c.wr && m_c.rd == d_rt) || w_rt || x_rt;
wire [31:0] rf_ah = (!W64 || d_rs == 0 || inflight_rs) ? 32'd0 : rfq_ah;
wire [31:0] rf_bh = (!W64 || d_rt == 0 || inflight_rt) ? 32'd0 : rfq_bh;
// (whether they are not zero: one bit of the register file's, not a 32-bit compare behind the masks)
wire        rf_aw = W64 && d_rs != 0 && !inflight_rs && rfq_aw;
wire        rf_bw = W64 && d_rt != 0 && !inflight_rt && rfq_bw;
// (64 bits: equal halves; the sign is the upper half's)
wire br_eq = br_a == br_b && rf_ah == rf_bh;
wire br_az = br_a == 32'd0 && rf_ah == 32'd0;
wire br_an = br_a[31] ^ rf_ah[31];

wire d_needs_rs = d_c.use_rs && d_rs != 0, d_needs_rt = d_c.use_rt && d_rt != 0;
wire e_rs_hit = e_wr && e_c.rd == d_rs, e_rt_hit = e_wr && e_c.rd == d_rt;
wire m_rs_late = m_wr && m_c.late && m_c.rd == d_rs, m_rt_late = m_wr && m_c.late && m_c.rd == d_rt;
wire d_reads_now = (d_c.br != B_NONE && d_c.br != B_J);
wire haz_br = d_reads_now && ((d_needs_rs && (e_rs_hit || m_rs_late)) || (d_needs_rt && (e_rt_hit || m_rt_late)));
wire haz_late = e_live && e_c.late && ((d_needs_rs && e_rs_hit) || (d_needs_rt && e_rt_hit));
wire haz_fcc = (d_c.br == B_C1F || d_c.br == B_C1T) && ((e_live && e_c.cop1_cond) || (m_valid && m_c.cop1_cond));
wire d_serial = d_c.mtc0 || d_c.eret || d_c.tlbwi || d_c.tlbwr || d_c.tlbr || d_c.tlbp || d_c.cache;
wire haz_serial = (d_serial && (e_live || m_valid || w_valid)) || (e_live && e_serial) || (m_valid && m_serial) || (w_valid && w_serial);
logic md_busy;
wire haz_md = (d_c.alu == A_MFHI || d_c.alu == A_MFLO || d_c.md != MD_NONE) &&
              (md_busy || (e_live && e_c.md != MD_NONE) || (m_valid && m_c.md != MD_NONE));
// (W64) a register on its way from M's unit or a 64-bit load: its hx is read from the register file
wire haz_wide = W64 && ((d_needs_rs && ((e_wp && e_c.rd == d_rs) || (m_wp && m_c.rd == d_rs) || (w_wp && w_c.rd == d_rs) || (x_wp && rf_xrd == d_rs))) ||
                        (d_needs_rt && ((e_wp && e_c.rd == d_rt) || (m_wp && m_c.rd == d_rt) || (w_wp && w_c.rd == d_rt) || (x_wp && rf_xrd == d_rt))));
// (FAST) what D holds is dropped by the branch decided last cycle: fetched
// behind its delay slot down the wrong way, or the delay slot it annuls
wire d_kill = FAST && rd_res && d_valid && (d_dly ? rd_annul : rd_fix);
wire d_imiss = FAST && d_valid && d_fmiss && !d_kill;       // (what D holds is not its instruction)
wire d_ok = d_valid && !(FAST && d_fmiss) && !d_kill;
// (FAST: a branch reads at E)
wire d_stall = d_ok && ((!FAST && (haz_br || haz_fcc)) || haz_late || haz_serial || haz_md || haz_wide);

// what E gets: with a 64-bit operand, the instruction as M's unit does it
ctl_t d_cx;
always_comb begin
    logic aw, bw;
    x_t x;
    aw = d_c.use_rs && rf_aw;
    bw = d_c.use_rt && !d_c.b_imm && rf_bw;
    d_cx = d_c;
    x = X_NONE;
    if (W64) begin
        case (d_c.alu)
        A_AND:  if ((aw || bw) && !d_c.b_imm) x = X_AND;    // (andi's result is 16 bits)
        A_OR:   if (aw || bw) x = X_OR;
        A_XOR:  if (aw || bw) x = X_XOR;
        A_NOR:  if (aw || bw) x = X_NOR;
        A_SLT:  if (aw || bw) x = X_SLT;
        A_SLTU: if (aw || bw) x = X_SLTU;
        A_MFHI: if (XMD && hi_wide) x = X_MFHI;
        A_MFLO: if (XMD && lo_wide) x = X_MFLO;
        default: ;
        endcase
        if (x != X_NONE) begin
            if (XALU) begin
                d_cx.x64 = x;
                if (x == X_SLT || x == X_SLTU) begin d_cx.alu = A_NONE; d_cx.late = 1'b1; end    // (the others: the lower word is E's, as ever)
            end
            else d_cx.ri = 1'b1;                            // (the runtime's)
        end
        if ((d_c.alu == A_MFHI || d_c.alu == A_MFLO) && !XMD && hilo_soft) d_cx.ri = 1'b1;
        // (HI and LO are 32 bits there: a 64-bit value into them is the runtime's to keep)
        // (... and while the runtime keeps them, either of the two: the other stays its)
        if ((d_c.md == MD_MTHI || d_c.md == MD_MTLO) && !XMD && (aw || hilo_soft)) d_cx.ri = 1'b1;
        if (d_c.trap != 3'd0 && (aw || bw)) begin d_cx.trap = 3'd0; d_cx.ri = 1'b1; end
        // (ddiv without XDIV: two words' (32-bit operands) on the 32-bit divider, the quotient and the remainder as they
        // are; a 64-bit operand is the runtime's)
        if (!XDIV && d_c.md == MD_DDIV && (aw || bw)) d_cx.ri = 1'b1;
    end
    // TLB_RAM with the monitor: the game's own TLB instructions (tlbwi, tlbwr, tlbr, tlbp) are a Reserved Instruction for the
    // monitor level, which does them on the entries it keeps (the translation tables hold no VR4300 entries to write, read or
    // search). EPC is the instruction (ErrorEPC, MCTL's cause 10); the monitor resumes past it.
    if (TLB_RAM && MON && mon_game && (d_c.tlbwi || d_c.tlbwr || d_c.tlbr || d_c.tlbp)) d_cx.ri = 1'b1;
end

logic d_taken;
always_comb begin
    case (d_c.br)
    B_EQ:  d_taken = br_eq;
    B_NE:  d_taken = !br_eq;
    B_LEZ: d_taken = br_an || br_az;
    B_GTZ: d_taken = !br_an && !br_az;
    B_LTZ: d_taken = br_an;
    B_GEZ: d_taken = !br_an;
    B_C1F: d_taken = !fcc;
    B_C1T: d_taken = fcc;
    B_J, B_JR: d_taken = 1'b1;
    default: d_taken = 1'b0;
    endcase
end
wire [31:0] d_target = d_c.br == B_J  ? {d_pc[31:28], d_insn[25:0], 2'b00}
                     : d_c.br == B_JR ? br_a
                     : d_pc + 32'd4 + {{14{d_insn[15]}}, d_insn[15:0], 2'b00};
wire d_cti = d_c.br != B_NONE && !d_fexc;
// FAST: where the fetch goes after the delay slot, before the branch is decided
function automatic logic guess(input ctl_t c, input logic [31:0] i);
    return c.br == B_J || (c.br != B_NONE && c.br != B_JR && i[15]);
endfunction
wire [31:0] d_ptarget = d_c.br == B_J ? {d_pc[31:28], d_insn[25:0], 2'b00}
                      : d_pc + 32'd4 + {{14{d_insn[15]}}, d_insn[15:0], 2'b00};

// =============================================================== E
// operands: E-stage forwarding from M (not late) and W
// (FAST: which of them, known as the instruction leaves D -- M and W then
// hold what E and M hold now -- and an immediate in e_rb, as an operand)
logic e_fa_m, e_fa_w, e_fb_m, e_fb_w;
wire e_m_a = FAST ? e_fa_m : m_wr && !m_c.late && m_c.rd == e_rs && e_rs != 0;
wire e_m_b = FAST ? e_fb_m : m_wr && !m_c.late && m_c.rd == e_rt && e_rt != 0;
wire e_w_a = FAST ? e_fa_w : w_wr && w_c.rd == e_rs && e_rs != 0;
wire e_w_b = FAST ? e_fb_w : w_wr && w_c.rd == e_rt && e_rt != 0;
wire [31:0] e_a  = e_m_a ? m_res : e_w_a ? w_res : e_ra;
wire [31:0] e_b  = e_m_b ? m_res : e_w_b ? w_res : e_rb;

wire [31:0] e_bv  = (!FAST && e_c.b_imm) ? (e_c.imm_zero ? {16'd0, e_insn[15:0]} : sx16(e_insn[15:0])) : e_b;
// the compares, for slt and the traps (32 bits: with a 64-bit operand slt
// runs at M and a trap is a Reserved Instruction -- d_cx)
// (FAST: from the adder, which subtracts for all but an add -- e_k_sub: no borrow is "not below", and the difference's sign
// decides a signed compare of operands of one sign)
logic       e_k_sub;
wire [32:0] e_cs;
wire        e_lt   = FAST ? ((e_a[31] != e_bv[31]) ? e_a[31] : e_cs[31]) : $signed(e_a) < $signed(e_bv);
wire        e_ltu  = FAST ? !e_cs[32] : e_a < e_bv;
wire        e_eq   = e_a == e_bv;
wire [5:0]  e_fn  = e_insn[5:0];
wire [4:0]  e_sh  = (e_fn == 6'h04 || e_fn == 6'h06 || e_fn == 6'h07) ? e_a[4:0] : e_insn[10:6];

// one shifter: SRL/SRA directly, SLL as a right shift of the bit-reversed value
function automatic logic [31:0] rev32(input logic [31:0] v);
    for (int i = 0; i < 32; i++) rev32[i] = v[31 - i];
endfunction
wire        e_sl = e_c.alu == A_SLL;
wire [32:0] e_shx = {e_c.alu == A_SRA && e_b[31], e_sl ? rev32(e_b) : e_b};
wire [32:0] e_shr = $signed(e_shx) >>> e_sh;
wire [31:0] e_shv = e_sl ? rev32(e_shr[31:0]) : e_shr[31:0];

// (FAST) the result by its kind, registered with the instruction: e_k_grp 0
// add or subtract (e_k_sub), 1 a shift, 2 one of e_k_op's, 3 a compare (e_k_f:
// unsigned) -- or nothing (e_k_op[0])
logic [1:0] e_k_grp; logic [2:0] e_k_op; logic e_k_f;
// (the carry out, for the upper word of a 64-bit add or subtract, which M does)
wire        e_fsub = FAST ? e_k_sub : e_c.alu == A_SUB;
assign      e_cs = {1'b0, e_a} + {1'b0, e_bv ^ {32{e_fsub}}} + 33'(e_fsub);
wire [31:0] e_addsub = e_cs[31:0];
logic [31:0] e_misc;
always_comb begin
    case (e_k_op)
    3'd0: e_misc = e_a & e_bv;
    3'd1: e_misc = e_a | e_bv;
    3'd2: e_misc = e_a ^ e_bv;
    3'd3: e_misc = ~(e_a | e_bv);
    3'd4: e_misc = {e_insn[15:0], 16'd0};
    3'd5: e_misc = e_pc + 32'd8;
    3'd6: e_misc = hi_r;
    default: e_misc = lo_r;
    endcase
end
logic [31:0] e_res;
always_comb begin
    e_res = 32'd0;
    if (FAST) begin
        case (e_k_grp)
        2'd0: e_res = e_addsub;
        2'd1: e_res = e_shv;
        2'd2: e_res = e_misc;
        default: e_res = {31'd0, !e_k_op[0] && (e_k_f ? e_ltu : e_lt)};
        endcase
    end
    else case (e_c.alu)
    A_ADD, A_SUB: e_res = e_cs[31:0];
    A_LUI:   e_res = {e_insn[15:0], 16'd0};
    A_SLL, A_SRL, A_SRA: e_res = e_shv;
    A_LINK:  e_res = e_pc + 32'd8;
    A_SLT:   e_res = {31'd0, e_lt};
    A_SLTU:  e_res = {31'd0, e_ltu};
    A_AND:   e_res = e_a & e_bv;
    A_OR:    e_res = e_a | e_bv;
    A_XOR:   e_res = e_a ^ e_bv;
    A_NOR:   e_res = ~(e_a | e_bv);
    A_MFHI:  e_res = hi_r;
    A_MFLO:  e_res = lo_r;
    default: ;
    endcase
end

// FAST: the branch, decided here (64 bits: the halves equal; the sign is the
// upper half's). The condition bit of a compare still at M is the FPU's
wire e_cti = e_c.br != B_NONE;
wire e_beq = e_eq && e_hxeq;
wire e_baz = e_a == 32'd0 && !e_aw;
wire e_ban = e_a[31] ^ e_ra_hx[31];
logic fpu_cond;
wire e_fcc = (m_valid && m_c.cop1 && m_c.cop1_cond) ? fpu_cond : fcc;
logic e_taken;
always_comb begin
    case (e_c.br)
    B_EQ:  e_taken = e_beq;
    B_NE:  e_taken = !e_beq;
    B_LEZ: e_taken = e_ban || e_baz;
    B_GTZ: e_taken = !e_ban && !e_baz;
    B_LTZ: e_taken = e_ban;
    B_GEZ: e_taken = !e_ban;
    B_C1F: e_taken = !e_fcc;
    B_C1T: e_taken = e_fcc;
    B_J, B_JR: e_taken = 1'b1;
    default: e_taken = 1'b0;
    endcase
end
// (a jump's or a branch's own target is what D made of it for the guess, d_ptarget, kept: no adder here)
logic [31:0] e_ptarget;
wire [31:0] e_target = e_c.br == B_JR ? e_a : e_ptarget;
// the delay slot it annuls, when that is at E by now
assign e_kill = FAST && rd_res && rd_annul && e_valid && e_dly;

// (a trap's second operand is e_bv: the register, or its immediate)
logic e_trap;
always_comb begin
    case (e_c.trap)
    3'd1: e_trap = !e_lt;
    3'd2: e_trap = !e_ltu;
    3'd3: e_trap = e_lt;
    3'd4: e_trap = e_ltu;
    3'd5: e_trap = e_eq;
    3'd6: e_trap = !e_eq;
    default: e_trap = 1'b0;
    endcase
end

wire [31:0] e_ea = e_a + sx16(e_insn[15:0]);
// What the instruction's kind says about its memory access is known as it leaves D, and registered with it (e_c's):
// the address's alignment requirement (halfword, word, doubleword), whether it is a memory access, a 64-bit FPU access,
// and whether the instruction is an FPU one (Coprocessor Unusable) -- so E's exception cone starts from flops, not from a decode of e_insn.
logic e_al2, e_al4, e_al8, e_mem, e_fwide, e_isfp, e_n64c;
logic d_al2, d_al4, d_al8, d_mem, d_fwide, d_isfp;
always_comb begin
    d_al2 = 1'b0; d_al4 = 1'b0; d_al8 = 1'b0;
    case (d_c.ld)
    M_LH, M_LHU: d_al2 = 1'b1;
    M_LW, M_LL, M_LWU: d_al4 = 1'b1;
    M_LD: d_al8 = 1'b1;
    M_FL: begin d_al8 = d_insn[31:26] == 6'h35; d_al4 = !d_al8; end
    default: ;
    endcase
    case (d_c.st)
    S_SH: begin d_al2 = 1'b1; d_al4 = 1'b0; d_al8 = 1'b0; end
    S_SW, S_SC: begin d_al2 = 1'b0; d_al4 = 1'b1; d_al8 = 1'b0; end
    S_SD: begin d_al2 = 1'b0; d_al4 = 1'b0; d_al8 = 1'b1; end
    S_FS: begin d_al8 = d_insn[31:26] == 6'h3D; d_al4 = !d_al8; d_al2 = 1'b0; end
    default: ;
    endcase
    d_mem   = d_c.ld != M_NONE || d_c.st != S_NONE;
    d_fwide = (d_c.ld == M_FL && d_insn[31:26] == 6'h35) || (d_c.st == S_FS && d_insn[31:26] == 6'h3D);
    d_isfp  = d_insn[31:26] == 6'h11 || d_c.ld == M_FL || d_c.st == S_FS;
end
// (FAST) a store's word and strobes, put in their lanes here (but sd's and the FPU's)
logic [31:0] e_sdata; logic [3:0] e_sstrb;
always_comb begin
    logic [1:0] k;
    k = BIG ? e_ea[1:0] : ~e_ea[1:0]; e_sdata = e_b; e_sstrb = 4'b1111;
    case (e_c.st)
    S_SB:  begin e_sdata = {4{e_b[7:0]}};  e_sstrb = 4'b1000 >> k; end
    S_SH:  begin e_sdata = {2{e_b[15:0]}}; e_sstrb = k[1] ? 4'b0011 : 4'b1100; end
    S_SWL: begin e_sdata = e_b >> (8 * k); e_sstrb = 4'b1111 >> k; end
    S_SWR: begin e_sdata = e_b << (8 * (3 - k)); e_sstrb = 4'b1111 << (3 - k); end
    default: ;
    endcase
end
wire e_misalign = (e_al2 && e_ea[0]) || (e_al4 && e_ea[1:0] != 0) || (e_al8 && e_ea[2:0] != 0);
// N64WIN: the game touching what is not RDRAM (Mirlo's runtime emulates it)
// (MI: the MI's and PI_STATUS' word accesses, in KSEG0 or KSEG1, are the device port's. The address error they would
// be is taken back at M (m_exc_v): this compare feeds one flop, m_hwx, and no other logic of E)
wire e_hwx = MI && mon_mi && !e_fexc && !e_misalign && e_ea[31] && !e_ea[30] && (e_c.ld == M_LW || e_c.st == S_SW) &&
             (e_ea[28:20] == 9'h043 || (VIW && e_ea[28:20] == 9'h044) || (AIW && e_ea[28:20] == 9'h045) || (PIW && e_ea[28:20] == 9'h046 && e_ea[19:2] == 18'd4) ||
              (SIW && (e_ea[28:20] == 9'h048 || (e_ea[28:20] == 9'h1FC && e_ea[10:6] == 5'h1F))) ||
              (PI2W && ((e_ea[28:27] == 2'b01 && !e_ea[16]) || (e_ea[28:26] == 3'b100 && (ROM64W || !(e_ea[25] && e_ea[24]))))));
// (MCTL bit 10, mon_mem4: a console without the Expansion Pak -- the upper 4 MiB of the window are nothing of the game's either,
// and the monitor answers as the console does: a game that sizes the memory itself finds 4 MiB)
wire e_n64dev = e_mem && e_n64c && (!n64win(e_ea) || (MON && mon_mem4 && e_ea[22])) && kseg01(e_ea);   // (a mapped address is the TLB's)

// E's synchronous exceptions (in order: fetch fault, coprocessor unusable,
// RI/sys/break, trap, address). CpU: an FPU instruction (bc1 included) or an
// FPU load/store while Status.CU1 is clear -- libultra starts every thread
// without it and, on this trap, gives the thread CU1 and marks it as using
// the FPU; only such threads get their FPU registers saved on a switch.
// Without it no thread ever was, and the sound thread's floats overwrote the
// game's mid-calculation (2026-09-29; sim/n64/cpu.c the same).
wire e_cpu1 = !c0_status[29] && e_isfp;
logic [4:0] e_exc_code; logic [31:0] e_badv;
// (e_exc is one OR of the causes, not the end of the priority chain that picks the code)
wire e_exc = e_fexc || e_cpu1 || e_c.ri || e_c.sys || e_c.brk || e_trap || e_misalign || e_n64dev;
always_comb begin
    e_badv = e_ea; e_exc_code = 5'd0;
    if (e_fexc) begin e_exc_code = e_fexc_code; e_badv = e_pc; end
    else if (e_cpu1) e_exc_code = 5'd11;
    else if (e_c.ri) e_exc_code = 5'd10;
    else if (e_c.sys) e_exc_code = 5'd8;
    else if (e_c.brk) e_exc_code = 5'd9;
    else if (e_trap) e_exc_code = 5'd13;
    else if (e_misalign || e_n64dev) e_exc_code = e_c.st != S_NONE ? 5'd5 : 5'd4;
end

// multiply / divide (runs in the background; HI/LO interlocked)
localparam bit MD64 = XDIV;
localparam int MW = MD64 ? 64 : 32;         // the divider's width
logic [6:0]      md_cnt;
logic [2*MW-1:0] md_acc;                     // divide: {remainder, quotient}
logic [MW-1:0]   md_dv;
logic            md_nq, md_nr;
logic            md_w;                       // the quotient and remainder are 64 bits
logic            md_dd;                      // (no XDIV) a ddiv of two words: its quotient may be 2^31
logic            md_m32, md_m32b;            // FAST: mult(u)'s product, from its operands kept, and kept itself
// The multiplier: one 33 x 33 signed product (the DSP's), its operands and its result
// in registers, a product two cycles behind M's operands. mult(u) is one of it (FAST);
// dmult(u) four, of the operands' halves, which md_ma and md_mb take in turn:
//   pass 0  aL * bL  (the operands of M, as it starts)
//   pass 1  aL * bH  (md_ma stays)
//   pass 2  aH * bL
//   pass 3  aH * bH  (md_ma stays)
// the halves not at M any more are in md_ah, md_bh and md_bl (aL stays in md_ma; kept
// apart from HI and LO, whose inputs would each take one more source). md_sa and md_sb say which of the two
// operands are signed (the upper half of a signed dmult's); md_psg that the product is.
// The products come out the same four cycles, and are summed a word a time in md_ac
// (md_c counts the cycles since the start: pass k's product is in md_p at cycle 2 + k, in md_p2 at 3 + k).
logic [31:0]     md_ma, md_mb;
logic [63:0]     md_p;
logic            md_sa, md_sb, md_psg, md_psg2;
logic [63:0]     md_p2;                      // dmult(u): md_p once more, so that the sum starts from a register of its own
logic            md_dm, md_sgn;              // a dmult(u) is under way; it is signed
logic            md_mt, md_mth;              // an mthi (md_mth) or mtlo is being written
logic [2:0]      md_c;
logic [66:0]     md_ac;
logic [31:0]     md_ah, md_bh, md_bl;        // the operands' words that are no longer at M (and in no register of the multiplier)

// =============================================================== M
logic        fpu_valid, fpu_busy;
logic [31:0] fpu_gpr_out, fpu_gpr_out_hi;
logic [63:0] fpu_mem_out;
// (FAST: what kind of instruction it is, registered with it)
logic m_k_ld, m_k_st, m_k_wide, m_k_fwide, m_k_cach, m_k_mapped, m_k_md, m_k_fpu, m_k_sbeq;
logic [3:0] m_k_sstrb; logic m_k_sfs, m_k_ssd;      // (FAST: a store of the FPU's word, of a doubleword)
wire m_ld = FAST ? m_k_ld : m_c.ld != M_NONE, m_st = FAST ? m_k_st : m_c.st != S_NONE, m_mem = m_ld || m_st;
// (FAST: what kind of address it is, registered with it)
logic m_f_kseg, m_f_devk;
wire m_kseg = FAST ? m_f_kseg : kseg01(m_addr);
typedef enum logic [2:0] { MS_IDLE, MS_REFILL, MS_REREAD, MS_SLOW_A, MS_SLOW_B, MS_DONE, MS_XLAT, MS_WALK } ms_t;
ms_t ms;
// a mapped access spends a cycle in MS_XLAT with its translation registered
// (m_tlbq): from the micro-TLB, or after a search (MS_WALK)
logic [34:0] m_tlbq;
logic        m_wq;                           // MS_WALK: the scanner took the request
wire m_mapped = FAST ? m_k_mapped : m_mem && !m_kseg;
wire [31:0] m_pa = m_kseg ? kphys(m_addr) : tphys({m_tlbq[31:12], m_addr[11:0]});   // (the offset: the address's own)
wire m_tlb_fault = m_mapped && ms == MS_XLAT && (!m_tlbq[34] || !m_tlbq[33] || (m_st && !m_tlbq[32]));
wire [4:0] m_tlb_code = (!m_tlbq[34] || !m_tlbq[33]) ? (m_st ? 5'd3 : 5'd2) : 5'd1;
wire m_wide = FAST ? m_k_wide :
              (m_c.ld == M_FL && m_insn[31:26] == 6'h35) || (m_c.st == S_FS && m_insn[31:26] == 6'h3D) ||   // ldc1, sdc1
              (W64 && (m_c.ld == M_LD || m_c.st == S_SD));
// the store that completed last cycle: a load right behind it read the RAM in
// the edge the store wrote it; its bytes are merged in
logic        sb_valid; logic [DW-1:0] sb_waddr; logic [31:0] sb_data; logic [3:0] sb_strb;
wire         sb_hit = sb_valid && (FAST ? m_k_sbeq : sb_waddr == m_addr[DW+1:2]);
wire m_fwide = FAST ? m_k_fwide : (m_c.ld == M_FL && m_insn[31:26] == 6'h35) || (m_c.st == S_FS && m_insn[31:26] == 6'h3D);
wire m_cached = FAST ? m_k_cach : cacheable(m_addr);
// the line dropped last cycle (an uncached store into cached memory, sdc1,
// a cache instruction): an access right behind it read the tag in the edge
// that cleared it
logic        tk_valid; logic [DW-4:0] tk_idx;
wire m_dc_hit = dc_tq[16] && dc_tq[15:0] == m_pa[28:13] &&   // (physically tagged; index bits pa == va)
                !(tk_valid && tk_idx == m_addr[DW+1:5]);
// FAST: a load's word is taken as it is; m_lmiss, late in the cycle, says it
// was not its line's. In a delay slot the load has waited a cycle: m_hit_r
logic m_w1, m_hit_r;
wire m_lmiss;
wire m_lhit = !FAST ? m_dc_hit : m_dly ? m_w1 && m_hit_r : 1'b1;
// ld, sd to cached memory: the first word (the lower address) in
// one cycle, its second in the next (m_ph). Both are in one line (it is
// aligned), so the first's hit is the second's. A load's first word waits in
// ms_data; a store's goes into the cache and the write buffer as it is done.
wire m_cw = m_wide && m_cached;
wire m_w0;

logic [63:0] ms_data;                       // the slow path's read (high word = the lower address)
logic [2:0]  ms_cnt;

wire [31:0] dc_qm = {sb_hit && sb_strb[3] ? sb_data[31:24] : dc_q[31:24],
                     sb_hit && sb_strb[2] ? sb_data[23:16] : dc_q[23:16],
                     sb_hit && sb_strb[1] ? sb_data[15:8]  : dc_q[15:8],
                     sb_hit && sb_strb[0] ? sb_data[7:0]   : dc_q[7:0]};
wire [31:0] m_word = (ms == MS_DONE) ? ms_data[31:0] : dc_qm;
// a 64-bit load's value: the slow path's, or the first word kept and the second from the cache
wire [63:0] m_w64 = (ms == MS_DONE) ? ms_data : BIG ? {ms_data[63:32], dc_qm} : {dc_qm, ms_data[31:0]};

logic [31:0] m_ld_val;
always_comb begin
    logic [1:0] k, kl;
    logic [31:0] w, rt;
    // k: the byte's big-endian number in its word (little-endian lwl at byte
    // k is big-endian lwl at 3 - k, the same for lwr/swl/swr); kl its lane
    k = BIG ? m_addr[1:0] : ~m_addr[1:0]; kl = ~k; w = m_word; rt = m_st_lo;
    m_ld_val = 32'd0;
    case (m_c.ld)
    M_LB:  m_ld_val = sx8 (w[kl * 8 +: 8]);
    M_LBU: m_ld_val = {24'd0, w[kl * 8 +: 8]};
    M_LH:  m_ld_val = sx16(w[kl[1] * 16 +: 16]);
    M_LHU: m_ld_val = {16'd0, w[kl[1] * 16 +: 16]};
    M_LW, M_LL, M_FL, M_LWU: m_ld_val = w;
    M_LD:  m_ld_val = m_w64[31:0];                   // (big-endian: the second word, as lw's)
    M_LWL: case (k)
        2'd0: m_ld_val = w;
        2'd1: m_ld_val = {w[23:0], rt[7:0]};
        2'd2: m_ld_val = {w[15:0], rt[15:0]};
        default: m_ld_val = {w[7:0], rt[23:0]};
        endcase
    M_LWR: case (k)
        2'd0: m_ld_val = {rt[31:8], w[31:24]};
        2'd1: m_ld_val = {rt[31:16], w[31:16]};
        2'd2: m_ld_val = {rt[31:24], w[31:8]};
        default: m_ld_val = w;
        endcase
    default: ;
    endcase
end

// (hx) a 32-bit load is its own sign extension, but lwu's, whose upper half is zero
logic [31:0] m_ld_hx;
always_comb begin
    m_ld_hx = 32'd0;
    if (m_c.ld == M_LWU) m_ld_hx = {32{m_word[31]}};
    if (m_c.ld == M_LD)  m_ld_hx = hx32(m_w64);
end

// the 64-bit unit, in two parts. The lower word of a dadd, dsub or a logic operation is E's, as
// any 32-bit instruction's (m_res); M does the upper word (a 32-bit adder, a 32-bit logic unit),
// with the carry of E's adder (m_xc), and gives its hx. A compare (slt, sltu on 64-bit operands)
// comes out of M late: the upper words' compare, and E's compare of the lower words (m_xltu).
// The d-shifts are the shifter's alone, whose result is late (a byte or a bit a cycle).
logic        m_xc, m_xltu;
wire [63:0]  m_a64 = v64(m_a_hx, m_a_lo), m_b64 = v64(m_st_hx, m_st_lo);
wire [31:0]  m_xha = m_a_hx ^ {32{m_a_lo[31]}};
wire [31:0]  m_xhb = m_c.b_imm ? (m_c.imm_zero ? 32'd0 : {32{m_insn[15]}}) : m_st_hx ^ {32{m_st_lo[31]}};
// (FAST: a compare is the 64-bit subtraction too -- E's adder subtracts the lower words for it (e_k_sub), its carry is m_xc,
// and the upper words' go through this adder: no borrow out of it is "not below", its sign decides operands of one sign)
wire         m_xsub = m_c.x64 == X_SUB || (FAST && (m_c.x64 == X_SLT || m_c.x64 == X_SLTU));
wire [32:0]  m_xsum_c = {1'b0, m_xha} + {1'b0, m_xhb ^ {32{m_xsub}}} + 33'(m_xc);
wire [31:0]  m_xsum = m_xsum_c[31:0];
wire         m_xlt = FAST ? (m_c.x64 == X_SLT ? ((m_xha[31] != m_xhb[31]) ? m_xha[31] : m_xsum[31]) : !m_xsum_c[32])
                   : (m_xha != m_xhb) ? (m_c.x64 == X_SLT ? $signed(m_xha) < $signed(m_xhb) : m_xha < m_xhb) : m_xltu;
// the upper word's hx: its difference from the lower word's sign (HI and LO's are stored so)
logic [31:0] m_xhx;
always_comb begin
    case (m_c.x64)
    X_ADD, X_SUB: m_xhx = m_xsum ^ {32{m_res[31]}};
    X_AND:        m_xhx = (m_xha & m_xhb) ^ {32{m_res[31]}};
    X_OR:         m_xhx = (m_xha | m_xhb) ^ {32{m_res[31]}};
    X_XOR:        m_xhx = (m_xha ^ m_xhb) ^ {32{m_res[31]}};
    X_NOR:        m_xhx = ~(m_xha | m_xhb) ^ {32{m_res[31]}};
    X_MFHI:       m_xhx = hi_hx;
    X_MFLO:       m_xhx = lo_hx;
    default:      m_xhx = 32'd0;
    endcase
end
// the d-shifts: the value in sh_r, a byte or a bit a cycle
// (registered with the instruction: M's wait for it is on the D-cache's address path)
logic       m_xshift;
logic       m_shwait;                                      // M's d-shift is not done: !(sh_on && sh_n == 0), registered
wire [5:0]  m_xsh = m_insn[5] ? {m_insn[2], m_insn[10:6]} : m_a_lo[5:0];        // dsll(32) ..., or the v forms
logic [63:0] sh_r; logic [5:0] sh_n; logic sh_on;
wire         m_xcmp = m_c.x64 == X_SLT || m_c.x64 == X_SLTU;

// stores: data and strobes of the word at m_addr (lane 3 - k is N64 byte k);
// sd's by which of its two words is being written
wire m_second = m_cw ? m_ph : ms == MS_SLOW_B;
logic [31:0] m_st_data; logic [3:0] m_st_strb;
always_comb begin
    logic [1:0] k;
    k = BIG ? m_addr[1:0] : ~m_addr[1:0]; m_st_data = 32'd0; m_st_strb = 4'd0;
    case (m_c.st)
    S_SB:  begin m_st_data = {4{m_st_lo[7:0]}};  m_st_strb = 4'b1000 >> k; end
    S_SH:  begin m_st_data = {2{m_st_lo[15:0]}}; m_st_strb = k[1] ? 4'b0011 : 4'b1100; end
    S_SW, S_SC: begin m_st_data = m_st_lo; m_st_strb = 4'b1111; end
    S_SWL: begin m_st_data = m_st_lo >> (8 * k); m_st_strb = 4'b1111 >> k; end
    S_SWR: begin m_st_data = m_st_lo << (8 * (3 - k)); m_st_strb = 4'b1111 << (3 - k); end
    S_FS:  begin m_st_data = (m_fwide && m_second != BIG) ? fpu_mem_out[63:32] : fpu_mem_out[31:0]; m_st_strb = 4'b1111; end
    S_SD:  begin m_st_data = (m_second == BIG) ? m_b64[31:0] : m_b64[63:32]; m_st_strb = 4'b1111; end
    default: ;
    endcase
    if (FAST && !m_k_sfs && !m_k_ssd) begin m_st_data = m_st_lo; m_st_strb = m_k_sstrb; end
    else if (FAST) begin m_st_strb = 4'b1111; m_st_data = m_k_sfs ? ((m_k_fwide && m_second != BIG) ? fpu_mem_out[63:32] : fpu_mem_out[31:0]) :
                                                      (m_second == BIG) ? m_b64[31:0] : m_b64[63:32]; end
end

wire [31:0] cause_now = {c0_cause[31:11], irq_rcp, c0_cause[9:0]};
logic [31:0] c0_rd;
always_comb begin
    case (m_insn[15:11])
    5'd0: c0_rd = c0_index;
    5'd1: c0_rd = {27'd0, rnd_now};
    5'd2: c0_rd = c0_entrylo0; 5'd3: c0_rd = c0_entrylo1; 5'd4: c0_rd = c0_context;
    5'd5: c0_rd = c0_pagemask; 5'd6: c0_rd = c0_wired;    5'd8: c0_rd = c0_badvaddr;
    // (31:28: which of the 64-bit levels this core has -- W64_ALU, W64_SH, W64_MD, W64_DIV; a core before them reads 0 there)
    5'd7: c0_rd = {XALU, XSH, XMD, XDIV, 17'd0, MON ? mon_mem4 : 1'b0, 1'b0, MON ? mon_mi : 1'b0, MON ? {mon_code, mon_bd, mon_game} : 7'd0, hilo_soft};
    5'd23: c0_rd = TLB_RAM ? {28'h5442000, tr_lastf, 2'b00, MON} : 32'd0;     // (bit 0: the game's TLB instructions are the monitor's)
    5'd22: c0_rd = (TLB_RAM && MON) ? mon_badv : 32'd0;
    5'd9: c0_rd = (SIM_COUNT_RETIRE && count_frac) ? c0_count + 32'd1 : c0_count;   // (the model counts before executing)
    5'd10: c0_rd = c0_entryhi; 5'd11: c0_rd = c0_compare;
    5'd12: c0_rd = c0_status;  5'd13: c0_rd = cause_now;  5'd14: c0_rd = c0_epc;   // (Cause: IP2 live, as the model)
    5'd15: c0_rd = 32'h00000B22; 5'd16: c0_rd = c0_config; 5'd17: c0_rd = c0_lladdr;
    5'd18: c0_rd = c0_watchlo; 5'd19: c0_rd = c0_watchhi;
    5'd28: c0_rd = c0_taglo;   5'd30: c0_rd = c0_errorepc;
    default: c0_rd = 32'd0;
    endcase
end

// interrupts: taken before the instruction at M (not while a slow access is under way)
assign hwx_on = MI && mon_mi;
assign dbg_pc = f_pc; assign dbg_cause = cause_now; assign dbg_status = c0_status; assign dbg_epc = c0_epc;
wire int_game = c0_status[0] && !c0_status[1] && !c0_status[2] && ((cause_now[15:8] & c0_status[15:8]) != 0);
wire int_mon  = MON && mon_game && irq_mon && !c0_status[2];       // (the game cannot mask it; ERL does)
wire int_pending = int_game || int_mon;
// (FAST: as it was a cycle ago. What changes Status waits for the pipeline to
// empty and holds what follows three cycles behind; a device's store is
// acknowledged a cycle before it completes)
logic int_pend_r, int_mon_r;
always_ff @(posedge clk) begin int_pend_r <= int_pending; int_mon_r <= int_mon; end
wire m_take_int = m_valid && (FAST ? int_pend_r : int_pending) && ms == MS_IDLE && !m_ph;
wire m_exc_v = m_exc && !(m_hwx && m_dev);          // (MI: the game's word access to a register the hardware has is no exception)
wire m_take_exc = m_valid && !m_take_int && (m_exc_v || m_tlb_fault);
wire [4:0] m_code = m_exc_v ? m_exc_code : m_tlb_code;
wire m_refill = !m_exc_v && m_tlb_fault && !m_tlbq[34];
// MON: what goes to the monitor level -- the interrupt of its own; a device
// access, an instruction the core lacks (RI), a translation fault (its entry is
// the runtime's to fill); nothing already in it
wire m_mon_int = MON && (FAST ? int_mon_r : int_mon);
wire m_to_mon  = MON && mon_game && !c0_status[2] &&
                 (m_take_int ? m_mon_int : (m_take_exc && (m_exc_v ? (m_dev || m_exc_code == 5'd10 || (TLB_RAM && m_exc_code == 5'd2)) : 1'b1)));
// a translation fault (a fetch's, code 2 from E, or an access's, found here) that the monitor level takes
wire m_tr_mon  = TLB_RAM && m_to_mon && m_take_exc && (!m_exc_v || m_exc_code == 5'd2);
// (the game's own handler: an exception of the game's code that a console takes)
wire m_to_game = MON && mon_game && (m_take_int || m_take_exc) && !m_to_mon && n64code(m_pc);
wire m_live = m_valid && !m_take_int && !m_take_exc;

logic m_k_sc;                                // (FAST: sc, registered with it)
wire m_sc_fail = (FAST ? m_k_sc : m_c.st == S_SC) && !ll_bit;
// a store to a device (not RDRAM, not the cartridge) completes when the device
// says so -- a DMA or a task it starts has finished by then, as in the model
function automatic logic pa_dev(input logic [31:0] pa);
    return FLAT ? !pa_cached(pa) : !(pa < 32'h03F0_0000) && !(pa >= 32'h1000_0000 && pa < 32'h1FC0_0000);
endfunction
// (FAST, an unmapped address: m_f_devk, registered; a mapped one: the table's page. Not pa_dev(m_pa) for both, which would
// put m_addr -> kphys -> pa_dev into this mux's cone -- and into m_needs_slow, m_done and every stall of the pipeline -- for a case the mux never takes)
wire m_devst = m_st && !m_wide && (FAST ? (m_kseg ? m_f_devk : pa_dev(tphys({m_tlbq[31:12], m_addr[11:0]}))) : pa_dev(m_pa));
// the slow path: uncached / device / mapped loads, device stores, 64-bit
// accesses to anything not cached; other stores (sc too) go through the write
// buffer whatever the region
wire m_needs_slow = m_mem && !m_sc_fail && ((m_wide && !m_cached) || (m_ld && !m_cached) || m_devst);
// a store's word this cycle: of a 64-bit one to cached memory, the half by m_ph
wire [31:0] m_cst_data = m_st_data;
wire [3:0]  m_cst_strb = m_st_strb;
wire [DW-1:0] m_cst_idx = m_cw ? {m_addr[DW+1:3], m_ph} : m_addr[DW+1:2];
wire [31:0] m_cst_pa   = m_cw ? {m_pa[31:3], m_ph, 2'b00} : {m_pa[31:2], 2'b00};
wire m_is_md = FAST ? m_k_md : m_c.md != MD_NONE;
// a store into the write buffer as it completes (a 64-bit one's first word: m_w0)
wire m_wb_push = (m_commit_e || m_w0) && m_st && (!m_wide || m_cw) && !m_sc_fail && !m_devst;
// XWB: and the slow path's stores, a word as soon as it is taken (a device's
// then waits for st_done: sw_wait)
logic sw_wait;
wire m_sw = XWB && m_live && m_st && (ms == MS_SLOW_A || ms == MS_SLOW_B);
wire m_sw_push = m_sw && !sw_wait && !st_full;
assign st_valid = XWB && (m_wb_push || m_sw_push);
assign st_dev   = m_devst;
assign st_addr  = {m_pa[31:3], m_cw ? m_ph : m_sw ? (ms == MS_SLOW_B || (!m_wide && m_pa[2])) : m_pa[2], 2'b00};
assign st_data  = m_st_data;
assign st_strb  = m_st_strb;
// an index cache op's lines when a cache has more than the VR4300's (see the cache RAMs' writes)
// (PG: a hit-type op on the I-cache, once a mapped page may have its lines under another colour than the physical
// address's, drops the line under every colour: IC_LOG2 - 7 index bits above the page offset, ck_n steps through them)
localparam int CK_WI = PG ? IC_LOG2 - 8 : IC_LOG2 - 9;
localparam int CK_W = (CK_WI > DC_LOG2 - 8) ? CK_WI : DC_LOG2 - 8;
logic [CK_W:0] ck_n;
logic          ic_alias;                     // a mapped page was entered at a colour that is not its physical address's
wire           ck_isw = PG && ic_alias && m_insn[17:16] == 2'd0 && m_insn[20];
wire           ck_run = !m_insn[20] || ck_isw;
wire  [CK_W:0] ck_last = ck_isw ? (CK_W + 1)'((1 << (IC_LOG2 - 7)) - 1) :
                         (CK_W + 1)'((m_insn[17:16] == 2'd0 ? (1 << (IC_LOG2 - 9)) : (1 << (DC_LOG2 - 8))) - 1);
logic m_done;
always_comb begin
    m_done = 1'b1;
    // (FAST: as if live -- an interrupt or an exception flushes, whatever M's wait)
    if (FAST ? m_valid : m_live) begin
        if (m_cw) m_done = m_ph && !(m_st && wb_full);
        else if (m_ld && m_cached) m_done = ms == MS_DONE || (ms == MS_IDLE && m_lhit);
        else if (m_needs_slow) m_done = ms == MS_DONE;
        else if (m_st && !m_sc_fail) m_done = !wb_full;
        // translating first (a store too: one whose page was not the last
        // translated completed during the search, on the page before's)
        if (m_mapped && (ms == MS_IDLE || ms == MS_WALK)) m_done = 1'b0;
        if (m_c.tlbp || m_c.tlbr) m_done = ms == MS_DONE;           // a search / a read of the TLB RAM
        if ((FAST ? m_k_fpu : (m_c.cop1 || m_c.ld == M_FL || m_c.st == S_FS)) && fpu_busy) m_done = 1'b0;
        if (m_is_md && md_busy) m_done = 1'b0;
        // (FAST: a line on its way in for a load that is on its way back)
        if (FAST && (ms == MS_REFILL || ms == MS_REREAD)) m_done = 1'b0;
        if (m_shwait) m_done = 1'b0;                                // (a d-shift: still shifting)
        if (CK_W != 0 && m_c.cache && ck_run && ck_n != ck_last) m_done = 1'b0;
    end
end
wire m_stall = (FAST ? m_valid : m_live) && !m_done;
wire m_cache_go = m_live && m_c.cache && !(FAST && (ms == MS_REFILL || ms == MS_REREAD));
assign m_w0 = m_live && m_cw && !m_ph && ms == MS_IDLE && (m_st ? !wb_full : m_lhit);
assign m_lmiss = FAST && m_live && m_ld && m_cached && !m_dly && ms == MS_IDLE && !m_ph && !m_dc_hit;
wire m_commit_e = m_live && m_done;         // (M always advances when done)
wire m_commit = m_commit_e && !m_lmiss;     // (FAST: but a load of the wrong line)

// =============================================================== pipeline advance
wire m_adv = !m_stall;
wire e_adv = m_adv;
wire d_adv = e_adv && !d_stall;
wire flush = m_take_int || m_take_exc || (m_commit_e && m_c.eret) || (FAST && rp_pend);
logic [31:0] flush_pc;
always_comb begin
    if (FAST && rp_pend) flush_pc = rp_m ? m_pc : w_pc;
    else if (m_commit_e && m_c.eret) flush_pc = c0_status[2] ? c0_errorepc : c0_epc;
    else if (m_to_game) flush_pc = 32'h8000_0180;
    else flush_pc = (c0_status[22] ? 32'hBFC00200 : EXC_BASE) + ((m_refill && !c0_status[1] && !FLAT) ? 32'h0 : 32'h180);   // (FLAT: one vector -- a miss there is the runtime's to report)
end

// =============================================================== F
typedef enum logic [2:0] { FS_RUN, FS_REFILL, FS_SLOW, FS_READY, FS_XLAT, FS_WALK } fs_t;
fs_t fs;
wire f_kseg = kseg01(f_pc);
wire f_cached = cacheable(f_pc);
// The page registers (PG): two of the fetch table's pages, the last that a walk found cacheable -- the virtual page
// (va[31:12]: the mapped ranges differ in all four top bits), the entry's global bit and ASID, the
// physical page (pa[22:12]: the N64's RDRAM). A mapped fetch is taken as the I-cache's, like KSEG0's (f_xmap: F
// delivers the cache's word at once, FAST), and is checked a cycle later, in D, as KSEG0's tag is: d_fmiss says the
// word is not that of the page's line -- the I-cache is indexed by the virtual address (as the VR4300's) and
// tagged by the physical one, which the page register gives (pg_tag) -- or that no register holds the page.
// d_imiss then fetches the instruction again: from a register that holds the page, the line is read in (FS_REFILL,
// d_fpfn is where from); from none, the table is walked (FS_WALK, FS_XLAT) -- the entry loads a register
// (pg_ld) and F goes back to FS_RUN -- or, for a page that is not cacheable, fetched uncached as ever (FS_SLOW).
// A write to COP0 23 (any table entry) empties the registers; an ASID that is not the entry's is no hit.
// Aliases: the index comes from the virtual address, so one physical line can sit under two indexes when a page is
// mapped at a virtual address whose bits [IW+1:12] differ from the physical ones (the VR4300's cache has this
// with its two colour bits too); a hit-type cache op on the I-cache then drops the line under every colour (ic_alias).
typedef struct packed { logic [19:0] vp; logic g; logic [7:0] asid; logic [10:0] pfn; } pg_t;
pg_t         pg [2];
logic [1:0]  pg_v;
logic        pg_mru;                         // the register hit or loaded last
logic        pg_inv_r;                       // a table write last cycle
wire         f_xmap = PG && !f_kseg;
wire         f_cx = f_cached || f_xmap;
wire [19:0]  f_vp = f_pc[31:12];
wire         f_mhit0 = PG && pg_v[0] && pg[0].vp == f_vp && (pg[0].g || pg[0].asid == c0_entryhi[7:0]);
wire         f_mhit1 = PG && pg_v[1] && pg[1].vp == f_vp && (pg[1].g || pg[1].asid == c0_entryhi[7:0]);
// (physical address bits [28:12] -- the tag, TLO: the RDRAM's place in the SDRAM, then pa[22:12])
wire [16:0]  pg0_tag = {N64_RDRAM[28:23], pg[0].pfn};
wire [16:0]  pg1_tag = {N64_RDRAM[28:23], pg[1].pfn};
logic        d_fmap, d_fphit;                // D's word: a mapped fetch; a page register held its page
logic [10:0] d_fpfn;                         // (and its physical page)
logic [IW+1:12] f_fill_col;                  // the index bits above the page offset of the line that is coming in
// a mapped fetch: translated in FS_XLAT (registered), then fetched uncached
logic [34:0] f_tlbq;
logic        f_wq;                           // FS_WALK: the scanner took the request
logic        f_xfault;                       // FS_READY holds a translation fault, not an instruction
logic        f_tc;                           // FS_XLAT: the page is cacheable
wire f_misal = f_pc[1:0] != 2'd0;
wire f_fault = f_misal || (fs == FS_READY && f_xfault);
wire [4:0] f_fault_code = f_misal ? 5'd4 : 5'd2;
logic [31:0] f_slow_insn;
logic [2:0]  f_cnt;
logic [31:0] f_fill_pa;
logic        f_discard;
wire [31:0] f_pa_k = kphys(f_pc);
wire f_hit_k = f_ok && f_cached && !f_misal && ic_tq[ITW] && ic_tq[ITW-1:0] == f_pa_k[28:TLO];
wire f_hit_m = PG && f_ok && !f_misal && ic_tq[ITW] && ((f_mhit0 && ic_tq[ITW-1:0] == pg0_tag) || (f_mhit1 && ic_tq[ITW-1:0] == pg1_tag));
wire f_hit = f_hit_k || f_hit_m;
wire f_have = f_ok && (fs == FS_READY || (fs == FS_RUN && ((FAST ? f_cx : f_hit) || f_misal)));
wire f_take = !d_valid || d_adv;
// (FAST) the fetch went the wrong way behind the delay slot: to rd_target now
wire b_redir = FAST && rd_res && rd_fix && !ds_next;
wire f_deliver = f_have && f_take && !flush && !d_imiss && !b_redir;
// (what the fetch has under way is dropped by a flush, and by that)
wire f_kill = flush || b_redir;
wire [31:0] f_insn = fs == FS_READY ? f_slow_insn : ic_q;

// the register file's read (see rfq_*)
wire [31:0] rf_rinsn = f_deliver ? (f_fault ? 32'd0 : f_insn) : d_insn;
always_ff @(posedge clk) begin
    rfq_a <= rf_lo[rf_rinsn[25:21]]; rfq_b <= rf_lo[rf_rinsn[20:16]];
    if (w_wr) rf_lo[w_c.rd] <= w_res;
    rf_xv <= w_wr; rf_xrd <= w_c.rd; rf_xlo <= w_res;
end
generate if (W64) begin : g_hx
    logic [31:0] rf_hx [32];
    logic        rf_hw [32];
    initial for (int i = 0; i < 32; i++) begin rf_hx[i] = 0; rf_hw[i] = 0; end
    always_ff @(posedge clk) begin
        rfq_ah <= rf_hx[rf_rinsn[25:21]]; rfq_bh <= rf_hx[rf_rinsn[20:16]];
        rfq_aw <= rf_hw[rf_rinsn[25:21]]; rfq_bw <= rf_hw[rf_rinsn[20:16]];
        if (w_wr) begin rf_hx[w_c.rd] <= w_hx; rf_hw[w_c.rd] <= w_hx != 32'd0; end
        rf_xw <= wideprod(w_c);
    end
end else begin : g_nohx
    always_comb begin rfq_ah = 32'd0; rfq_bh = 32'd0; rfq_aw = 1'b0; rfq_bw = 1'b0; rf_xw = 1'b0; end
end endgenerate

// the delay slot's branch: decided this cycle (leaving D) or earlier
wire cti_now = d_ok && d_adv && d_cti;
wire rd_valid = cti_now || rd_pend;
wire rd_t = cti_now ? d_taken : rd_taken;
wire rd_a = cti_now ? (d_c.likely && !d_taken) : rd_annul;
wire [31:0] rd_tg = cti_now ? d_target : rd_target;
// the instruction F delivers now is a delay slot: its branch left D earlier, or leaves now
wire ds_now = ds_next || cti_now;

logic [31:0] f_next;
always_comb begin
    if (flush) f_next = flush_pc;
    else if (f_deliver) f_next = (ds_now && rd_valid && rd_t) ? rd_tg : f_pc + 32'd4;
    else f_next = f_pc;
end
// FAST: the next fetch, when there is one -- F holds an instruction D cannot
// take (f_hold) with the I-cache's read kept, so whether D's branch leaves
// now is not asked here: if it does not, nothing is fetched
wire f_hold = f_have && !f_take;
logic [31:0] f_nx;
always_comb begin
    if (flush) f_nx = flush_pc;
    else if (d_imiss) f_nx = d_pc;
    else if (b_redir) f_nx = rd_target;
    else if (!f_have) f_nx = f_pc;
    else if (ds_next) f_nx = (rd_res || rd_pend) ? rd_target : f_pc + 32'd4;    // (the delay slot of a branch decided, or not yet)
    else if (d_ok && d_k_cti) f_nx = d_k_pred ? d_ptarget : f_pc + 32'd4;
    else f_nx = f_pc + 32'd4;
end
always_comb begin
    ic_ren = !FAST || !f_hold || flush || d_imiss || b_redir;
    ic_raddr = FAST ? f_nx[IW+1:2] : f_next[IW+1:2];
end

// =============================================================== the TLB scanner
wire sc_req_m = ms == MS_WALK && !m_wq;
wire sc_req_f = fs == FS_WALK && !f_wq && !f_discard && !f_kill;
wire sc_start = sc == SC_IDLE && (sc_req_m || sc_req_f);
wire sc_done_m = sc == SC_DONE && sc_for_m, sc_done_f = sc == SC_DONE && !sc_for_m;
wire sc_match = TLB_EN && tlb_match(tlb_q, sc_va, sc_asid);
// TLB_RAM: the entry of the page asked for (read as the scanner starts), and its write
wire [31:0] sc_va_n = !sc_req_m ? f_pc : m_c.tlbp ? c0_entryhi : m_addr;
always_comb begin
    tr_raddr = sc_va_n[20:12];
    tr_we_d  = TLB_RAM && m_commit_e && m_c.mtc0 && m_insn[15:11] == 5'd23 && !m_st_lo[3];
    tr_we_i  = TLB_RAM && m_commit_e && m_c.mtc0 && m_insn[15:11] == 5'd23 && m_st_lo[3];
    tr_waddr = {c0_entryhi[20:13], m_st_lo[4]};
    tr_wdata = '{v: m_st_lo[0], d: m_st_lo[1], g: m_st_lo[2], c: m_st_lo[5], asid: c0_entryhi[7:0],
                 tag: c0_entryhi[31:21], pfn: m_st_lo[28:12]};
end
// (what asked waits for the answer: its address and the ASID are as they were)
tre_t tr_q;
always_comb tr_q = sc_for_m ? tr_qd : tr_qi;
wire [10:0] tr_tag = sc_for_m ? m_addr[31:21] : f_pc[31:21];
wire tr_hit = tr_q.v && tr_q.tag == tr_tag && (tr_q.g || tr_q.asid == c0_entryhi[7:0]);
always_comb begin
    tlb_raddr = sc_k + 5'd1;                                  // the next entry
    if (sc_start) tlb_raddr = (sc_req_m && m_c.tlbr) ? c0_index[4:0] : 5'd0;
    // writes: tlbwi / tlbwr, as they complete
    tlb_we = TLB_EN && m_commit_e && (m_c.tlbwi || m_c.tlbwr);
    tlb_waddr = m_c.tlbwi ? c0_index[4:0] : rnd_now;
    tlb_wdata.mask = c0_pagemask[24:13];
    tlb_wdata.vpn  = c0_entryhi[31:13] & ~{7'd0, c0_pagemask[24:13]};
    tlb_wdata.asid = c0_entryhi[7:0];
    tlb_wdata.g    = c0_entrylo0[0] & c0_entrylo1[0];
    tlb_wdata.lo0  = c0_entrylo0[31:1];
    tlb_wdata.lo1  = c0_entrylo1[31:1];
end

// =============================================================== memory port
typedef enum logic [2:0] { P_IDLE, P_WB, P_ILINE, P_DLINE, P_SLOW } port_t;
port_t port;
// the line M's refill reads (FAST: its load is not in M meanwhile)
wire [31:0] m_fpa = FAST ? d_fill_pa : m_pa;
logic [2:0] port_cnt;
logic        slow_we; logic [31:0] slow_pa, slow_wdata; logic [3:0] slow_strb;
// whose the slow request is: fixed when the port is granted -- M may enter
// its slow path while F's uncached fetch is out (the address must not change
// under it, nor its word complete M's load)
always_comb slow_is_d = port == P_SLOW ? slow_d_own : (ms == MS_SLOW_A || ms == MS_SLOW_B);
always_comb begin
    slow_we = 1'b0; slow_pa = {f_fill_pa[31:2], 2'b00}; slow_wdata = 32'd0; slow_strb = 4'd0;
    if (slow_is_d) begin
        slow_pa = {m_pa[31:3], ms == MS_SLOW_B ? 1'b1 : (m_wide ? 1'b0 : m_pa[2]), 2'b00};
        slow_we = m_st;
        if (m_wide) begin                                   // sd, sdc1: lower address first
            slow_wdata = m_c.st == S_SD ? m_st_data : (ms == MS_SLOW_B) == BIG ? fpu_mem_out[31:0] : fpu_mem_out[63:32];
            slow_strb  = 4'hF;
        end else begin                                      // a device store
            slow_wdata = m_st_data; slow_strb = m_st_strb;
        end
    end
end
always_comb begin
    mem_req = 1'b0; mem_addr = 32'd0; mem_we = 1'b0; mem_wdata = 32'd0; mem_wstrb = 4'd0; mem_line = 1'b0;
    case (port)
    P_WB:    if (!XWB) begin mem_req = 1'b1; mem_we = 1'b1; mem_addr = wb_addr[wb_rp[2:0]]; mem_wdata = wb_data[wb_rp[2:0]]; mem_wstrb = wb_strb[wb_rp[2:0]]; end
    P_ILINE: begin mem_req = 1'b1; mem_line = 1'b1; mem_addr = {f_fill_pa[31:5], 5'd0}; end
    P_DLINE: begin mem_req = 1'b1; mem_line = 1'b1; mem_addr = {m_fpa[31:5], 5'd0}; end
    P_SLOW:  begin mem_req = 1'b1; mem_we = slow_we; mem_addr = slow_pa; mem_wdata = slow_wdata; mem_wstrb = slow_strb; end
    default: ;
    endcase
end
assign mem_wrun = !XWB && port == P_WB;
wire port_done = (port == P_SLOW || port == P_WB) ? ((mem_we && mem_ack) || (!mem_we && mem_rvalid)) : 1'b0;

// =============================================================== the page registers (PG; see above F)
wire pg_ld = PG && sc_done_f && sc_hit && sc_x.c;                    // F's own walk found a cacheable page
wire pg_vic = !pg_v[0] ? 1'b0 : !pg_v[1] ? 1'b1 : !pg_mru;          // the register to load: a free one, else the older
always_ff @(posedge clk) begin
    if (rst) begin pg_v <= 2'b00; pg_mru <= 1'b0; pg_inv_r <= 1'b0; ic_alias <= 1'b0; end
    else if (PG) begin
        pg_inv_r <= tr_we_i || tr_we_d;
        if (f_mhit1) pg_mru <= 1'b1; else if (f_mhit0) pg_mru <= 1'b0;
        if (pg_ld) begin
            pg[pg_vic] <= '{vp: sc_va[31:12], g: sc_x.g, asid: sc_x.asid, pfn: sc_x.pfn[10:0]};
            pg_v[pg_vic] <= 1'b1; pg_mru <= pg_vic;
            if (sc_va[IW+1:12] != sc_x.pfn[IW-11:0]) ic_alias <= 1'b1;
        end
        if (pg_inv_r) pg_v <= 2'b00;                                 // (a table entry was written: whatever the registers hold may be stale)
    end
end

// =============================================================== cache RAM writes
always_comb begin
    ic_we = 0; ic_waddr = 0; ic_wdata = 0; ict_we = 0; ict_waddr = 0; ict_wdata = 0;
    dc_we = 0; dc_waddr = 0; dc_wdata = 0; dc_wbe = 0;
    // (SNOOP: a line another master wrote is dropped in a cycle the core writes no tag, sn_go says)
    dct_we = sn_go; dct_waddr = sn_idx; dct_wdata = 0;
    if (port == P_ILINE && mem_rvalid) begin
        ic_we = 1; ic_waddr = {PG ? {f_fill_col, f_fill_pa[11:5]} : f_fill_pa[IW+1:5], f_cnt}; ic_wdata = mem_rdata;
        if (f_cnt == 3'd7) begin ict_we = 1; ict_waddr = PG ? {f_fill_col, f_fill_pa[11:5]} : f_fill_pa[IW+1:5]; ict_wdata = {1'b1, f_fill_pa[28:TLO]}; end
    end
    if (port == P_DLINE && mem_rvalid) begin
        dc_we = 1; dc_wbe = 4'hF;
        if (ms_cnt == 3'd7) begin dct_we = 1; dct_waddr = m_fpa[DW+1:5]; dct_wdata = {!sn_kill, m_fpa[28:13]}; end
    end
    // a store's word: into the D-cache (hit), or drop the line an uncached store may shadow
    if ((m_commit_e || m_w0) && m_st && !m_sc_fail) begin
        if (m_cached && m_dc_hit) begin
            dc_we = 1; dc_wbe = m_cst_strb;
        end else if (!m_cached && pa_cached(m_pa)) begin
            dct_we = 1; dct_waddr = m_pa[DW+1:5]; dct_wdata = 17'd0;
        end
    end
    // (the D-cache's write address and data do not wait for the conditions of the write -- they are
    // the enables' -- but go by the port only: no store completes while a line is coming in)
    dc_waddr = port == P_DLINE ? {m_fpa[DW+1:5], ms_cnt} : m_cst_idx;
    dc_wdata = port == P_DLINE ? mem_rdata : m_cst_data;
    // cache instructions: invalidate a line (every op but the fills; a write-through D-cache has nothing to write back)
    // (a cache of more lines than the VR4300's: an index op -- software that
    // empties "the" cache walks the VR4300's size -- drops every line the
    // VR4300's index stands for, one a cycle: ck_n)
    if (m_cache_go) begin
        if (m_insn[17:16] == 2'd0) begin ict_we = 1; ict_waddr = m_addr[IW+1:5]; if (!m_insn[20]) ict_waddr = IC_LOG2'({ck_n, m_addr[13:5]}); else if (ck_isw) ict_waddr = IC_LOG2'({ck_n, m_addr[11:5]}); ict_wdata = '0; end
        else if (m_insn[17:16] == 2'd1) begin dct_we = 1; dct_waddr = m_addr[DW+1:5]; if (!m_insn[20]) dct_waddr = DC_LOG2'({ck_n, m_addr[12:5]}); dct_wdata = 17'd0; end
    end
end
// the D-cache read: the next M's address, or M's own while it waits
// (a cached 64-bit load: its second word once the first is taken)
always_comb dc_raddr = m_stall ? {m_addr[DW+1:3], (m_cw && (m_w0 || m_ph)) ? 1'b1 : m_addr[2]} : e_ea[DW+1:2];

// =============================================================== the snoop (SNOOP)
// The D-cache's lines are 32 bytes, the VR4300's 16. Software drops a buffer's lines before another master writes
// into it, but a buffer that is 16-byte aligned shares a line with the 16 bytes beside it, and a load of those while
// the transfer is under way reads the line back from memory without the transfer's words: on the console the two are
// different lines. So the line of every write of another master is looked up, and dropped when the cache holds it.
//   S  sn_t and sn_line sampled; a new write when sn_t has turned over
//   R  its index read from a copy of the tags (sn_dup: the valid bit and the nine tag bits above the index, written
//      with the tags: its read port is the snoop's alone, so a lookup never waits); the line M's refill reads, if it
//      is this one, comes in invalid (sn_kill: it may hold words from before the write)
//   C  the copy compared; a hit is registered
//   Q  the hit's index queued (sn_q), and dropped from the queue's head through the tags' write port in a cycle where
//      the core writes no tag (a refill's last word, an uncached store into cached memory and a cache instruction
//      may write one: sn_core). With two or more waiting, sn_hold holds the other masters' writes.
// A drop of a line the core has written since (nine tag bits are not all of them) only costs a refill. The core's own
// accesses take the cycles they took without it, but where a drop or sn_kill changes what a tag says.
generate if (SNOOP) begin : g_sn
    logic          sn_ts, sn_seen, r_kill, c_v, h_v;
    logic [20:0]   sn_ls;
    logic [DC_LOG2+8:0] c_line;                          // (the index and the nine bits the copy has)
    logic [9:0]    c_q;                                  // the copy's entry at R's index
    logic [9:0]    sn_dup [DC_LINES];
    logic [DW-4:0] h_idx;
    (* ramstyle = "MLAB" *) logic [DW-4:0] sn_q [8];      // (not an M10K of its own)
    logic [2:0]    q_r, q_w;
    logic [3:0]    q_n;
    wire           r_v = sn_ts != sn_seen;
    wire           r_fill = r_v && ms == MS_REFILL && sn_ls == m_fpa[25:5];
    wire           sn_core = (port == P_DLINE && ms_cnt == 3'd7) || (m_valid && m_c.cache) ||
                             (m_valid && m_st && !m_cached && pa_cached(m_pa));
    wire  [3:0]    q_nn = q_n + {3'd0, h_v} - {3'd0, sn_go};
    assign sn_go   = q_n != 4'd0 && !sn_core;
    assign sn_idx  = sn_q[q_r];
    assign sn_kill = r_kill || r_fill;
    always_ff @(posedge clk) begin
        sn_ts <= sn_t; sn_ls <= sn_line; sn_seen <= sn_ts;
        c_line <= sn_ls[DC_LOG2+8:0];
        c_q <= sn_dup[sn_ls[DC_LOG2-1:0]];
        if (dct_we) sn_dup[dct_waddr] <= {dct_wdata[16], dct_wdata[DC_LOG2:DC_LOG2-8]};
        h_idx <= c_line[DC_LOG2-1:0];
        if (h_v) sn_q[q_w] <= h_idx;
        if (rst) begin c_v <= 1'b0; h_v <= 1'b0; r_kill <= 1'b0; q_r <= '0; q_w <= '0; q_n <= '0; sn_hold <= 1'b0; end
        else begin
            c_v <= r_v;
            h_v <= c_v && c_q[9] && c_q[8:0] == c_line[DC_LOG2+8:DC_LOG2];
            if (ms != MS_REFILL) r_kill <= 1'b0; else if (r_fill) r_kill <= 1'b1;
            if (h_v) q_w <= q_w + 3'd1;
            if (sn_go) q_r <= q_r + 3'd1;
            q_n <= q_nn;
            sn_hold <= q_nn >= 4'd2;
        end
    end
end else begin : g_nosn
    assign sn_go = 1'b0; assign sn_idx = '0; assign sn_kill = 1'b0;
    always_comb sn_hold = 1'b0;
end endgenerate

// =============================================================== retire trace
always_comb begin
    tr_retire = w_ok;
    tr_pc = w_pc; tr_insn = w_insn;
    tr_wr = w_wr; tr_rd = w_c.rd; tr_val = v64(w_hx, w_res);
    tr_exc = m_take_int || m_take_exc;
    tr_exc_int = m_take_int;
    tr_exc_pc = m_pc;
    tr_exc_code = m_take_int ? 5'd0 : m_code;
end

// COP1: at M; its results are written as the instruction completes
wire m_fpu = FAST ? m_k_fpu : m_c.cop1 || m_c.ld == M_FL || m_c.st == S_FS;
fpu_n64 #(.FAST(FAST)) fpu (
    .clk, .rst,
    .insn_next(m_adv ? e_insn : m_insn), .insn(m_insn), .insn_e(e_insn), .adv(m_adv),
    .live(m_live && m_fpu), .valid(fpu_valid),
    .cancel(FAST && w_lm), .gpr_in(m_st_lo), .gpr_in_hi(m_st_hx ^ {32{m_st_lo[31]}}), .mem_in(m_wide ? m_w64 : {32'd0, m_word}),
    .gpr_out(fpu_gpr_out), .gpr_out_hi(fpu_gpr_out_hi), .mem_out(fpu_mem_out), .cond(fpu_cond), .busy(fpu_busy),
    .tr_we_even(tr_fwe_even), .tr_we_odd(tr_fwe_odd), .tr_widx(tr_fwidx),
    .tr_weven(tr_fweven), .tr_wodd(tr_fwodd), .tr_fcr31(tr_fcr31)
);
assign fpu_valid = (FAST ? m_commit_e : m_commit) && m_fpu;    // (FAST: a load's write is taken back with w_lm)

// (FAST, and dmult(u)) the operands and the product in registers of their own (the
// DSP's): taken every cycle, the product two cycles behind M's operands. One
// multiplier, signed, of 33 bits: an operand's 33rd is its sign, or 0 (multu)
always_ff @(posedge clk) begin
    if (!(XMD && md_dm && (md_c == 3'd1 || md_c == 3'd3))) md_ma <= (XMD && md_dm && md_c == 3'd2) ? md_ah : m_a_lo;
    md_mb <= !(XMD && md_dm) ? m_st_lo : md_c == 3'd2 ? md_bl : md_c == 3'd0 ? m_st_lo : md_bh;
    md_p <= 64'($signed({md_sa & md_ma[31], md_ma}) * $signed({md_sb & md_mb[31], md_mb}));
    md_psg <= md_sa | md_sb;
    md_p2 <= md_p; md_psg2 <= md_psg;
    // dmult(u): the words of M's operands that pass 0 does not take, kept as it starts: they follow M's
    // until then (no enable of M's to wait for) and stay as md_dm says so
    if (XMD && !md_dm) begin
        md_ah <= m_a64[63:32]; md_bh <= m_b64[63:32]; md_bl <= m_st_lo; md_sgn <= m_c.md == MD_DMULT;
    end
end

// =============================================================== sequential
integer k;
always_ff @(posedge clk) begin
    if (rst) begin
        f_pc <= reset_pc; f_ok <= 0; fs <= FS_RUN; ds_next <= 0; rd_pend <= 0; f_discard <= 0;
        d_valid <= 0; e_valid <= 0; m_valid <= 0; w_valid <= 0;
        c0_status <= 32'h34000000; c0_config <= 32'h7006E463; c0_cause <= 0; c0_count <= 0; c0_compare <= 0;
        c0_wired <= 0; c0_index <= 0; c0_entryhi <= 0; c0_entrylo0 <= 0; c0_entrylo1 <= 0; c0_pagemask <= 0;
        c0_context <= 0; c0_epc <= 0; c0_errorepc <= 0; c0_badvaddr <= 0; c0_lladdr <= 0; c0_watchlo <= 0;
        c0_watchhi <= 0; c0_taglo <= 0;
        count_frac <= 0; c0_random <= 5'd31; ll_bit <= 0; fcc <= 0;
        hi_r <= 0; lo_r <= 0; hi_hx <= 0; lo_hx <= 0; hi_wide <= 0; lo_wide <= 0; m_ph <= 0; sh_on <= 0; m_shwait <= 0; m_xshift <= 0;
        hilo_soft <= 0; tr_lastf <= 0; mon_game <= 0; mon_mi <= 0; mon_bd <= 0; mon_code <= 0; mon_mem4 <= 0;
        md_busy <= 0; md_mt <= 0; md_cnt <= 0; md_dm <= 0; md_c <= 0; md_w <= 0; md_m32 <= 0; md_m32b <= 0;
        wb_rp <= 0; wb_wp <= 0; wb_cnt <= 0; port <= P_IDLE; port_cnt <= 0;
        ms <= MS_IDLE; ms_cnt <= 0; sb_valid <= 0; tk_valid <= 0; sw_wait <= 0;
        sc <= SC_IDLE; sc_k <= 0; utlb_f.valid <= 0; utlb_m.valid <= 0; m_wq <= 0; f_wq <= 0;
        d_fmiss <= 0; rd_res <= 0; rd_fix <= 0; rp_pend <= 0; m_w1 <= 0;
    end else begin
        // ---------------------------------------------------- Count / Compare
        begin
            logic step;
            step = SIM_COUNT_RETIRE ? (m_commit || m_take_exc) : 1'b1;
            if (step) begin
                c0_random <= rnd_dec;
                count_frac <= !count_frac;
                if (count_frac) begin
                    c0_count <= c0_count + 32'd1;
                    if (c0_count + 32'd1 == c0_compare) c0_cause[15] <= 1'b1;
                end
            end
        end
        c0_cause[10] <= irq_rcp;

        // ---------------------------------------------------- M -> W
        w_valid <= FAST ? m_commit_e : m_commit;
        w_lm <= m_lmiss;
        if (m_commit_e) begin
            w_c <= m_c; w_pc <= m_pc; w_insn <= m_insn; w_serial <= m_serial;
            if (m_c.mfc0) w_res <= c0_rd;
            else if (m_c.cop1_gpr) w_res <= fpu_gpr_out[31:0];
            else if (m_c.st == S_SC) w_res <= {31'd0, !m_sc_fail};
            else if (m_ld) w_res <= m_ld_val;
            else if (XALU && m_xcmp) w_res <= {31'd0, m_xlt};
            else if (XALU && m_xshift) w_res <= sh_r[31:0];
            else w_res <= m_res;
            w_hx <= 32'd0;
            if (W64) begin
                if (m_ld) w_hx <= m_ld_hx;
                else if (m_c.cop1_64) w_hx <= fpu_gpr_out_hi ^ {32{fpu_gpr_out[31]}};   // dmfc1
                else if (XALU && m_xshift) w_hx <= hx32(sh_r);
                else if (XALU && m_c.x64 != X_NONE && !m_xcmp) w_hx <= m_xhx;
            end
        end

        // ---------------------------------------------------- M: exceptions
        if (m_take_int || m_take_exc) begin
            if (m_to_mon) begin
                // the monitor level (MON): ERL and ErrorEPC; what the game's handler may be in the middle of is not touched
                c0_errorepc <= m_dly ? m_pc - 32'd4 : m_pc;
                c0_status[2] <= 1'b1;
                mon_bd <= m_dly; mon_code <= m_take_int ? 5'd31 : m_code;
            end else begin
                if (!c0_status[1]) begin
                    c0_epc <= m_dly ? m_pc - 32'd4 : m_pc;
                    c0_cause[31] <= m_dly;
                end
                c0_cause[6:2] <= m_take_int ? 5'd0 : m_code;
                c0_cause[29:28] <= (!m_take_int && m_exc_v && m_code == 5'd11) ? 2'd1 : 2'd0;   // CE: COP1
                c0_status[1] <= 1'b1;
            end
            if (m_take_exc) begin
                // (TLB_RAM: a fetch's too -- its page's entry is the runtime's to fill; a device access of the
                // game's, in the monitor, leaves BadVAddr alone: the runtime works the address out)
                if (m_exc_v && (m_code == 5'd4 || m_code == 5'd5 || (TLB_RAM && m_code == 5'd2)) && !(m_to_mon && m_dev) && !m_tr_mon) c0_badvaddr <= m_badv;
                if (TLB_RAM && m_code >= 5'd1 && m_code <= 5'd3) tr_lastf <= m_exc_v;
                if (TLB_RAM && m_exc_v && m_code == 5'd2 && !m_tr_mon) c0_entryhi <= (m_badv & 32'hFFFFE000) | (c0_entryhi & 32'hFF);
                // (the monitor level's: the game's BadVAddr and EntryHi stay as they are)
                if (m_tr_mon) mon_badv <= m_exc_v ? m_badv : m_addr;
                if (!m_exc_v && !m_tr_mon) begin                    // a TLB fault
                    c0_badvaddr <= m_addr;
                    if (!TLB_RAM) c0_context <= (c0_context & 32'hFF800000) | ((m_addr >> 13) << 4);
                    c0_entryhi <= (m_addr & 32'hFFFFE000) | (c0_entryhi & 32'hFF);
                end
            end
        end

        // ---------------------------------------------------- M: completing
        sb_valid <= 0;
        tk_valid <= dct_we && !dct_wdata[16]; tk_idx <= dct_waddr;
        if (m_commit_e) begin
            if (m_st && !m_sc_fail && m_cached && m_dc_hit) begin
                sb_valid <= 1; sb_waddr <= m_cst_idx; sb_data <= m_cst_data; sb_strb <= m_cst_strb;
            end
            if (m_c.ld == M_LL && !m_lmiss) begin ll_bit <= 1'b1; c0_lladdr <= m_pa >> 4; end
            if (m_c.st == S_SC) ll_bit <= 1'b0;
            if (m_c.cop1 && m_c.cop1_cond) fcc <= fpu_cond;
            // mthi, mtlo: written in the next cycle, from md_ma (rs's lower word, which the multiplier's
            // operand register takes every cycle) and md_ah -- md_busy holds back whoever reads them
            if (m_c.md == MD_MTHI || m_c.md == MD_MTLO) begin md_mt <= 1'b1; md_mth <= m_c.md == MD_MTHI; md_busy <= 1'b1; end
            if (m_is_md && m_c.md != MD_MTHI && m_c.md != MD_MTLO) hilo_soft <= 1'b0;   // (a multiply or a divide: HI and LO are the core's again)
            if (m_c.mtc0) begin
                case (m_insn[15:11])
                5'd1, 5'd15: ;                                      // Random, PRId: read-only
                5'd9:  c0_count <= m_st_lo;
                5'd11: begin c0_compare <= m_st_lo; c0_cause[15] <= 1'b0; end
                5'd13: begin c0_cause[9:8] <= m_st_lo[9:8];
                             // (the monitor level delivers a TLB exception to the game: its BD and ExcCode are its to set)
                             if (MON && c0_status[2]) begin c0_cause[31] <= m_st_lo[31]; c0_cause[6:2] <= m_st_lo[6:2]; end end
                // the VR4300's writable bits (the constant rest is not stored; BadVAddr,
                // XContext, TagHi and the reserved registers take no write)
                // (BadVAddr: the monitor level's, for the TLB exception it delivers to the game)
                5'd8:  if (TLB_RAM && MON && c0_status[2]) c0_badvaddr <= m_st_lo;
                5'd0:  begin c0_index[5:0] <= m_st_lo[5:0];
                             // (the monitor does the game's tlbp: its P bit, which no mtc0 of the game's reaches)
                             if (TLB_RAM && MON && c0_status[2]) c0_index[31] <= m_st_lo[31]; end
                5'd2:  c0_entrylo0 <= m_st_lo & 32'h3FFFFFFF; 5'd3: c0_entrylo1 <= m_st_lo & 32'h3FFFFFFF;
                5'd4:  begin c0_context[31:23] <= m_st_lo[31:23];
                             // (likewise its BadVPN2: the tables' faults do not set it)
                             if (MON && c0_status[2]) c0_context[22:4] <= m_st_lo[22:4]; end
                5'd5:  c0_pagemask <= m_st_lo & 32'h01FFE000;
                5'd6:  begin c0_wired <= m_st_lo & 32'h3F; c0_random <= 5'd31; end
                5'd7:  begin hilo_soft <= W64 && !XMD && m_st_lo[0]; if (MON) mon_game <= m_st_lo[1]; if (MON && MI && m_st_lo[9]) mon_mi <= m_st_lo[8];
                             if (MON && m_st_lo[11]) mon_mem4 <= m_st_lo[10]; end      // (bit 11: the write is of bit 10)
                5'd10: c0_entryhi <= m_st_lo & 32'hFFFFE0FF;  5'd12: c0_status <= m_st_lo;
                5'd14: c0_epc <= m_st_lo;
                5'd16: c0_config <= (c0_config & ~32'h0F00800F) | (m_st_lo & 32'h0F00800F);
                5'd17: c0_lladdr <= m_st_lo;
                5'd18: c0_watchlo <= m_st_lo & 32'hFFFFFFFB;  5'd19: c0_watchhi <= m_st_lo & 32'hF;
                5'd28: c0_taglo <= m_st_lo & 32'h0FFFFFC0;    5'd30: c0_errorepc <= m_st_lo;
                default: ;
                endcase
            end
            if (m_c.eret) begin
                if (c0_status[2]) c0_status[2] <= 1'b0; else c0_status[1] <= 1'b0;
                ll_bit <= 1'b0;
            end
            if (m_c.tlbwi || m_c.tlbwr) begin utlb_f.valid <= 0; utlb_m.valid <= 0; end   // (the RAM: tlb_we)
            if (m_c.tlbr) begin
                c0_pagemask <= {7'd0, sc_e.mask, 13'd0};
                c0_entryhi  <= {sc_e.vpn, 5'd0, sc_e.asid};
                c0_entrylo0 <= {sc_e.lo0, sc_e.g};
                c0_entrylo1 <= {sc_e.lo1, sc_e.g};
            end
            if (m_c.tlbp) c0_index <= sc_hit ? {27'd0, sc_idx} : 32'h80000000;
        end

        // ---------------------------------------------------- the write buffer
        wb_age <= wb_empty ? 6'd0 : wb_age[5] ? wb_age : wb_age + 6'd1;
        if (m_wb_push && !XWB) begin
            wb_addr[wb_wp[2:0]] <= m_cst_pa; wb_data[wb_wp[2:0]] <= m_cst_data; wb_strb[wb_wp[2:0]] <= m_cst_strb;
            wb_wp <= wb_wp + 4'd1;
        end
        wb_cnt <= wb_cnt + 4'(m_wb_push) - 4'(port == P_WB && mem_ack);
        // a cached 64-bit access's first word is done: a load's is kept
        if (flush || m_adv) m_ph <= 1'b0;
        else if (m_w0 && !m_lmiss) m_ph <= 1'b1;
        if (m_w0 && m_ld) begin if (BIG) ms_data[63:32] <= dc_qm; else ms_data[31:0] <= dc_qm; end
        // a d-shift: the value in, then a byte or a bit a cycle
        if (flush || m_adv) begin sh_on <= 1'b0; end
        else if (m_live && m_xshift) begin
            logic fill;
            fill = m_c.x64 == X_SRA && sh_r[63];
            if (!sh_on) begin sh_r <= m_b64; sh_n <= m_xsh; sh_on <= 1'b1; m_shwait <= m_xsh != 6'd0; end
            else if (sh_n >= 6'd8) begin
                m_shwait <= sh_n != 6'd8;
                sh_r <= m_c.x64 == X_SLL ? {sh_r[55:0], 8'd0} : {{8{fill}}, sh_r[63:8]};
                sh_n <= sh_n - 6'd8;
            end else if (sh_n != 6'd0) begin
                m_shwait <= sh_n != 6'd1;
                sh_r <= m_c.x64 == X_SLL ? {sh_r[62:0], 1'b0} : {fill, sh_r[63:1]};
                sh_n <= sh_n - 6'd1;
            end
        end

        // ---------------------------------------------------- E -> M
        if (flush) m_valid <= 0;
        else if (m_adv) begin
            m_valid <= e_valid && !e_kill;
            m_xshift <= XSH && (e_c.x64 == X_SLL || e_c.x64 == X_SRL || e_c.x64 == X_SRA);
            m_shwait <= XSH && (e_c.x64 == X_SLL || e_c.x64 == X_SRL || e_c.x64 == X_SRA);
            m_xc <= e_cs[32]; m_xltu <= e_ltu;
            m_c <= e_c; m_pc <= e_pc; m_insn <= e_insn; m_dly <= e_dly; m_serial <= e_serial;
            m_res <= e_res;
            m_addr <= e_ea;
            if (FAST) begin
                m_f_kseg <= kseg01(e_ea); m_f_devk <= pa_dev(kphys(e_ea));
                m_k_ld <= e_c.ld != M_NONE; m_k_st <= e_c.st != S_NONE; m_k_sc <= e_c.st == S_SC;
                m_k_fwide <= e_fwide; m_k_wide <= e_fwide || (W64 && (e_c.ld == M_LD || e_c.st == S_SD));
                m_k_cach <= cacheable(e_ea); m_k_mapped <= e_mem && !kseg01(e_ea);
                m_k_md <= e_c.md != MD_NONE; m_k_fpu <= e_c.cop1 || e_c.ld == M_FL || e_c.st == S_FS;
                m_k_sbeq <= m_cst_idx == e_ea[DW+1:2];            // (it is behind the store that is at M now)
                m_k_sstrb <= e_sstrb; m_k_sfs <= e_c.st == S_FS; m_k_ssd <= e_c.st == S_SD;
            end
            m_st_lo <= FAST ? e_sdata : e_b;        // (FAST: a store's word in its lanes already)
            m_a_lo <= e_a;
            m_a_hx <= e_ra_hx; m_st_hx <= e_rb_hx;
            m_exc <= e_exc; m_exc_code <= e_exc_code; m_badv <= e_badv;
            m_dev <= N64WIN && e_n64dev; m_hwx <= e_hwx;
        end
        // FAST: the load of the wrong line, and what came in behind it, go;
        // the line is read (ms, below) and the fetch made again from the load
        rp_pend <= 1'b0;
        if (FAST && m_lmiss) begin m_valid <= 1'b0; rp_pend <= 1'b1; rp_m <= !m_adv; end
        if (FAST && ms == MS_IDLE) d_fill_pa <= m_pa;
        if (flush || m_adv || CK_W == 0) ck_n <= '0;
        else if (m_cache_go && ck_run) ck_n <= ck_n + 1'b1;
        // (a delay slot's load: its tag, a cycle after its word)
        if (flush || m_adv || ms != MS_IDLE) m_w1 <= 1'b0;
        else if (FAST && m_live && m_ld && m_cached && m_dly) begin m_w1 <= 1'b1; m_hit_r <= m_dc_hit; end

        // ---------------------------------------------------- D -> E
        if (flush) e_valid <= 0;
        else if (e_adv) begin
            e_valid <= d_ok && !d_stall;
            e_c <= d_cx; e_pc <= d_pc; e_insn <= d_insn; e_dly <= d_dly;
            e_al2 <= d_al2; e_al4 <= d_al4; e_al8 <= d_al8; e_mem <= d_mem; e_fwide <= d_fwide; e_isfp <= d_isfp; e_n64c <= n64code(d_pc);
            e_rs <= d_rs; e_rt <= d_rt;
            e_ra <= rf_a; e_rb <= rf_b;
            if (FAST) begin
                if (d_c.b_imm) e_rb <= d_c.imm_zero ? {16'd0, d_insn[15:0]} : sx16(d_insn[15:0]);
                e_fa_m <= e_wr && !e_c.late && e_c.rd == d_rs && d_rs != 0;
                e_fa_w <= m_wr && m_c.rd == d_rs && d_rs != 0;
                e_fb_m <= !d_c.b_imm && e_wr && !e_c.late && e_c.rd == d_rt && d_rt != 0;
                e_fb_w <= !d_c.b_imm && m_wr && m_c.rd == d_rt && d_rt != 0;
            end
            e_ra_hx <= rf_ah; e_rb_hx <= rf_bh;
            e_aw <= rf_aw;
            e_hxeq <= (rf_aw && rf_bw) ? rfq_ah == rfq_bh : !rf_aw && !rf_bw;     // (a masked hx is zero)
            e_fexc <= d_fexc; e_fexc_code <= d_fexc_code;
            e_serial <= d_serial;
            e_bp <= d_k_pred && f_deliver;
            e_ptarget <= d_ptarget;
            e_k_grp <= 2'd3; e_k_op <= 3'd1; e_k_f <= 1'b0;                 // (nothing)
            e_k_sub <= d_cx.alu != A_ADD;                                   // (the adder subtracts: sub, and for the compares)
            case (d_cx.alu)
            A_ADD:  e_k_grp <= 2'd0;
            A_SUB:  begin e_k_grp <= 2'd0; e_k_f <= 1'b1; end
            A_SLL, A_SRL, A_SRA: e_k_grp <= 2'd1;
            A_AND:  begin e_k_grp <= 2'd2; e_k_op <= 3'd0; end
            A_OR:   begin e_k_grp <= 2'd2; e_k_op <= 3'd1; end
            A_XOR:  begin e_k_grp <= 2'd2; e_k_op <= 3'd2; end
            A_NOR:  begin e_k_grp <= 2'd2; e_k_op <= 3'd3; end
            A_LUI:  begin e_k_grp <= 2'd2; e_k_op <= 3'd4; end
            A_LINK: begin e_k_grp <= 2'd2; e_k_op <= 3'd5; end
            A_MFHI: begin e_k_grp <= 2'd2; e_k_op <= 3'd6; end
            A_MFLO: begin e_k_grp <= 2'd2; e_k_op <= 3'd7; end
            A_SLT:  e_k_op <= 3'd0;
            A_SLTU: begin e_k_op <= 3'd0; e_k_f <= 1'b1; end
            default: ;
            endcase
        end else begin
            if (e_kill) e_valid <= 1'b0;
            // (its delay slot is to be fetched again: the fetch will not have followed it)
            if (d_imiss && d_dly) e_bp <= 1'b0;
            // E held behind a stalled M: W's result is only bypassed for one
            // cycle, so an operand it supplies is kept here
            if (e_w_a) e_ra <= w_res;
            if (e_w_b) e_rb <= w_res;
            if (FAST) begin e_fa_w <= 1'b0; e_fb_w <= 1'b0; end
        end

        // ---------------------------------------------------- F -> D
        // FAST: E's branch, decided as it leaves
        rd_res <= 1'b0; rd_fix <= 1'b0;
        if (FAST && !flush && e_adv && e_valid && e_cti) begin
            rd_res <= 1'b1; rd_fix <= e_taken != e_bp;
            rd_annul <= e_c.likely && !e_taken; rd_target <= e_taken ? e_target : e_pc + 32'd8;
        end
        if (flush) begin
            d_valid <= 0; ds_next <= 0; rd_pend <= 0;
        end else begin
            if (d_adv || d_kill) d_valid <= 0;
            if (FAST) begin
                if (rd_res && ds_next) rd_pend <= 1;                // (its delay slot is still to come)
            end else if (cti_now && !f_deliver) begin
                rd_pend <= 1; rd_taken <= d_taken; rd_annul <= d_c.likely && !d_taken; rd_target <= d_target;
            end
            if (f_deliver) begin
                d_valid <= !(ds_now && rd_valid && rd_a);           // an annulled delay slot never enters
                d_pc <= f_pc; d_insn <= f_fault ? 32'd0 : f_insn;
                d_dly <= ds_now;
                d_fexc <= f_fault; d_fexc_code <= f_fault_code;
                d_fmiss <= FAST && fs == FS_RUN && !f_misal && !f_hit;
                d_fmap <= f_xmap; d_fphit <= f_mhit0 || f_mhit1; d_fpfn <= f_mhit0 ? pg[0].pfn : pg[1].pfn;
                if (FAST) begin
                    ctl_t fc;
                    fc = decode(f_fault ? 32'd0 : f_insn);
                    d_c_r <= fc;
                    d_k_cti <= fc.br != B_NONE;
                    d_k_pred <= guess(fc, f_insn);
                    // (a delay slot: dropped if its branch is known to annul it; one
                    // that enters before that is known is dropped where it is by then)
                    d_valid <= !(ds_next && (rd_res || rd_pend) && rd_annul);
                end
                ds_next <= 0;
                rd_pend <= 0;
            end else if (cti_now) ds_next <= 1;
            // FAST: D's word was another line's. Its line is read (below) and it
            // is fetched again -- as the delay slot it was: with its branch's
            // outcome kept (rd_pend), unless the branch is still at E
            if (d_imiss) begin
                d_valid <= 0; ds_next <= d_dly;
                rd_pend <= d_dly && !(e_valid && e_cti);
            end
        end

        // ---------------------------------------------------- F
        if (!FAST) f_pc <= f_next;
        else if (ic_ren) f_pc <= f_nx;
        f_ok <= fs == FS_RUN || fs == FS_READY;
        case (fs)
        FS_RUN: if (f_ok && !f_have && !f_kill) begin
            f_cnt <= 0; f_discard <= 0; f_xfault <= 0;
            if (!f_kseg) begin
                if (TLB_EN && utlb_hit(utlb_f, f_pc, c0_entryhi[7:0])) begin fs <= FS_XLAT; f_tlbq <= utlb_res(utlb_f.x, f_pc); end
                else begin fs <= FS_WALK; f_wq <= 0; end
            end
            else begin f_fill_pa <= kphys(f_pc); f_fill_col <= f_pc[IW+1:12]; fs <= f_cached ? FS_REFILL : FS_SLOW; end
        end
        FS_WALK: begin                                      // a search; a flush waits for its end
            if (f_kill) f_discard <= 1;
            if (sc_start && !sc_req_m) f_wq <= 1;
            if (sc_done_f) begin
                if (f_discard || f_kill) fs <= FS_RUN;
                else begin
                    fs <= FS_XLAT;
                    f_tlbq <= sc_hit ? utlb_res(sc_x, f_pc) : 35'd0;
                    f_tc <= sc_x.c;
                    if (sc_hit) begin utlb_f.valid <= 1; utlb_f.x <= sc_x; end
                end
            end else if (!f_wq && !sc_start && (f_discard || f_kill)) fs <= FS_RUN;   // not started: just go
        end
        FS_XLAT:
            if (f_kill) fs <= FS_RUN;
            else if (!f_tlbq[34] || !f_tlbq[33]) begin fs <= FS_READY; f_xfault <= 1; f_ok <= 1; end
            else if (PG && f_tc) fs <= FS_RUN;              // (a page register has it now: the I-cache's from here)
            else begin f_fill_pa <= tphys({f_tlbq[31:12], f_pc[11:0]}); fs <= FS_SLOW; end
        FS_REFILL: if (port == P_ILINE && mem_rvalid) begin
            f_cnt <= f_cnt + 3'd1;
            if (f_cnt == 3'd7) begin fs <= FS_RUN; f_ok <= 0; end
        end
        FS_SLOW: begin
            if (f_kill) f_discard <= 1;
            if (port == P_SLOW && !slow_is_d && mem_rvalid) begin
                f_slow_insn <= mem_rdata;
                fs <= (f_discard || f_kill) ? FS_RUN : FS_READY;
                f_ok <= !(f_discard || f_kill);
            end
        end
        FS_READY: if (f_deliver || f_kill) fs <= FS_RUN;
        default: fs <= FS_RUN;
        endcase
        if (fs == FS_REFILL || (fs == FS_SLOW && !(port == P_SLOW && !slow_is_d && mem_rvalid))) f_ok <= 0;
        if (d_imiss && !flush) begin
            fs <= FS_REFILL; f_fill_pa <= kphys(d_pc); f_fill_col <= d_pc[IW+1:12]; f_cnt <= 0; f_discard <= 0; f_xfault <= 0;
            if (PG && d_fmap) begin
                if (d_fphit) f_fill_pa <= tphys({9'd0, d_fpfn, d_pc[11:0]});
                else begin fs <= FS_WALK; f_wq <= 0; end
            end
        end
        // (FAST: a line dropped from the I-cache: what F holds of it is read again)
        if (FAST && ict_we && !ict_wdata[ITW]) f_ok <= 0;

        // ---------------------------------------------------- M's slow path / refill
        case (ms)
        MS_IDLE: if (m_live && (m_c.tlbp || m_c.tlbr)) begin ms <= MS_WALK; m_wq <= 0; end
        else if (m_live && m_mem) begin
            if (m_mapped) begin
                if (TLB_EN && utlb_hit(utlb_m, m_addr, c0_entryhi[7:0])) begin ms <= MS_XLAT; m_tlbq <= utlb_res(utlb_m.x, m_addr); end
                else begin ms <= MS_WALK; m_wq <= 0; end
            end
            else if (m_ld && m_cached && !m_dc_hit && !m_ph) begin ms <= MS_REFILL; ms_cnt <= 0; end
            else if (m_needs_slow) ms <= MS_SLOW_A;
        end
        MS_WALK: begin                               // a search (a mapped access, tlbp) or tlbr's read
            if (sc_start && sc_req_m) m_wq <= 1;
            if (sc_done_m) begin
                if (m_c.tlbp || m_c.tlbr) ms <= MS_DONE;
                else begin
                    ms <= MS_XLAT;
                    m_tlbq <= sc_hit ? utlb_res(sc_x, m_addr) : 35'd0;
                    if (sc_hit) begin utlb_m.valid <= 1; utlb_m.x <= sc_x; end
                end
            end
        end
        MS_XLAT:                                     // a fault, a store done, or on to the slow path
            if (!m_live || m_commit_e) ms <= MS_IDLE;
            else if (m_needs_slow) ms <= MS_SLOW_A;
        MS_REFILL: if (port == P_DLINE && mem_rvalid) begin
            ms_cnt <= ms_cnt + 3'd1;
            if (ms_cnt == 3'd7) ms <= MS_REREAD;
        end
        MS_REREAD: ms <= MS_IDLE;
        MS_SLOW_A: if (m_sw) begin                   // (XWB) a store: out by st_*
            if (m_sw_push) begin
                if (m_devst) sw_wait <= 1'b1;
                else ms <= m_wide ? MS_SLOW_B : MS_DONE;
            end
            if (sw_wait && st_done) begin sw_wait <= 1'b0; ms <= MS_DONE; end
        end
        else if (port == P_SLOW && slow_d_own && port_done) begin
            if (m_wide) begin if (BIG) ms_data[63:32] <= mem_rdata; else ms_data[31:0] <= mem_rdata; ms <= MS_SLOW_B; end
            else begin ms_data[31:0] <= mem_rdata; ms <= MS_DONE; end
        end
        MS_SLOW_B: if (m_sw) begin if (m_sw_push) ms <= MS_DONE; end
        else if (port == P_SLOW && slow_d_own && port_done) begin
            if (BIG) ms_data[31:0] <= mem_rdata; else ms_data[63:32] <= mem_rdata; ms <= MS_DONE;
        end
        MS_DONE: if (m_commit_e) ms <= MS_IDLE;
        default: ms <= MS_IDLE;
        endcase

        // ---------------------------------------------------- the TLB scanner
        case (sc)
        SC_IDLE: if (sc_start) begin
            sc <= SC_RUN; sc_k <= tlb_raddr; sc_for_m <= sc_req_m;
            sc_mode <= !sc_req_m ? SCM_LOOKUP : m_c.tlbp ? SCM_PROBE : m_c.tlbr ? SCM_READ : SCM_LOOKUP;
            sc_va <= !sc_req_m ? f_pc : m_c.tlbp ? c0_entryhi : m_addr;
            sc_asid <= c0_entryhi[7:0];
        end
        SC_RUN: begin                                 // tlb_q holds entry sc_k
            sc_e <= TLB_EN ? tlb_q : TLBE_RESET;
            sc_idx <= sc_k; sc_x <= tlb_xlat(tlb_q, sc_va);
            if (sc_mode == SCM_READ) sc <= SC_DONE;
            else if (sc_match) begin sc_hit <= 1; sc <= SC_DONE; end
            else if (!TLB_EN || sc_k == 5'(TLB_N - 1)) begin sc_hit <= 0; sc <= SC_DONE; end
            else sc_k <= sc_k + 5'd1;
            if (TLB_RAM) begin                            // the table's entry: it translates, or nothing does
                sc_x <= '{v: 1'b1, d: tr_q.d, c: tr_q.c, vpage: 20'd0, om: 13'd0, pfn: {3'd0, tr_q.pfn}, g: tr_q.g, asid: tr_q.asid};
                if (sc_mode == SCM_LOOKUP) begin sc_hit <= tr_hit; sc <= SC_DONE; end
            end
        end
        default: sc <= SC_IDLE;                       // SC_DONE: the result, for one cycle
        endcase

        // ---------------------------------------------------- the memory port
        case (port)
        P_IDLE: begin
            port_cnt <= 0;
            if (!wb_empty) begin
                if (wb_go || ms == MS_REFILL || ms == MS_SLOW_A || ms == MS_SLOW_B || fs == FS_REFILL || fs == FS_SLOW) port <= P_WB;
            end
            else if (ms == MS_REFILL) port <= P_DLINE;
            else if ((ms == MS_SLOW_A || ms == MS_SLOW_B) && !(XWB && m_st)) begin port <= P_SLOW; slow_d_own <= 1; end
            else if (fs == FS_REFILL) port <= P_ILINE;
            else if (fs == FS_SLOW) begin port <= P_SLOW; slow_d_own <= 0; end
        end
        P_WB: if (mem_ack) begin wb_rp <= wb_rp + 4'd1; if (wb_rp + 4'd1 == wb_wp) port <= P_IDLE; end
        P_ILINE, P_DLINE: if (mem_rvalid) begin
            port_cnt <= port_cnt + 3'd1;
            if (port_cnt == 3'd7) port <= P_IDLE;
        end
        P_SLOW: if (port_done) port <= P_IDLE;
        default: port <= P_IDLE;
        endcase

        // ---------------------------------------------------- multiply / divide (started as M completes)
        // mult(u): the DSP's product, at once (FAST: two cycles on). dmult(u): the DSP's
        // too, four products of the operands' halves and their sum (see md_ma). The rest a
        // bit a cycle on md_acc and one adder: div(u) and ddiv(u) restoring, on the magnitudes
        // (a 32-bit dividend starts 32 bits up: 32 steps).
        if (XMD && !md_dm) md_ac <= '0;                           // (the sum's start, with no wait for M)
        if (m_commit_e && m_is_md && m_c.md != MD_MTHI && m_c.md != MD_MTLO) begin
            logic sgn, w, sa, sb;
            logic [MW-1:0] a, b, ma;
            w = XDIV && (m_c.md == MD_DDIV || m_c.md == MD_DDIVU);
            sgn = m_c.md == MD_DIV || m_c.md == MD_MULT || m_c.md == MD_DDIV || m_c.md == MD_DMULT;
            a = w ? MW'(m_a64) : MW'({{32{sgn && m_a_lo[31]}}, m_a_lo});
            b = w ? MW'(m_b64) : MW'({{32{sgn && m_st_lo[31]}}, m_st_lo});
            sa = sgn && a[MW-1]; sb = sgn && b[MW-1];
            ma = sa ? -a : a;
            md_w <= w; md_dd <= !XDIV && m_c.md == MD_DDIV;
            if (FAST && (m_c.md == MD_MULT || m_c.md == MD_MULTU)) begin
                md_sa <= m_c.md == MD_MULT; md_sb <= m_c.md == MD_MULT; md_m32 <= 1'b1; md_busy <= 1'b1;
            end else if (m_c.md == MD_MULT || m_c.md == MD_MULTU) begin
                logic [63:0] p;
                p = m_c.md == MD_MULT ? 64'($signed(m_a_lo) * $signed(m_st_lo)) : 64'(m_a_lo) * 64'(m_st_lo);
                lo_r <= p[31:0]; hi_r <= p[63:32]; lo_hx <= 0; hi_hx <= 0; lo_wide <= 0; hi_wide <= 0;
            end else if (XMD && (m_c.md == MD_DMULT || m_c.md == MD_DMULTU)) begin
                // (aL and bL are in md_ma and md_mb: pass 0)
                md_sa <= 1'b0; md_sb <= 1'b0;
                md_dm <= 1'b1; md_c <= 3'd1; md_busy <= 1'b1;
            end else begin                                         // (by zero too: the quotient of all ones, the remainder the dividend, as the VR4300 leaves)
                md_acc <= {MW'(0), (MD64 && !w) ? MW'({ma[31:0], 32'd0}) : ma};
                md_dv <= sb ? -b : b;
                md_nq <= sa ^ sb; md_nr <= sa;
                md_cnt <= w ? 7'd64 : 7'd32; md_busy <= 1'b1;
            end
        end else if (md_busy && md_mt) begin
            logic [31:0] hx;
            hx = md_ah ^ {32{md_ma[31]}};
            md_mt <= 1'b0; md_busy <= 1'b0;
            if (md_mth) begin hi_r <= md_ma; if (XMD) begin hi_hx <= hx; hi_wide <= hx != 32'd0; end end
            else begin lo_r <= md_ma; if (XMD) begin lo_hx <= hx; lo_wide <= hx != 32'd0; end end
        end else if (md_busy && md_m32) begin
            md_m32 <= 1'b0; md_m32b <= 1'b1;
        end else if (md_busy && md_m32b) begin
            lo_r <= md_p[31:0]; hi_r <= md_p[63:32]; lo_hx <= 0; hi_hx <= 0; lo_wide <= 0; hi_wide <= 0;
            md_busy <= 1'b0; md_m32b <= 1'b0;
        end else if (XMD && md_busy && md_dm) begin
            // dmult(u): the products' sum, a word at a time: the low word of the sum of
            // pass 0 is LO's lower, that of passes 1 and 2 (with what passed 0 carried) its upper, ...
            logic [66:0] t;
            md_c <= md_c + 3'd1;
            t = md_ac + {{2{md_psg2 & md_p2[63]}}, md_psg2 & md_p2[63], md_p2};
            // (the passes' signs, as each is selected)
            case (md_c)
            3'd1: begin md_sa <= 1'b0; md_sb <= md_sgn; end
            3'd2: begin md_sa <= md_sgn; md_sb <= 1'b0; end
            3'd3: begin md_sa <= md_sgn; md_sb <= md_sgn; end
            default: ;
            endcase
            // (the sum: pass k's product is in md_p2 at cycle 3 + k)
            case (md_c)
            3'd3: begin lo_r <= t[31:0]; md_ac <= {{32{t[66]}}, t[66:32]}; end
            3'd4: md_ac <= t;
            3'd5: begin lo_hx <= t[31:0] ^ {32{lo_r[31]}}; md_ac <= {{32{t[66]}}, t[66:32]}; end
            3'd6: begin hi_r <= t[31:0]; hi_hx <= t[63:32] ^ {32{t[31]}}; end
            3'd7: begin                                                  // (the flags: which words are 32-bit values)
                lo_wide <= lo_hx != 32'd0; hi_wide <= hi_hx != 32'd0;
                md_busy <= 1'b0; md_dm <= 1'b0;
            end
            default: ;
            endcase
        end else if (md_busy) begin
            if (md_cnt != 0) begin
                logic [MW:0] x, y, t;
                x = {1'b0, md_acc[2*MW-2:MW-1]};
                y = {1'b0, md_dv};
                t = x - y;
                md_acc <= t[MW] ? {x[MW-1:0], md_acc[MW-2:0], 1'b0} : {t[MW-1:0], md_acc[MW-2:0], 1'b1};
                md_cnt <= md_cnt - 7'd1;
            end else begin
                logic [MW-1:0] q, r;
                md_busy <= 1'b0;
                q = md_nq ? -md_acc[MW-1:0] : md_acc[MW-1:0];
                r = md_nr ? -md_acc[2*MW-1:MW] : md_acc[2*MW-1:MW];
                lo_r <= q[31:0]; hi_r <= r[31:0];
                lo_hx <= md_w ? hx32(64'(q)) : 32'd0; hi_hx <= md_w ? hx32(64'(r)) : 32'd0;
                lo_wide <= md_w; hi_wide <= md_w;
                // (ddiv of two words: only -2^31 / -1 leaves the 32 bits, a positive 2^31 -- its upper half 0)
                if (!md_w && md_dd && !md_nq && q[31]) begin lo_hx <= 32'hFFFFFFFF; lo_wide <= 1'b1; end
            end
        end
    end
end

endmodule
`default_nettype wire
