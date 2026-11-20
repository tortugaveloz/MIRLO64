/* The PI's domain 2 and the cartridge's reads in hardware (the SoC built with MIRLO_MON, MIRLO_MI, MIRLO_PI, MIRLO_PI2;
 * sim/mips_soc; rtl/soc/rcp_mi.sv, mirlo_mips.sv's X_CART, mips_core's PI2W).
 *
 * Checked, phase by phase:
 *   1  the engine from a cartridge address 2 mod 4 and of any length: copies of 1 .. 4097 bytes from a word's first and
 *      second half into the RDRAM, every byte against the source, and not a byte more (the guard bytes either side);
 *      the PILEN of a core without PI2 (whole words) and its PIDST (the RDRAM's 21-bit word) still work
 *   2  the RDRAM into the save's memory (PIDST: an SDRAM word, PI_RD_LEN's direction), of an even length that is not a
 *      multiple of 4
 *   3  the game CPU's reads of the ROM (0xB000_0000.., KSEG0 too, the last word of 48 MiB) and its reads and writes of
 *      domain 2 as an SRAM (the save's memory at 0x0332_0000; the window repeats every 128 KiB); a read while a copy runs
 *      waits for its end and reads right
 *   4  the FlashRAM: lang/n64rt/n64rt_save.h -- the runtime's own command interpreter -- with the engine and the game's
 *      reads of domain 2 in hardware (MONITOR.PI2's bit 0: a read is the status word, a write goes nowhere), driven as
 *      libultra's osFlash* drive it: the silicon ID by DMA (osFlashReadId), the status (osFlashReadStatus), a chip erase,
 *      a page written (osFlashWriteBuffer: the RDRAM into the page buffer by DMA; osFlashWriteArray; the program at
 *      osFlashClearStatus), read back by DMA (osFlashReadArray: devAddr page * 64), a sector erase
 *      the PI's write latch (phase 5): a write to the ROM's bus, a read while it goes on returns its word, a second write is ignored,
 *      and the ROM is read again when it is over
 *   5  the game's own lw / sw (from the RDRAM, MCTL.GAME, the pass-through on): no entry into the monitor for the ROM's and
 *      domain 2's words; an entry for the FlashRAM's command register (the runtime's) and for a half-word load (lh)
 * The source data is a pseudo-random pattern (lcg). */
#include <stdint.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>
#include <layout.h>

typedef uint8_t u8;
typedef uint32_t u32;

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}
static void nolog(const char *f, ...) { (void)f; }

/* the runtime's FlashRAM (n64rt_save.h), with this d2_use: domain 2 is a FlashRAM once a command came */
static void d2_use(int kind);
#define N64RT_SAVE_LOG nolog
#include "../../n64rt/n64rt_save.h"
static void d2_use(int kind)
{
    if (s_d2 == D2_FLASH || (int)s_d2 == kind) return;
    s_d2 = (u32)kind;
    if (kind == D2_FLASH) { fl_status(0u, 0u); monitor_pi2_write(1u); }
}

#define RES   ((volatile uint32_t *)0xA0000400u)     /* the game's results, RDRAM 0x400 (KSEG1) */
#define ROM   0x40100000u                             /* the ROM's place in the SDRAM: 1 MiB */
#define SRC   0x42000000u                             /* the engine's source pattern */
#define DSTP  0x00010000u                             /* the RDRAM's destination (physical) */
#define D2    ((volatile uint32_t *)0xA8000000u)      /* domain 2, KSEG1 */

static uint32_t lcg_s = 12345u;
static uint32_t lcg(void) { lcg_s = lcg_s * 1103515245u + 12345u; return lcg_s >> 8 ^ lcg_s << 13; }

static void pi_wait(void) { for (uint32_t t = 0; t < 4000000u && (monitor_pilen_read() >> 31); t++) { } }
/* an SDRAM-to-SDRAM copy by the engine (byte addresses; the source may be 2 mod 4) */
static void pi_run(uint32_t dst, uint32_t src, uint32_t bytes)
{
    monitor_pidst_write((dst - 0x40000000u) >> 2);
    monitor_pisrc_write((src - 0x40000000u) >> 2 | ((src >> 1) & 1u) << 24);
    monitor_pilen_write(((bytes + 3u) >> 2) | (bytes & 3u) << 22);
    pi_wait();
}
static inline uint8_t rd8(uint32_t a) { return *(volatile uint8_t *)(uintptr_t)a; }

/* the monitor: its RAM (0xFFFF_8000) has the game's $k0, $k1 and the counts [2] entries, [3] loads (AdEL), [4] stores
 * (AdES), [5] anything else; the instruction is skipped */
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl exception_entry\n"
"exception_entry:\n"
"    sw    $k0, -0x8000($zero)\n"
"    sw    $k1, -0x7FFC($zero)\n"
"    lw    $k0, -0x7FF8($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF8($zero)\n"
"    mfc0  $k0, $7\n"
"    srl   $k1, $k0, 3\n"
"    andi  $k1, $k1, 31\n"
"    li    $k0, 4\n"
"    beq   $k1, $k0, 40f\n"
"    li    $k0, 5\n"
"    beq   $k1, $k0, 50f\n"
"    nop\n"
"    lw    $k0, -0x7FEC($zero)\n"
"    addiu $k0, $k0, 1\n"
"    b     60f\n"
"    sw    $k0, -0x7FEC($zero)\n"
"40: lw    $k0, -0x7FF4($zero)\n"
"    addiu $k0, $k0, 1\n"
"    b     60f\n"
"    sw    $k0, -0x7FF4($zero)\n"
"50: lw    $k0, -0x7FF0($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF0($zero)\n"
"60: mfc0  $k0, $30\n"
"    addiu $k0, $k0, 4\n"
"    mtc0  $k0, $30\n"
"    lw    $k0, -0x8000($zero)\n"
"    lw    $k1, -0x7FFC($zero)\n"
"    eret\n"
"    nop\n"
);

/* the game (section "pi2game", copied to the RDRAM's 0x1000) */
__attribute__((section("pi2game"), noinline, used, optimize("O1"))) void pi2_game(void)
{
    volatile uint32_t *res = (volatile uint32_t *)0xA0000400u;
    res[0] = *(volatile uint32_t *)0xB0000000u;                 /* the ROM, KSEG1 */
    res[1] = *(volatile uint32_t *)0xB0000004u;
    res[2] = *(volatile uint32_t *)0x90000008u;                 /* ... KSEG0 */
    res[3] = *(volatile uint32_t *)0xB2FFFFFCu;                 /* the last word of its 48 MiB */
    *(volatile uint32_t *)0xA8000040u = 0x12345678u;            /* domain 2, an SRAM */
    res[4] = *(volatile uint32_t *)0xA8000040u;
    res[5] = *(volatile uint32_t *)0x88000044u;                 /* ... KSEG0 */
    uint32_t h = 0xDEADu;
    __asm__ volatile("lh %0, 2(%1)" : "+r"(h) : "r"(0xB0000000u) : "memory");   /* a half-word: the monitor's */
    res[6] = h;
    *(volatile uint32_t *)0xA8010000u = 0xF0000000u;            /* the FlashRAM's command register: the monitor's */
    /* the PI's write latch: a write to the cartridge's bus (the ROM ignores it) is asynchronous; while it goes on a read, of any
     * address, returns its word and a second write is ignored */
    res[11] = *(volatile uint32_t *)0xB0000004u;
    *(volatile uint32_t *)0xB0000010u = 0xCAFEF00Du;
    res[8] = *(volatile uint32_t *)0xB0000004u;
    *(volatile uint32_t *)0xB0000020u = 0x11111111u;
    res[9] = *(volatile uint32_t *)0xB0000008u;
    for (volatile int i = 0; i < 400; i++) { }                  /* (the write is over) */
    res[10] = *(volatile uint32_t *)0xB0000004u;
    res[7] = 1u;
}
extern const uint32_t __start_pi2game[], __stop_pi2game[];

static void fill_src(uint32_t bytes)
{
    volatile uint32_t *s = (volatile uint32_t *)(uintptr_t)SRC;
    for (uint32_t i = 0; i < (bytes + 8u) / 4u; i++) s[i] = lcg();
}
/* a copy from SRC + half of `len` bytes into the RDRAM's DSTP, checked byte for byte, guards either side */
static uint32_t copy_check(uint32_t half, uint32_t len)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(0xA0000000u + DSTP);
    for (uint32_t i = 0; i < len / 4u + 6u; i++) d[i] = 0xEEEEEEEEu;
    d[-1] = 0xEEEEEEEEu; d[-2] = 0xEEEEEEEEu;
    pi_run(0x43800000u + DSTP, SRC + half, len);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < len; i++) if (rd8(0xA0000000u + DSTP + i) != rd8(SRC + half + i)) bad++;
    for (uint32_t i = len; i < len + 8u; i++) if (rd8(0xA0000000u + DSTP + i) != 0xEEu) bad += 0x10000u;
    if (d[-1] != 0xEEEEEEEEu || d[-2] != 0xEEEEEEEEu) bad += 0x1000000u;
    if (monitor_pilen_read() != 0u) bad += 0x10000000u;
    return bad;
}
/* a FlashRAM DMA (rd: the RDRAM into it), as the runtime starts it: the engine at d2_engine_addr's */
static int fl_dma(int rd, uint32_t off, uint32_t rdram_phys, uint32_t size)
{
    uint32_t a = d2_engine_addr(rd, off, size);
    if (!a) return 0;
    if (rd) pi_run(a, 0x43800000u + rdram_phys, size); else pi_run(0x43800000u + rdram_phys, a, size);
    return 1;
}
static void cmd(uint32_t c) { flash_cmd(c); }                   /* what the runtime does at the trap of a command */
static uint32_t readio(void) { return D2[0]; }                  /* the game's osEPiReadIo of domain 2: the hardware's */
static void flash_status(void) { cmd(0xD2000000u); (void)readio(); cmd(0xD2000000u); (void)readio(); }
static void flash_clear(void) { cmd(0xD2000000u); D2[0] = 0u; }

int main(void)
{
    puts_("pi2_test\n");
    uint32_t id = monitor_id_read();
    if ((id & 0x46u) != 0x46u) { puts_("no MI, PI and PI2 in this SoC\npi2_test: FAILED\n"); putc_(4); for (;;) ; }
    monitor_miclr_write(0x3Eu);

    /* 1: the engine, any length, either half */
    static const uint16_t lens[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 31, 62, 63, 64, 125, 126, 127, 128, 129, 130,
                                     255, 256, 1023, 1024, 4097 };
    fill_src(8192u);
    flush_cpu_dcache();
    for (uint32_t half = 0; half <= 2u; half += 2u)
        for (uint32_t k = 0; k < sizeof lens / sizeof lens[0]; k++) {
            uint32_t bad = copy_check(half, lens[k]);
            if (bad) { puts_("  half "); puthex(half); puts_(" len "); puthex(lens[k]); puts_("\n"); }
            check("engine copy: bytes wrong / guard / before / left", bad, 0u);
        }
    {   /* the timing of 64 KiB from a second half */
        timer0_uptime_latch_write(1); uint32_t t0 = timer0_uptime_cycles_read();
        pi_run(0x43800000u + 0x20000u, SRC + 2u, 0x10000u);
        timer0_uptime_latch_write(1); uint32_t t1 = timer0_uptime_cycles_read();
        puts_("PI engine: "); puthex(t1 - t0); puts_(" sys cycles for 64 KiB from a half word\n");
    }
    {   /* a core without PI2's PIDST (the RDRAM's 21-bit word) and PILEN (whole words) */
        volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(0xA0000000u + DSTP);
        for (uint32_t i = 0; i < 20u; i++) d[i] = 0;
        monitor_pidst_write(DSTP >> 2);
        monitor_pisrc_write((SRC - 0x40000000u) >> 2);
        monitor_pilen_write(16u);
        pi_wait();
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 16u; i++) if (d[i] != *(volatile uint32_t *)(uintptr_t)(SRC + 4u * i)) bad++;
        check("the old PIDST / PILEN: 16 words", bad, 0u);
        check("... not one more", d[16], 0u);
    }
    check("PI interrupt raised by the copies", monitor_miintr_read() & 0x10u, 0x10u);
    monitor_miclr_write(0x10u);

    /* 2: the RDRAM into the save's memory, 0x1452 bytes at 0x0332_0100 */
    {
        volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(0xA0000000u + 0x30000u);
        volatile uint32_t *sv = (volatile uint32_t *)(uintptr_t)LAYOUT_SAVE;
        for (uint32_t i = 0; i < 0x1460u / 4u; i++) r[i] = lcg();
        for (uint32_t i = 0; i < 0x1500u / 4u; i++) sv[0x3Fu + i] = 0x55555555u;
        pi_run(LAYOUT_SAVE + 0x100u, 0x43800000u + 0x30000u, 0x1452u);
        flush_cpu_dcache();
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 0x1452u; i++) if (rd8(LAYOUT_SAVE + 0x100u + i) != rd8(0xA0030000u + i)) bad++;
        check("RDRAM -> save: bytes", bad, 0u);
        check("... and not one more", rd8(LAYOUT_SAVE + 0x100u + 0x1452u) | rd8(LAYOUT_SAVE + 0x100u + 0x1453u) << 8, 0x5555u);
        check("... nor before", sv[0x3Fu], 0x55555555u);
    }

    /* 3: the CPU's reads of the ROM and its accesses to domain 2 (an SRAM) */
    {
        volatile uint32_t *rom = (volatile uint32_t *)(uintptr_t)ROM;
        for (uint32_t i = 0; i < 64u; i++) rom[i] = 0xB0000000u + i * 0x01010101u;
        *(volatile uint32_t *)(uintptr_t)(ROM + 0x02FFFFFCu) = 0x48484848u;
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 64u; i++) if (*(volatile uint32_t *)(uintptr_t)(0xB0000000u + 4u * i) != 0xB0000000u + i * 0x01010101u) bad++;
        check("ROM reads, KSEG1", bad, 0u);
        check("ROM read, KSEG0", *(volatile uint32_t *)0x90000010u, 0xB0000000u + 4u * 0x01010101u);
        check("ROM read at 48 MiB - 4", *(volatile uint32_t *)0xB2FFFFFCu, 0x48484848u);
        monitor_pi2_write(0u);
        volatile uint32_t *sv = (volatile uint32_t *)(uintptr_t)LAYOUT_SAVE;
        sv[8] = 0xCAFEF00Du;
        check("domain 2 (SRAM) read", D2[8], 0xCAFEF00Du);
        D2[9] = 0x0BADBEEFu;
        flush_cpu_dcache_range((const void *)&sv[9], 4u);
        check("domain 2 (SRAM) write: in the save's memory", sv[9], 0x0BADBEEFu);
        check("domain 2 repeats every 128 KiB", *(volatile uint32_t *)0xA8020024u, 0x0BADBEEFu);
        /* a read while a long copy runs: it waits, and the copy is right */
        monitor_pidst_write((0x43800000u + 0x40000u - 0x40000000u) >> 2);
        monitor_pisrc_write((SRC + 2u - 0x40000000u) >> 2 | 1u << 24);
        monitor_pilen_write(0x1000u);                               /* 16 KiB */
        uint32_t busy = monitor_pilen_read() >> 31, v = *(volatile uint32_t *)0xB0000008u, after = monitor_pilen_read();
        check("a ROM read during a copy: the copy was running", busy, 1u);
        check("... the read waits for its end", after, 0u);
        check("... and reads right", v, 0xB0000000u + 2u * 0x01010101u);
        pi_wait();
        bad = 0;
        for (uint32_t i = 0; i < 0x4000u; i += 4u)
            if (*(volatile uint32_t *)(uintptr_t)(0xA0040000u + i) != (uint32_t)(rd8(SRC + 2u + i) << 24 | rd8(SRC + 3u + i) << 16 | rd8(SRC + 4u + i) << 8 | rd8(SRC + 5u + i)))
                bad++;
        check("... the copy's words", bad, 0u);
    }

    /* 4: the FlashRAM */
    {
        volatile uint32_t *id2 = (volatile uint32_t *)0xA0000800u;
        /* osFlashInit / osFlashReadId */
        flash_status();
        check("FlashRAM: domain 2 is a FlashRAM after a command", s_d2, D2_FLASH);
        check("... MONITOR.PI2 bit 0", monitor_pi2_read(), 1u);
        cmd(0xE1000000u);
        id2[0] = id2[1] = 0;
        check("silicon ID by the engine", fl_dma(0, 0u, 0x800u, 8u), 1);
        check("silicon ID: type", id2[0], 0x11118001u);
        check("silicon ID: maker (old MX)", id2[1], 0x00C2001Eu);
        check("a read of domain 2: the status word", readio(), 0x11118001u);
        /* osFlashAllErase */
        cmd(0x3C000000u); cmd(0x78000000u);
        check("erase: not busy", readio() & 2u, 0u);
        check("erase: status", readio() & 0xFFu, 0x08u);
        flash_clear();
        /* osFlashReadArray(page 5) */
        volatile uint32_t *pg = (volatile uint32_t *)0xA0000A00u;
        cmd(0xF0000000u); (void)readio();
        for (int i = 0; i < 32; i++) pg[i] = 0;
        check("read array by the engine", fl_dma(0, 5u * 64u, 0xA00u, 128u), 1);
        uint32_t bad = 0;
        for (int i = 0; i < 32; i++) if (pg[i] != 0xFFFFFFFFu) bad++;
        check("erased: page 5 reads 0xFF", bad, 0u);
        /* osFlashWriteBuffer + osFlashWriteArray(5) */
        volatile uint32_t *wb = (volatile uint32_t *)0xA0000C00u;
        uint32_t ref[32];
        for (int i = 0; i < 32; i++) wb[i] = ref[i] = lcg();
        cmd(0xB4000000u);
        check("the page buffer by the engine", fl_dma(1, 0u, 0xC00u, 128u), 1);
        cmd(0xA5000000u | 5u);
        check("program: not busy", readio() & 1u, 0u);
        check("program: status", readio() & 0xFFu, 0x04u);
        flash_clear();
        D2[1] = 0x77777777u;                                           /* (a write of domain 2 goes nowhere) */
        cmd(0xF0000000u); (void)readio();
        for (int i = 0; i < 32; i++) pg[i] = 0;
        fl_dma(0, 5u * 64u, 0xA00u, 128u);
        bad = 0;
        for (int i = 0; i < 32; i++) if (pg[i] != ref[i]) bad++;
        check("page 5 reads what was written", bad, 0u);
        for (int i = 0; i < 32; i++) pg[i] = 0;
        fl_dma(0, 4u * 64u, 0xA00u, 128u);
        bad = 0;
        for (int i = 0; i < 32; i++) if (pg[i] != 0xFFFFFFFFu) bad++;
        check("page 4 still erased", bad, 0u);
        flush_cpu_dcache();
        check("the save's memory: page 5", *(volatile uint32_t *)(uintptr_t)(LAYOUT_SAVE + 5u * 128u), ref[0]);
        check("... domain 2's write went nowhere", *(volatile uint32_t *)(uintptr_t)(LAYOUT_SAVE + 4u), 0xFFFFFFFFu);
        /* a read from a cartridge address 2 mod 4 (devAddr 5 * 64 + 2: the array's page 5 + 4 bytes) */
        for (int i = 0; i < 40; i++) pg[i] = 0;
        fl_dma(0, 5u * 64u + 2u, 0xA00u, 124u);
        bad = 0;
        for (int i = 0; i < 31; i++) if (pg[i] != ref[i + 1]) bad++;
        check("a FlashRAM read at devAddr 2 mod 4", bad, 0u);
        /* osFlashSectorErase(5) */
        cmd(0x4B000000u | 5u); cmd(0x78000000u);
        (void)readio(); (void)readio();
        flash_clear();
        cmd(0xF0000000u); (void)readio();
        fl_dma(0, 5u * 64u, 0xA00u, 128u);
        bad = 0;
        for (int i = 0; i < 32; i++) if (pg[i] != 0xFFFFFFFFu) bad++;
        check("sector erase: page 5 erased", bad, 0u);
        cmd(0xE1000000u);
        check("a DMA in ID mode at another offset: the engine leaves it", d2_engine_addr(0, 0x40u, 8u), 0u);
        check("... and one into a FlashRAM not in page program mode", d2_engine_addr(1, 0u, 128u), 0u);
    }

    /* 5: the game's own accesses */
    monitor_pi2_write(0u);
    {
        volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
        for (int i = 0; i < 8; i++) mr[i] = 0;
        for (int i = 0; i < 16; i++) RES[i] = 0;
        volatile uint32_t *d = (volatile uint32_t *)0xA0001000u;
        for (const uint32_t *p = __start_pi2game; p < __stop_pi2game; p++) *d++ = *p;
        flush_cpu_dcache();
        flush_cpu_icache();
        monitor_ctl_write(5u);
        uint32_t m = 0x302u;
        __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(m));
        typedef void (*game_t)(void);
        ((game_t)0x80001000u)();
        __asm__ volatile(".set push\n.set mips3\n li $t0, 0x200\n mtc0 $t0, $7\n.set pop" ::: "t0");
        monitor_ctl_write(0u);
        check("game: ROM word 0", RES[0], 0xB0000000u);
        check("game: ROM word 1", RES[1], 0xB1010101u);
        check("game: ROM word 2 (KSEG0)", RES[2], 0xB2020202u);
        check("game: ROM at 48 MiB - 4", RES[3], 0x48484848u);
        check("game: domain 2 written and read", RES[4], 0x12345678u);
        check("game: domain 2, KSEG0", RES[5], *(volatile uint32_t *)(uintptr_t)(LAYOUT_SAVE + 0x44u));
        check("game: the half-word load was the monitor's (rt left)", RES[6], 0xDEADu);
        check("game: a read while a write to the cartridge goes on is its word", RES[8], 0xCAFEF00Du);
        check("... a second write is ignored: the read after it is still the first's word", RES[9], 0xCAFEF00Du);
        check("... and after it, the ROM's word again", RES[10], RES[11]);
        check("game: ran to its end", RES[7], 1u);
        check("monitor entries: the lh and the command", mr[2], 2u);
        check("... a load", mr[3], 1u);
        check("... a store", mr[4], 1u);
        check("... nothing else", mr[5], 0u);
    }
    puts_(fails ? "pi2_test: FAILED\n" : "pi2_test: PASS\n");
    putc_(4);
    for (;;);
}
