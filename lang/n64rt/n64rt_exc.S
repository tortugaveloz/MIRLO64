# n64rt's exceptions, thread switch and the patched functions' way in.
#
# exception_entry: the crt0 vector (lang/mips/linker/init_asm.S) jumps here
# untouched. The whole state of what was running -- the current thread, in
# the game's code or in the runtime's on its behalf -- goes to that thread's
# OSThread (its libultra context area, laid out as n64rt_ctx below); C
# (n64rt_exception) handles the cause on a stack of its own and may pick
# another thread (n64rt_cur); that thread's state comes back and eret
# resumes it. A thread blocks or yields with a syscall (n64rt_switch), so
# every switch takes this one path.
#
# n64rt_ctx, at OSThread + 0x20 (n64rt_os.c):
#   0x00 epc   0x04 status   8n gpr n (64 bits, upper word first; n = 1..25,
#   28..31)   0xD0 hi   0xD8 lo   0x100 fcsr   0x104 fpr[32]
# With n64rt_cpu64 the registers, HI and LO are saved and restored whole (sd,
# ld); on the CPU without 64-bit integers their lower words only.
# 0x18C soft: hi and lo are a 64-bit result of the runtime's making, the
# CPU's own being 32 bits (its COP0 7: mfhi and mflo come to the runtime).
    .set    noreorder
    .set    noat
    .set    mips3

#define CTX 0x20
    .text
#ifdef N64RT_OWN
# ---- the monitor's entry (N64RT_OWN: the game runs its own libultra) ----------------
# The CPU (mips_core's MON) comes here, the runtime's vector, only for the monitor
# level: ERL set, ErrorEPC the place to return to, neither EXL nor EPC nor Cause
# the game's own touched -- the game's handler may be in the middle of its work, with
# its $k0 and $k1 live. Those two go to the monitor's RAM (0xFFFF8000: reachable with
# $zero as the base) first; the rest of the game's state to n64rt_mon_ctx, as a thread's
# is; Status gets CU1 for the FPU's registers (the game's thread may not have it yet).
# n64rt_mon_exception (n64rt_os.c) does the work; what it left in the context -- the
# registers a load wrote, ErrorEPC past the instruction done for the game -- comes back.
# n64rt_mon_ctx is 4 KiB aligned (n64rt_os.c): with $k0 = %hi(n64rt_mon_ctx) << 16 every field of it is one
# instruction away, and a clobbered $k0 is a lui from being the context's base again.
#define CTXL(off) %lo(n64rt_mon_ctx+(off))($k0)
    .globl  exception_entry
exception_entry:
    # $k0 and $k1 can be 64 bits wide: a game that loads a 64-bit HI or LO into $k1 and does mtlo has the upper half in it
    # (__osDispatchThread's ld $k1 / mtlo $k1). A 64-bit store to the monitor's RAM (sd $k1, -0x7FF8($zero)) does not work
    # for the CPU on its own clock: its store FIFO (cpu_port) takes mips_core's wide uncached store for an SDRAM store
    # (m_devst is for narrow ones), so the monitor RAM never sees it and the SDRAM word at its address is overwritten --
    # the game's $k0 came back 0 and the first thread dispatch faulted. So: $k0's low word goes to the monitor RAM (a word
    # store is a device's), $k0 becomes the context's address, and $k1 whole goes there with an sd to cached memory.
    # $k0's upper half is then the sign extension of its low word (a pointer, Status, Cause: always).
    sw      $k0, -0x7FFC($zero)
    lui     $k0, %hi(n64rt_mon_ctx)
    sd      $k1, CTXL(8*27)
    mfc0    $k1, $12
    sw      $k1, CTXL(0x04)         # the game's Status
    andi    $k1, $k1, 4             # not ERL: nothing the monitor level was entered for
    beqz    $k1, n64rt_mon_stray
    mfc0    $k1, $9                 # Count as the game last had it: the monitor's time is not the game's (below)
    sw      $k1, CTXL(0x1A0)
#if !defined(N64RT_NO_FASTMON) && !defined(N64RT_STATS)
    # ---- the fast path: a plain lw / sw of a register that is a copy of another word ----------------------------------
    # The game's load or store of an SP or DPC register (the RSP hardware's own, at 0x2000_0000), of PI_DRAM_ADDR,
    # PI_CART_ADDR and the PI's domain registers or of an RI register (the runtime's tables, n64rt_io.c) is done here with
    # $k0 and $k1 alone, and the game goes on past it: nothing else is saved or restored. What this does not take -- a
    # different cause, a delay slot of another branch than jr, code outside KSEG0, another instruction, another register,
    # a PI_WR_LEN, the SI --
    # falls through to the full path below at any point: nothing of the game's has been touched before the access
    # itself, and the context holds what the full path needs (Status, Count, $k1; $k0 is in the monitor RAM).
    # Only once the MI is in hardware (n64rt_mon_fast_enable): otherwise the MI's line must be worked out after every write.
    lw      $k1, CTXL(0x1B4)
    beqz    $k1, n64rt_mon_slow
    mfc0    $k1, $7                 # MCTL: the cause (bits 7:3) and the delay slot (bit 2)
    andi    $k1, $k1, 0xF8
    xori    $k1, $k1, 0x20          # 4 (AdEL) -> 0, 5 (AdES) -> 8
    andi    $k1, $k1, 0xF7
    bnez    $k1, n64rt_mon_tlbq     # (not a load or store: maybe the game's TLB instruction, below)
    mfc0    $k1, $30                # ErrorEPC: the instruction, or the branch it is the delay slot of
    sw      $k1, CTXL(0x1A4)
    srl     $k1, $k1, 28
    xori    $k1, $k1, 8             # (code in KSEG0 only: the TLB's pages and the runtime's are the full path's)
    bnez    $k1, n64rt_mon_slow
    mfc0    $k1, $7
    andi    $k1, $k1, 4
    bnez    $k1, 10f
    lw      $k1, CTXL(0x1A4)
    addiu   $k1, $k1, 4             # not in a delay slot: the game goes on at the next instruction
    sw      $k1, CTXL(0x1BC)
    lw      $k1, CTXL(0x1A4)
    b       12f
    lw      $k1, 0($k1)
10: lw      $k1, 0($k1)             # in a delay slot: of a jr (libultra's leaf functions return a register so:
    sll     $k1, $k1, 11            # jr $ra / lw $v0, SP_STATUS); any other branch is the full path's
    srl     $k1, $k1, 11
    xori    $k1, $k1, 8             # (rt, rd, the hint 0; jr's function code)
    bnez    $k1, n64rt_mon_slow
    lw      $k1, CTXL(0x1A4)
    lw      $k1, 0($k1)
    srl     $k1, $k1, 26            # (SPECIAL)
    bnez    $k1, n64rt_mon_slow
    lw      $k1, CTXL(0x1A4)
    lw      $k1, 0($k1)
    srl     $k1, $k1, 21            # rs: the game goes on at its value as it was before the delay slot
    andi    $k1, $k1, 31
    sll     $k0, $k1, 3
    lui     $k1, %hi(n64rt_mon_rdtab)
    addiu   $k1, $k1, %lo(n64rt_mon_rdtab)
    addu    $k1, $k1, $k0
    lui     $k0, %hi(11f)
    jr      $k1
    addiu   $k0, $k0, %lo(11f)
11: lui     $k0, %hi(n64rt_mon_ctx)
    sw      $k1, CTXL(0x1BC)
    lw      $k1, CTXL(0x1A4)
    lw      $k1, 4($k1)             # the delay slot's instruction
12: sw      $k1, CTXL(0x1A8)        # the instruction word
    srl     $k1, $k1, 26
    xori    $k1, $k1, 0x23          # lw -> 0
    beqz    $k1, 1f
    xori    $k1, $k1, 0x08          # lw -> 8, sw -> 0
    bnez    $k1, n64rt_mon_slow
    nop
1:  sw      $k1, CTXL(0x1AC)        # 8: a load, 0: a store
    lw      $k1, CTXL(0x1A8)        # the base register's value, through the table (its number times 8)
    srl     $k1, $k1, 21
    andi    $k1, $k1, 31
    sll     $k0, $k1, 3
    lui     $k1, %hi(n64rt_mon_rdtab)
    addiu   $k1, $k1, %lo(n64rt_mon_rdtab)
    addu    $k1, $k1, $k0
    lui     $k0, %hi(2f)
    jr      $k1
    addiu   $k0, $k0, %lo(2f)
2:  lui     $k0, %hi(n64rt_mon_ctx)
    sw      $k1, CTXL(0x1B0)
    lw      $k1, CTXL(0x1A8)
    sll     $k1, $k1, 16
    sra     $k1, $k1, 16            # the offset
    lw      $k0, CTXL(0x1B0)
    addu    $k1, $k1, $k0           # the address
    lui     $k0, %hi(n64rt_mon_ctx)
    sw      $k1, CTXL(0x1B0)
    srl     $k1, $k1, 29
    xori    $k1, $k1, 4
    sltiu   $k1, $k1, 2             # KSEG0 or KSEG1 (a device's address)
    beqz    $k1, n64rt_mon_slow
    lw      $k1, CTXL(0x1B0)
    sll     $k1, $k1, 3
    srl     $k1, $k1, 3             # the physical address
    lui     $k0, 0x0404
    subu    $k0, $k1, $k0
    sltiu   $k0, $k0, 0x20
    bnez    $k0, 5f                 # SP_MEM_ADDR .. SP_SEMAPHORE
    lui     $k0, 0x0408
    beq     $k0, $k1, 6f            # SP_PC
    srl     $k0, $k1, 20
    xori    $k0, $k0, 0x041
    beqz    $k0, 7f                 # DPC
    xori    $k0, $k0, 0x041 ^ 0x046
    beqz    $k0, 8f                 # PI
    xori    $k0, $k0, 0x046 ^ 0x047
    beqz    $k0, 9f                 # RI
    nop
    b       n64rt_mon_slow
    nop
5:  andi    $k1, $k1, 0x1C          # SP: the hardware's register at 0x2000_0000 + the offset
#ifdef N64RT_RSPDUMP
    lui     $k0, %hi(n64rt_mon_ctx) # (RSPDUMP: a write of the DMA registers or SP_STATUS is marked in rsp_wr: the full path's)
    lw      $k0, CTXL(0x1AC)
    bnez    $k0, 55f
    sltiu   $k0, $k1, 0x14
    bnez    $k0, n64rt_mon_slow
    nop
55:
#endif
    lui     $k0, 0x2000
    b       n64rt_mon_dev
    or      $k1, $k1, $k0
6:  lui     $k1, 0x2000
    b       n64rt_mon_dev
    ori     $k1, $k1, 0x20
7:  sll     $k0, $k1, 12            # DPC: a read of START .. STATUS from 0x2000_0040 + the offset (a write is the full path's)
    srl     $k1, $k0, 14
    sll     $k1, $k1, 2
    sltiu   $k0, $k1, 0x10
    beqz    $k0, n64rt_mon_slow
    lui     $k0, %hi(n64rt_mon_ctx)
    lw      $k0, CTXL(0x1AC)
    beqz    $k0, n64rt_mon_slow
    lui     $k0, 0x2000
    addu    $k1, $k1, $k0
    b       n64rt_mon_dev
    addiu   $k1, $k1, 0x40
8:  sll     $k0, $k1, 12            # PI: the table's word, but for RD_LEN, WR_LEN (a DMA) and STATUS (the engine's)
    srl     $k1, $k0, 14
    sll     $k1, $k1, 2
    sltiu   $k0, $k1, 0x34
    beqz    $k0, n64rt_mon_slow
    sltiu   $k0, $k1, 8             # PI_DRAM_ADDR, PI_CART_ADDR: a store is the table's (the full path masks what a read gives)
    beqz    $k0, 81f
    lui     $k0, %hi(n64rt_mon_ctx)
    lw      $k0, CTXL(0x1AC)        # 8: a load
    bnez    $k0, n64rt_mon_slow
    nop
81:
    xori    $k0, $k1, 0x08
    beqz    $k0, n64rt_mon_slow
    xori    $k0, $k1, 0x0C
    beqz    $k0, n64rt_mon_slow
    xori    $k0, $k1, 0x10
    beqz    $k0, n64rt_mon_slow
    lui     $k0, %hi(n64rt_pi_regs)
    addiu   $k0, $k0, %lo(n64rt_pi_regs)
    b       n64rt_mon_dev
    addu    $k1, $k1, $k0
9:  sll     $k0, $k1, 12            # RI: the table's word
    srl     $k1, $k0, 14
    sll     $k1, $k1, 2
    sltiu   $k0, $k1, 0x20
    beqz    $k0, n64rt_mon_slow
    lui     $k0, %hi(n64rt_ri_regs)
    addiu   $k0, $k0, %lo(n64rt_ri_regs)
    b       n64rt_mon_dev
    addu    $k1, $k1, $k0
n64rt_mon_dev:                      # the access, at the address in $k1: a load into the game's rt, or a store of it
    lui     $k0, %hi(n64rt_mon_ctx)
    sw      $k1, CTXL(0x1B0)
    lw      $k1, CTXL(0x1AC)
    beqz    $k1, 3f
    lw      $k1, CTXL(0x1B0)
    lw      $k1, 0($k1)             # the load: its value into rt through the table (an exit of its own)
    sw      $k1, CTXL(0x1B0)
    lw      $k1, CTXL(0x1A8)
    srl     $k1, $k1, 16
    andi    $k1, $k1, 31
    sll     $k0, $k1, 3
    lui     $k1, %hi(n64rt_mon_wrtab)
    addiu   $k1, $k1, %lo(n64rt_mon_wrtab)
    addu    $k0, $k1, $k0
    lui     $k1, %hi(n64rt_mon_ctx)
    jr      $k0
    lw      $k1, %lo(n64rt_mon_ctx+0x1B0)($k1)
3:  lw      $k1, CTXL(0x1A8)        # the store: rt's value through the table, then the store
    srl     $k1, $k1, 16
    andi    $k1, $k1, 31
    sll     $k0, $k1, 3
    lui     $k1, %hi(n64rt_mon_rdtab)
    addiu   $k1, $k1, %lo(n64rt_mon_rdtab)
    addu    $k1, $k1, $k0
    lui     $k0, %hi(4f)
    jr      $k1
    addiu   $k0, $k0, %lo(4f)
4:  lui     $k0, %hi(n64rt_mon_ctx)
    lw      $k0, CTXL(0x1B0)
    sw      $k1, 0($k0)
    .globl  n64rt_mon_fast_exit
n64rt_mon_fast_exit:                # the game goes on past the instruction (or at the jr's target); Count as it was (below)
    lui     $k0, %hi(n64rt_mon_ctx)
    lw      $k1, CTXL(0x1BC)
    mtc0    $k1, $30
    lw      $k1, CTXL(0x1B8)
    addiu   $k1, $k1, 1
    sw      $k1, CTXL(0x1B8)
    lw      $k1, CTXL(0x04)         # (Count, as the full path's exit: put back only when the game has its interrupts off)
    andi    $k1, $k1, 3
    xori    $k1, $k1, 1
    beqz    $k1, 6f
    nop
    lw      $k1, CTXL(0x1A0)
    mtc0    $k1, $9
6:  ld      $k1, CTXL(8*27)
    lw      $k0, -0x7FFC($zero)
    eret
    nop
# The tables: entry n (8 bytes) is register n. n64rt_mon_rdtab: its value into $k1, back to the address in $k0;
# n64rt_mon_wrtab: $k1 into it, and the exit. The game's $k0 is the monitor RAM's word, its $k1 the context's.
    .balign 32
n64rt_mon_rdtab:
    jr      $k0
    or      $k1, $zero, $zero
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25
    jr      $k0
    or      $k1, $\n, $zero
    .endr
    jr      $k0
    lw      $k1, -0x7FFC($zero)
    b       n64rt_mon_rd_k1
    nop
    .irp    n, 28,29,30,31
    jr      $k0
    or      $k1, $\n, $zero
    .endr
n64rt_mon_rd_k1:
    lui     $k1, %hi(n64rt_mon_ctx)
    lw      $k1, %lo(n64rt_mon_ctx+8*27+4)($k1)
    jr      $k0
    nop
    .balign 32
n64rt_mon_wrtab:
    b       n64rt_mon_fast_exit
    nop
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25
    b       n64rt_mon_fast_exit
    or      $\n, $k1, $zero
    .endr
    b       n64rt_mon_fast_exit
    sw      $k1, -0x7FFC($zero)
    b       n64rt_mon_wr_k1
    nop
    .irp    n, 28,29,30,31
    b       n64rt_mon_fast_exit
    or      $\n, $k1, $zero
    .endr
n64rt_mon_wr_k1:
    lui     $k0, %hi(n64rt_mon_ctx)
    sw      $k1, CTXL(8*27+4)
    sra     $k1, $k1, 31
    b       n64rt_mon_fast_exit
    sw      $k1, CTXL(8*27)

# ---- the fast path for the game's own TLB instructions ----------------------------------------------------------------
# tlbr, tlbp and the tlbwi that changes no translation (the entry it replaces maps nothing and the one it writes has both pages
# invalid: osUnmapTLBAll's 32 writes) are done here in KSEG0 code, not on a DELAY SLOT, without the full path's save of all the
# registers: GoldenEye's refill handler walks the 32 entries with a tlbr each, 842 of them in 9 s, ~930 cycles apiece through
# n64rt_tlb_insn (n64rt_os.c). Seven of the game's registers ($8-$14) are saved in the context for the work and put back; the
# tables are n64rt_tlbr (the entries as the game's registers gave them, 16 bytes: mask, hi ^ 0x80000000, lo0, lo1) and n64rt_tlbs
# (osMapTLB's, 20 bytes, whose `span` at +4 is 0 for an entry that maps nothing). Anything else -- a tlbwr, a write that maps or
# unmaps a page (the CPU's translation tables to empty), a delay slot, code outside KSEG0 -- is the full path's, with nothing yet
# changed: the registers are put back first.
n64rt_mon_tlbq:
    mfc0    $k1, $7                 # MCTL: the cause (bits 7:3) and the delay slot (bit 2)
    andi    $k1, $k1, 0xFC
    xori    $k1, $k1, 0x50          # 10 (a Reserved Instruction), not in a delay slot
    bnez    $k1, n64rt_mon_slow
    mfc0    $k1, $30                # ErrorEPC
    srl     $k0, $k1, 28
    xori    $k0, $k0, 8             # KSEG0
    bnez    $k0, n64rt_mon_slow
    lui     $k0, 0x4200             # (this delay slot must not be the load: for code on a mapped page, GoldenEye's libultra at
    lw      $k1, 0($k1)             #  0x7000_0000, it runs before the branch's decision and faults on the tables, with ErrorEPC the
                                    #  same: the entry came back for ever)
    xor     $k0, $k0, $k1
    srl     $k0, $k0, 6             # CO set and the rest 0, but the function
    bnez    $k0, n64rt_mon_slow
    andi    $k0, $k1, 0x3F
    addiu   $k0, $k0, -1            # 1 tlbr, 2 tlbwi, 8 tlbp
    beqz    $k0, 1f
    addiu   $k0, $k0, -1
    beqz    $k0, 2f
    addiu   $k0, $k0, -6
    beqz    $k0, 3f
    nop
    b       n64rt_mon_slow
    nop
    .macro  TLB_SAVE
    lui     $k0, %hi(n64rt_mon_ctx)
    .irp    n, 8,9,10,11,12,13,14
    sd      $\n, CTXL(8*\n)
    .endr
    .endm
    .macro  TLB_EXIT                # the game goes on past the instruction (ErrorEPC + 4)
    lui     $k0, %hi(n64rt_mon_ctx)
    mfc0    $k1, $30
    addiu   $k1, $k1, 4
    sw      $k1, CTXL(0x1BC)
    b       n64rt_mon_fast_exit
    nop
    .endm
    .macro  TLB_DONE                # ... with the seven registers put back
    lui     $k0, %hi(n64rt_mon_ctx)
    .irp    n, 8,9,10,11,12,13,14
    ld      $\n, CTXL(8*\n)
    .endr
    mfc0    $k1, $30
    addiu   $k1, $k1, 4
    sw      $k1, CTXL(0x1BC)
    b       n64rt_mon_fast_exit
    nop
    .endm
# tlbr: EntryHi, PageMask, EntryLo0/1 := entry Index & 31 (G in both EntryLo: the two entries' AND)
1:  TLB_SAVE
    mfc0    $8, $0
    andi    $8, $8, 31
    sll     $8, $8, 4
    lui     $9, %hi(n64rt_tlbr)
    addiu   $9, $9, %lo(n64rt_tlbr)
    addu    $9, $9, $8
    lw      $10, 0($9)
    mtc0    $10, $5                 # PageMask
    lw      $10, 4($9)
    lui     $11, 0x8000
    xor     $10, $10, $11
    mtc0    $10, $10                # EntryHi
    lw      $10, 8($9)
    lw      $11, 12($9)
    and     $12, $10, $11
    andi    $12, $12, 1             # G
    ori     $10, $10, 1
    xori    $10, $10, 1
    or      $10, $10, $12
    mtc0    $10, $2
    ori     $11, $11, 1
    xori    $11, $11, 1
    or      $11, $11, $12
    mtc0    $11, $3
    TLB_DONE
# tlbwi: entry Index & 63 (nothing if 32 or more) := PageMask, EntryHi, EntryLo0, EntryLo1 -- but only when it changes no translation:
# checked with $k0 and $k1 alone first, so that the ones that do (a page mapped or unmapped) lose nothing to the register save
# (and while entries put out by a tlbwr are kept -- n64rt_os.c, n64rt_vic_n -- a tlbwi and a tlbp are C's: they look at them)
2:  lui     $k0, %hi(n64rt_vic_n)
    lw      $k0, %lo(n64rt_vic_n)($k0)
    bnez    $k0, n64rt_mon_slow
    mfc0    $k1, $0
    andi    $k1, $k1, 63
    sltiu   $k0, $k1, 32
    beqz    $k0, 21f                # (an Index of 32 or more writes nothing)
    sll     $k0, $k1, 4
    sll     $k1, $k1, 2
    addu    $k1, $k1, $k0           # index * 20
    lui     $k0, %hi(n64rt_tlbs)
    addiu   $k0, $k0, %lo(n64rt_tlbs)
    addu    $k1, $k1, $k0
    lw      $k1, 4($k1)             # span: the entry maps something
    bnez    $k1, 22f
    mfc0    $k0, $2                 # EntryLo0
    mfc0    $k1, $3                 # EntryLo1
    or      $k0, $k0, $k1
    andi    $k0, $k0, 2             # V of either
    bnez    $k0, n64rt_mon_slow
    nop
    TLB_SAVE
    mfc0    $8, $0
    andi    $8, $8, 31
    mfc0    $9, $2
    mfc0    $10, $3
    lui     $11, 0x3FFF
    ori     $11, $11, 0xFFFF
    and     $9, $9, $11             # lo0 &= 0x3FFFFFFF
    and     $10, $10, $11           # lo1 &= 0x3FFFFFFF
    mfc0    $11, $5
    lui     $12, 0x01FF
    ori     $12, $12, 0xE000
    and     $11, $11, $12           # mask = PageMask & 0x01FFE000
    mfc0    $12, $10                # EntryHi
    ori     $13, $11, 0x1F00
    nor     $13, $13, $0
    and     $12, $12, $13           # hi &= ~(mask | 0x1F00)
    lui     $13, 0x8000
    xor     $12, $12, $13           # (hi ^ 0x80000000)
    sll     $8, $8, 4
    lui     $13, %hi(n64rt_tlbr)
    addiu   $13, $13, %lo(n64rt_tlbr)
    addu    $13, $13, $8
    sw      $11, 0($13)
    sw      $12, 4($13)
    sw      $9, 8($13)
    sw      $10, 12($13)
    TLB_DONE
21: TLB_EXIT
# ... and the tlbwi that maps a pair of 4 KiB pages over an entry that mapped one ($k1: its span): osMapTLB moving a window
# of the game's to other frames, which a game can do a hundred times a field -- some 550 instructions apiece through the full
# path, a dozen of them on the way here. n64rt_os.c's tlb_write for that case: the entry in both tables, the old pair's pages
# out of the CPU's translation tables. (Anything else -- a bigger page, an entry that maps nothing now, the only mapped entry,
# whose first mapping the full path logs -- is the full path's.)
22: xori    $k1, $k1, 0x1000
    bnez    $k1, n64rt_mon_slow     # (the old entry's pages are bigger)
    mfc0    $k0, $5
    lui     $k1, 0x01FF
    ori     $k1, $k1, 0xE000
    and     $k0, $k0, $k1
    bnez    $k0, n64rt_mon_slow     # (PageMask: bigger pages now)
    mfc0    $k0, $2
    mfc0    $k1, $3
    or      $k0, $k0, $k1
    andi    $k0, $k0, 2
    beqz    $k0, n64rt_mon_slow     # (V of neither: it maps nothing now)
    lui     $k0, %hi(n64rt_tlb_n)
    lw      $k0, %lo(n64rt_tlb_n)($k0)
    xori    $k0, $k0, 1
    beqz    $k0, n64rt_mon_slow     # (the only entry that maps something: the full path may log it)
    nop
    TLB_SAVE
    .irp    n, 15,24,25
    sd      $\n, CTXL(8*\n)
    .endr
    mfc0    $8, $0
    andi    $8, $8, 31              # the index
    mfc0    $9, $2
    mfc0    $10, $3
    lui     $11, 0x3FFF
    ori     $11, $11, 0xFFFF
    and     $9, $9, $11             # lo0 &= 0x3FFFFFFF
    and     $10, $10, $11           # lo1
    mfc0    $12, $10
    lui     $11, 0xFFFF
    ori     $11, $11, 0xE0FF
    and     $12, $12, $11           # hi &= ~0x1F00 (no mask)
    sll     $14, $8, 4
    lui     $11, %hi(n64rt_tlbr)
    addiu   $11, $11, %lo(n64rt_tlbr)
    addu    $14, $14, $11
    sw      $0, 0($14)              # n64rt_tlbr[i]: mask 0, hi ^ 0x80000000, lo0, lo1
    lui     $11, 0x8000
    xor     $11, $12, $11
    sw      $11, 4($14)
    sw      $9, 8($14)
    sw      $10, 12($14)
    sll     $14, $8, 4
    sll     $11, $8, 2
    addu    $14, $14, $11
    lui     $11, %hi(n64rt_tlbs)
    addiu   $11, $11, %lo(n64rt_tlbs)
    addu    $14, $14, $11           # &n64rt_tlbs[i]
    lui     $11, %hi(n64rt_tlbram)
    lw      $11, %lo(n64rt_tlbram)($11)
    beqz    $11, 23f                # (no translation tables)
    lw      $15, 0($14)             # the old pair's va: its two pages out of both tables (tr_inval), EntryHi put back
    mfc0    $24, $10
    mtc0    $15, $10
    mtc0    $0, $23                 # (COP0 23: {page, the pair's half << 4, flags}: 0 the data table, 8 TR_FETCH)
    li      $11, 8
    mtc0    $11, $23
    li      $11, 16
    mtc0    $11, $23
    li      $11, 24
    mtc0    $11, $23
    mtc0    $24, $10
23: lui     $11, 0xFFFF
    ori     $11, $11, 0xE000
    and     $11, $12, $11
    sw      $11, 0($14)             # va: the pair's
    li      $11, 0x1000
    sw      $11, 4($14)             # span
    and     $11, $9, $10
    andi    $11, $11, 1             # G of both: asid -1, else EntryHi's
    addiu   $25, $0, -1
    bnez    $11, 24f
    nop
    andi    $25, $12, 0xFF
24: sw      $25, 16($14)
    addiu   $25, $0, -1             # pa[0], pa[1]: the frame, or -1 with V clear
    andi    $11, $9, 2
    beqz    $11, 25f
    move    $13, $25
    srl     $13, $9, 6
    sll     $13, $13, 12
25: sw      $13, 8($14)
    andi    $11, $10, 2
    beqz    $11, 26f
    move    $13, $25
    srl     $13, $10, 6
    sll     $13, $13, 12
26: sw      $13, 12($14)
    lui     $11, %hi(n64rt_tlb_unmaps)
    sw      $0, %lo(n64rt_tlb_unmaps)($11)
    lui     $k0, %hi(n64rt_mon_ctx)
    .irp    n, 15,24,25
    ld      $\n, CTXL(8*\n)
    .endr
    TLB_DONE
# tlbp: Index := the entry EntryHi names (VPN2 under the entry's mask, ASID unless global), or P (bit 31) if none
3:  lui     $k0, %hi(n64rt_vic_n)
    lw      $k0, %lo(n64rt_vic_n)($k0)
    bnez    $k0, n64rt_mon_slow
    nop
    TLB_SAVE
    mfc0    $8, $10                 # EntryHi: the key
    lui     $14, 0x8000
    xor     $14, $14, $8            # key ^ 0x80000000: an entry holds hi ^ 0x80000000, so (entry ^ this) is key ^ hi
    lui     $9, %hi(n64rt_tlbr)
    addiu   $9, $9, %lo(n64rt_tlbr)
    addiu   $13, $9, 32*16          # the end
30: lw      $11, 0($9)              # PageMask
    lw      $12, 4($9)
    ori     $11, $11, 0x1FFF
    nor     $11, $11, $0            # VPN2's bits
    xor     $12, $12, $14           # key ^ the entry's EntryHi
    and     $10, $12, $11
    bnez    $10, 31f                # (VPN2 differs)
    lw      $10, 8($9)
    lw      $11, 12($9)
    and     $10, $10, $11
    andi    $10, $10, 1             # G (both pages)
    bnez    $10, 32f
    andi    $12, $12, 0xFF          # the ASIDs' difference
    beqz    $12, 32f
    nop
31: addiu   $9, $9, 16
    bne     $9, $13, 30b
    nop
    b       33f
    lui     $10, 0x8000             # none: P
32: subu    $10, $9, $13
    addiu   $10, $10, 32*16
    srl     $10, $10, 4             # its index
33: mtc0    $10, $0
    TLB_DONE
#endif
# ---- the full path: the whole state of the game to the context, C, and back ----------------------------------------
n64rt_mon_slow:
    lui     $k0, %hi(n64rt_mon_ctx)
    addiu   $k0, $k0, %lo(n64rt_mon_ctx)
    sd      $1, 8*1($k0)            # ($1 is the scratch from here until it is saved again below)
    lw      $1, 0x04($k0)           # the game's Status
    lui     $k1, 0x2000
    or      $k1, $k1, $1            # (CU1: the FPU's registers are saved below)
    mtc0    $k1, $12
    ld      $1, 8*1($k0)            # ($1 back, saved with the rest below)
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    sd      $\n, 8*\n($k0)
    .endr
    lw      $k1, -0x7FFC($zero)     # the game's $k0 ($k1 is in the context already)
    sw      $k1, 8*26+4($k0)
    sra     $k1, $k1, 31
    sw      $k1, 8*26($k0)
    mfc0    $k1, $7                 # HI and LO, unless they are the runtime's (MCTL bit 0)
    andi    $k1, $k1, 1
    sw      $k1, 0x18C($k0)
    bnez    $k1, 2f
    nop
    mfhi    $k1
    sd      $k1, 0x190($k0)
    mflo    $k1
    sd      $k1, 0x198($k0)
2:  li      $k1, 2                  # the monitor's own HI and LO now (bit 0 clear); the game's vector stays (bit 1)
    mtc0    $k1, $7
    mfc0    $k1, $30
    sw      $k1, 0x00($k0)          # ErrorEPC
    cfc1    $k1, $31
    sw      $k1, 0x100($k0)
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    swc1    $f\n, 0x104+4*\n($k0)
    .endr
    la      $sp, n64rt_exc_stack_top
    jal     n64rt_mon_exception
    nop
    lui     $k0, %hi(n64rt_mon_ctx)
    addiu   $k0, $k0, %lo(n64rt_mon_ctx)
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    lwc1    $f\n, 0x104+4*\n($k0)
    .endr
    lw      $k1, 0x100($k0)
    ctc1    $k1, $31
    lw      $k1, 0x00($k0)
    mtc0    $k1, $30                # ErrorEPC
    lw      $k1, 0x04($k0)
    mtc0    $k1, $12                # (the game's Status; ERL stays set until the eret)
    lw      $k1, 0x18C($k0)
    bnez    $k1, 4f
    nop
    ld      $k1, 0x190($k0)
    mthi    $k1
    ld      $k1, 0x198($k0)
    mtlo    $k1
    b       5f
    move    $k1, $0
4:  lw      $k1, 0x194($k0)
    mthi    $k1
    lw      $k1, 0x19C($k0)
    mtlo    $k1
    li      $k1, 1
5:  ori     $k1, $k1, 2
    mtc0    $k1, $7
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    ld      $\n, 8*\n($k0)
    .endr
    # Count: back to what it was on the way in when the game has its interrupts off (IE clear, or EXL set) -- a monitor
    # entry the game does not ask for (a field, a tick) can land between its osGetCount and its write of Compare
    # (__osSetTimerIntr, with its interrupts off; the timer's handler, at EXL) and Count would pass Compare, which then
    # waits a wrap (91 s). With its interrupts on the game is in neither, and Count keeps the monitor's time: it is the
    # console's clock, and what the monitor does for the game takes real time. (Frozen always, a game's wait by osGetTime
    # lasted as much longer as the monitor worked in it: a wait of 3.8 s around a loop of 64-bit conversions, which are
    # the monitor's, took 14 s.)
    lw      $k1, 0x04($k0)          # the game's Status, as it goes back
    andi    $k1, $k1, 3
    xori    $k1, $k1, 1
    beqz    $k1, 6f                 # IE set, EXL clear
    nop
    lw      $k1, 0x1A0($k0)
    mtc0    $k1, $9
6:
    ld      $k1, 8*27($k0)          # the game's $k0 and $k1, as the runtime left them: 64 bits each, k0 last
    ld      $k0, 8*26($k0)          # (its own base)
    eret
    nop
n64rt_mon_stray:                    # (the vector without the monitor level: a fault of the runtime's own)
    lui     $sp, %hi(n64rt_exc_stack_top)
    addiu   $sp, $sp, %lo(n64rt_exc_stack_top)
    jal     n64rt_mon_fatal
    nop
1:  b       1b
    nop
#else
    .globl  exception_entry
exception_entry:
#ifdef N64RT_STATS
    mfc0    $k1, $9                 # (a build that counts: Count on the way in and, before eret, on the way out)
    lui     $k0, %hi(n64rt_stat_in)
    sw      $k1, %lo(n64rt_stat_in)($k0)
#endif
    lui     $k0, %hi(n64rt_cur)
    lw      $k0, %lo(n64rt_cur)($k0)
    lui     $k1, %hi(n64rt_cpu64)
    lw      $k1, %lo(n64rt_cpu64)($k1)
    beqz    $k1, 1f
    addiu   $k0, $k0, CTX
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    sd      $\n, 8*\n($k0)
    .endr
    mfc0    $k1, $7                 # HI and LO are the runtime's (a 64-bit product in the
    sw      $k1, 0x18C($k0)         # context already): nothing of the CPU's to save
    bnez    $k1, 2f
    mtc0    $0, $7
    mfhi    $k1
    sd      $k1, 0xD0($k0)
    mflo    $k1
    b       2f
    sd      $k1, 0xD8($k0)
1:
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    sw      $\n, 8*\n+4($k0)
    .endr
    mfhi    $k1
    sw      $k1, 0xD4($k0)
    mflo    $k1
    sw      $k1, 0xDC($k0)
2:
    mfc0    $k1, $14
    sw      $k1, 0x00($k0)
    mfc0    $k1, $12
    sw      $k1, 0x04($k0)
    cfc1    $k1, $31
    sw      $k1, 0x100($k0)
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    swc1    $f\n, 0x104+4*\n($k0)
    .endr
    la      $sp, n64rt_exc_stack_top
    jal     n64rt_exception
    nop
#endif
    .globl  n64rt_resume
n64rt_resume:                       # (also how the first thread starts)
    lui     $k0, %hi(n64rt_cur)
    lw      $k0, %lo(n64rt_cur)($k0)
    addiu   $k0, $k0, CTX
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    lwc1    $f\n, 0x104+4*\n($k0)
    .endr
    lw      $k1, 0x100($k0)
    ctc1    $k1, $31
    lw      $k1, 0x00($k0)
    mtc0    $k1, $14
    lw      $k1, 0x04($k0)          # (EXL set: still at exception level until eret)
    mtc0    $k1, $12
    lui     $k1, %hi(n64rt_cpu64)
    lw      $k1, %lo(n64rt_cpu64)($k1)
    beqz    $k1, 1f
    nop
    lw      $k1, 0x18C($k0)
    bnez    $k1, 4f                 # the runtime's HI and LO: the CPU's get their lower words
    nop                             # (a 64-bit value into them would come back here)
    ld      $k1, 0xD0($k0)
    mthi    $k1
    ld      $k1, 0xD8($k0)
    mtlo    $k1
    b       5f
    move    $k1, $0
4:  lw      $k1, 0xD4($k0)
    mthi    $k1
    lw      $k1, 0xDC($k0)
    mtlo    $k1
    li      $k1, 1
5:
#ifdef N64RT_OWN
    ori     $k1, $k1, 2             # (the game's own vector from here: MCTL.GAME)
#endif
    mtc0    $k1, $7
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    ld      $\n, 8*\n($k0)
    .endr
#ifdef N64RT_STATS
    mfc0    $k1, $9
    lui     $k0, %hi(n64rt_stat_out)
    sw      $k1, %lo(n64rt_stat_out)($k0)
#endif
    eret
    nop
1:
    lw      $k1, 0xD4($k0)
    mthi    $k1
    lw      $k1, 0xDC($k0)
    mtlo    $k1
    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31
    lw      $\n, 8*\n+4($k0)
    .endr
    eret
    nop

# void n64rt_switch(void): give the CPU to whichever thread should run now
# (the caller has queued itself, or set its state, first). Returns when this
# thread is chosen again.
    .globl  n64rt_switch
n64rt_switch:
    syscall
    jr      $ra
    nop

# void n64rt_call_on_stack(void (*fn)(void *), void *arg, void *stack_top):
# fn(arg) on a stack of the runtime's own. The game's threads have small
# stacks (a few KiB, next to their OSThread); the runtime's big work -- an
# RSP task, a printf -- must not run on them. (The caller masks interrupts,
# so nothing else enters the same stack.)
    .globl  n64rt_call_on_stack
n64rt_call_on_stack:
    addiu   $a2, $a2, -24
    sw      $sp, 16($a2)
    sw      $ra, 20($a2)
    move    $sp, $a2
    move    $t9, $a0
    jalr    $t9
    move    $a0, $a1
    lw      $ra, 20($sp)
    lw      $sp, 16($sp)
    jr      $ra
    nop

# ---- the patched functions' way in --------------------------------------
# A replaced libultra function starts with
#     lui  $t9, %hi(n64rt_dispatch)
#     jr   $t9
#     ori  $t9, $t9, <index>
# (n64rt_boot.c), so it arrives here with the game's arguments, return
# address and stack, and its index in t9's low half. $t9 and $at are
# caller-saved scratch in the o32 convention: the function is entered as if
# called directly.
    .section .text.n64rt_dispatch, "ax"
    .balign 65536
    .globl  n64rt_dispatch
n64rt_dispatch:
    andi    $t9, $t9, 0xFFFF
    sll     $t9, $t9, 2
#ifdef N64RT_STATS
# (a build that counts: n64rt_stats, 4 KiB aligned -- each function's calls,
# and from its word 256 on the table again)
    lui     $at, %hi(n64rt_stats)
    addu    $at, $at, $t9
    lw      $t9, %lo(n64rt_stats)($at)
    addiu   $t9, $t9, 1
    sw      $t9, %lo(n64rt_stats)($at)
    lw      $t9, %lo(n64rt_stats + 1024)($at)
#else
    lui     $at, %hi(n64rt_table)
    addu    $at, $at, $t9
    lw      $t9, %lo(n64rt_table)($at)
#endif
    jr      $t9
    nop

    .bss
    .balign 16
    .space  8192
    .globl  n64rt_exc_stack_top
n64rt_exc_stack_top:
    .space  16
