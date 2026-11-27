# cartwr_game: a game that stores to the cartridge's bus where nothing answers it -- the CPU's pointer is a stray one (an index no one set), a byte store
# to 0xB9DD_C776, as a game that works on the console does: the PI writes to a bus the ROM ignores and no exception is taken. The runtime
# (n64rt_io.c n64rt_hw_write) takes the store the CPU raises for it and goes on. It leaves a count in the RDRAM at 0x8030_0040 and the magic 0x600D600D in
# word 0 of 0x8030_0000 when it is through, for sim/mips_soc/cartwr_check.sh to read (DUMP=0x43B00000,64 in the sim).
#   make -f cartwr_game.mk [BUILD=<dir>]       -> $(BUILD)/cartwr_game.z64
    .set    noreorder
    .set    noat
    .set    mips3
    .section .text.start, "ax"
    .globl  _start
_start:
    lui     $16, 0x8030              # s0: the report, 0x8030_0000
    lui     $17, 0xB9DD              # s1: a stray pointer into the cartridge's bus, 0xB9DD_C770 (KSEG1)
    ori     $17, $17, 0xC770
    lui     $18, 0x99DD              # s2: the same through KSEG0, 0x99DD_C770
    ori     $18, $18, 0xC770
    lui     $19, 0xB400              # s3: the first word past the 64 MiB the ROM may have, 0xB400_0000
    li      $8, 0x55
    li      $15, 0
    sb      $8, 6($17)               # a byte store, the one that stopped a game
    addiu   $15, $15, 1
    sh      $8, 6($17)               # a half word
    addiu   $15, $15, 1
    sw      $8, 4($17)               # a word
    addiu   $15, $15, 1
    sb      $8, 6($18)               # the same through KSEG0
    addiu   $15, $15, 1
    sw      $8, 0($19)               # the first word of the range
    addiu   $15, $15, 1
    sb      $8, 3($19)               # and its first word's last byte
    addiu   $15, $15, 1
    lui     $9, 0x1FBF               # the last word before the PIF's: 0xBFBF_FFFC
    ori     $9, $9, 0xFFFC
    lui     $10, 0xA000
    addu    $9, $9, $10
    sw      $8, 0($9)
    addiu   $15, $15, 1
    sw      $15, 0x40($16)           # 0x40: seven stores gone through
    lui     $9, 0x600D
    ori     $9, $9, 0x600D
    sw      $9, 0($16)               # the magic
1:  b       1b
    nop
