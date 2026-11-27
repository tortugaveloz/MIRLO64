# lld_game: a game that uses LLD and SCD, which the CPU does not have (a Reserved Instruction: the runtime does them, n64rt_os.c emulate_lld_scd), as
# libdragon's rand() and entropy pool do (a compare-and-swap loop on a 64-bit state). It leaves its results in the RDRAM at 0x8030_0000 for
# sim/mips_soc/lld_check.sh to read (DUMP=0x43B00000,64 in the sim) and ends with the magic 0x600D600D in word 0.
#   make -f lld_game.mk [BUILD=<dir>]       -> $(BUILD)/lld_game.z64
    .set    noreorder
    .set    noat
    .set    mips3
    .section .text.start, "ax"
    .globl  _start
    .macro  LI64 r, hi, lo
    lui     \r, %hi(\hi)
    addiu   \r, \r, %lo(\hi)
    dsll32  \r, \r, 0
    lui     $1, %hi(\lo)
    addiu   $1, $1, %lo(\lo)
    dsll32  $1, $1, 0
    dsrl32  $1, $1, 0
    or      \r, \r, $1
    .endm
_start:
    lui     $16, 0x8030              # s0: the report, and the data: 0x8030_0000 + 0x800 (cached KSEG0)
    ori     $16, $16, 0x0800
    lui     $17, 0xA030              # s1: the same memory through KSEG1
    ori     $17, $17, 0x0800
    lui     $18, 0x8030              # s2: the results at 0x8030_0040 on
    ori     $18, $18, 0x0040
    # (1) lld, scd of the same doubleword: the new value is stored and the register is 1
    LI64    $8, 0x01234567, 0x89ABCDEF
    sd      $8, 0($16)
    lld     $10, 0($16)
    LI64    $11, 0x11111111, 0x22222222
    scd     $11, 0($16)
    ld      $12, 0($16)
    sd      $10, 0($18)              # 0x40: what lld read
    sd      $11, 8($18)              # 0x48: scd's answer (1)
    sd      $12, 16($18)             # 0x50: the doubleword after
    # (2) the doubleword is written between them: scd fails and leaves it alone
    lld     $10, 0($16)
    li      $9, 0x77
    sw      $9, 4($16)               # the lower word -> 0x11111111_00000077
    LI64    $11, 0x33333333, 0x44444444
    scd     $11, 0($16)
    ld      $12, 0($16)
    sd      $10, 24($18)             # 0x58: what lld read (0x11111111_22222222)
    sd      $11, 32($18)             # 0x60: scd's answer (0)
    sd      $12, 40($18)             # 0x68: the doubleword (0x11111111_00000077)
    # (3) scd with no lld before it (the last scd ended the link): fails
    LI64    $11, 0x55555555, 0x66666666
    scd     $11, 0($16)
    ld      $12, 0($16)
    sd      $11, 48($18)             # 0x70: 0
    sd      $12, 56($18)             # 0x78: still 0x11111111_00000077
    # (4) lld in a branch's delay slot, then the pair at a doubleword of its own: success
    LI64    $8, 0xAAAABBBB, 0xCCCCDDDD
    sd      $8, 8($16)
    beq     $0, $0, 1f
    lld     $10, 8($16)
1:  LI64    $11, 0x0F0F0F0F, 0xF0F0F0F0
    beq     $0, $0, 2f
    scd     $11, 8($16)              # (scd in a delay slot too)
2:  ld      $12, 8($16)
    sd      $10, 64($18)             # 0x80: what lld read
    sd      $11, 72($18)             # 0x88: 1
    sd      $12, 80($18)             # 0x90: the new value
    # (5) libdragon's xorshift update on a state at 0x8030_0810, as a compare-and-swap loop, three times; the returned values
    LI64    $8, 0x12345678, 0x9ABCDEF0
    sd      $8, 16($16)
    addiu   $4, $16, 16
    li      $22, 3
    addiu   $19, $18, 96            # results at 0x8030_00A0
3:  ld      $2, 0($4)
    dsrl    $5, $2, 12
    xor     $5, $5, $2
    dsll    $3, $5, 25
    xor     $3, $3, $5
    dsrl    $5, $3, 27
    xor     $3, $5, $3
    move    $6, $2
    lld     $2, 0($4)
    bne     $2, $6, 5f
    li      $5, 0
    move    $1, $3
    scd     $1, 0($4)
    beqz    $1, 3b
    li      $5, 1
5:  beqzl   $5, 3b
    ld      $2, 0($4)
    sd      $3, 0($19)
    addiu   $19, $19, 8
    addiu   $22, $22, -1
    bnez    $22, 3b
    nop
    # (6) the same memory through KSEG1 (uncached): a successful pair and a failed one
    LI64    $8, 0xDEADBEEF, 0xFEEDFACE
    sd      $8, 24($17)
    lld     $10, 24($17)
    LI64    $11, 0x01010101, 0x02020202
    scd     $11, 24($17)
    ld      $12, 24($17)
    sd      $10, 0($19)              # 0xB8: what lld read
    sd      $11, 8($19)              # 0xC0: 1
    sd      $12, 16($19)             # 0xC8: the new value
    lld     $10, 24($17)
    sw      $0, 28($17)
    LI64    $11, 0x03030303, 0x04040404
    scd     $11, 24($17)
    ld      $12, 24($17)
    sd      $11, 24($19)             # 0xD0: 0
    sd      $12, 32($19)             # 0xD8: 0x01010101_00000000
    # done
    lui     $8, 0x8030
    lui     $9, 0x600D
    ori     $9, $9, 0x600D
    sw      $9, 0($8)
4:  b       4b
    nop
