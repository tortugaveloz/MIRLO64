/* A directed TLB program for the CPU's lockstep bench (sim/mips/build.sh; tb rom.z64). The bench makes the model (sim/n64)
 * and the RTL agree on every retired instruction's register write and on every exception's code and vector, so what this
 * program has to do is reach the cases: the pages of every size (4 KiB .. 16 MiB), odd and even, the ASID and the global
 * bit, a page that is invalid or clean, a refill and a fault in a delay slot, with EXL set, a fault of a fetch, tlbr /
 * tlbp / tlbwi / tlbwr and Random / Wired -- and read every COP0 register the handler is given (mfc0 writes a register the
 * bench compares). tools: sim/mips/tlb_rom.sh builds it.
 *
 * The exception handler (h_entry, copied to 0x80000000 and 0x80000180) reads the registers, then by h_mode:
 *   0  continues at h_ret
 *   1  writes the entry h_idx (0xFF: tlbwr) from h_pm / h_hi / h_lo0 / h_lo1 and retries the instruction
 *   2  repairs: tlbp, then tlbwi of the entry found with h_lo0 / h_lo1 (the page pair made valid / dirty), and retries
 */
typedef unsigned int u32;
#define MFC0(r) ({ u32 v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_) :: "memory"); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((u32)(v)) : "memory")
#define TLBWI() __asm__ volatile("tlbwi" ::: "memory")
#define TLBWR() __asm__ volatile("tlbwr" ::: "memory")
#define TLBR()  __asm__ volatile("tlbr" ::: "memory")
#define TLBP()  __asm__ volatile("tlbp" ::: "memory")
#define K0(pa) ((volatile u32 *)(0x80000000u | (pa)))
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))

volatile u32 h_mode, h_ret, h_idx, h_pm, h_hi, h_lo0, h_lo1, h_count;
volatile u32 h_save[8];
volatile u32 sink;                  /* what the program reads, kept */

/* EntryLo for a physical address with C, D, V, G */
#define LO(pa, c, d, v, g) ((((u32)(pa) >> 12) << 6) | ((c) << 3) | ((d) << 2) | ((v) << 1) | (g))
#define CV (3u)                     /* cached */
#define UC (2u)                     /* uncached */

__asm__(
"    .section .text.start,\"ax\"\n"
"    .set noreorder\n"
"    .globl _start\n"
"_start:\n"
"    lui   $sp, 0x800F\n"
"    ori   $sp, $sp, 0xF000\n"
"    jal   main\n"
"    nop\n"
"1:  b     1b\n"
"    nop\n"
"    .text\n"
"    .set noat\n"
"    .globl h_entry\n"
"h_entry:\n"
"    lui   $k0, %hi(h_save)\n"
"    addiu $k0, $k0, %lo(h_save)\n"
"    sw    $at, 0($k0)\n"
"    sw    $v0, 4($k0)\n"
"    sw    $v1, 8($k0)\n"
"    sw    $a0, 12($k0)\n"
"    sw    $a1, 16($k0)\n"
"    mfc0  $v1, $13\n"                   /* Cause */
"    mfc0  $v0, $8\n"                    /* BadVAddr */
"    srl   $v0, $v1, 2\n"                /* Context: a TLB exception's (the VR4300's address errors set it too: the model's do not) */
"    andi  $v0, $v0, 31\n"
"    addiu $v0, $v0, -1\n"
"    sltiu $v0, $v0, 3\n"
"    beqz  $v0, 7f\n"
"    nop\n"
"    mfc0  $v0, $4\n"
"7:\n"
"    mfc0  $v0, $10\n"                   /* EntryHi */
"    mfc0  $v0, $14\n"                   /* EPC */
"    mfc0  $v0, $12\n"                   /* Status */
"    lui   $k1, %hi(h_count)\n"
"    lw    $v0, %lo(h_count)($k1)\n"
"    addiu $v0, $v0, 1\n"
"    sw    $v0, %lo(h_count)($k1)\n"
"    lui   $k1, %hi(h_mode)\n"
"    lw    $v1, %lo(h_mode)($k1)\n"
"    beqz  $v1, 9f\n"
"    nop\n"
"    addiu $v1, $v1, -1\n"
"    beqz  $v1, 2f\n"
"    nop\n"
/* mode 2: repair the entry the faulting address names */
"    tlbp\n"
"    mfc0  $v0, $0\n"
"    lui   $k1, %hi(h_lo0)\n"
"    lw    $a0, %lo(h_lo0)($k1)\n"
"    lw    $a1, %lo(h_lo1)($k1)\n"
"    mtc0  $a0, $2\n"
"    mtc0  $a1, $3\n"
"    tlbr\n"                             /* (the PageMask and EntryHi the entry has) */
"    mtc0  $a0, $2\n"
"    mtc0  $a1, $3\n"
"    mtc0  $v0, $0\n"
"    tlbwi\n"
"    b     8f\n"
"    nop\n"
/* mode 1: write the entry */
"2:\n"
"    lui   $k1, %hi(h_idx)\n"
"    lw    $a0, %lo(h_idx)($k1)\n"
"    mtc0  $a0, $0\n"
"    lw    $a0, %lo(h_pm)($k1)\n"
"    mtc0  $a0, $5\n"
"    lw    $a0, %lo(h_hi)($k1)\n"
"    mtc0  $a0, $10\n"
"    lw    $a0, %lo(h_lo0)($k1)\n"
"    mtc0  $a0, $2\n"
"    lw    $a0, %lo(h_lo1)($k1)\n"
"    mtc0  $a0, $3\n"
"    lw    $a0, %lo(h_idx)($k1)\n"
"    addiu $a0, $a0, -0xFF\n"
"    beqz  $a0, 3f\n"
"    nop\n"
"    tlbwi\n"
"    b     8f\n"
"    nop\n"
"3:  tlbwr\n"
"    b     8f\n"
"    nop\n"
/* mode 0 */
"9:\n"
"    lui   $k1, %hi(h_ret)\n"
"    lw    $a0, %lo(h_ret)($k1)\n"
"    mtc0  $a0, $14\n"
"8:\n"
"    lui   $k0, %hi(h_save)\n"
"    addiu $k0, $k0, %lo(h_save)\n"
"    lw    $at, 0($k0)\n"
"    lw    $v0, 4($k0)\n"
"    lw    $v1, 8($k0)\n"
"    lw    $a0, 12($k0)\n"
"    lw    $a1, 16($k0)\n"
"    eret\n"
"    nop\n"
"    .set reorder\n"
"    .set at\n"
);
extern void h_entry(void);

static void tlbwi_(u32 idx, u32 pm, u32 hi, u32 lo0, u32 lo1)
{
    MTC0(0, idx); MTC0(5, pm); MTC0(10, hi); MTC0(2, lo0); MTC0(3, lo1);
    TLBWI();
}
static void setup(u32 mode, u32 ret, u32 idx, u32 pm, u32 hi, u32 lo0, u32 lo1)
{
    h_mode = mode; h_ret = ret; h_idx = idx; h_pm = pm; h_hi = hi; h_lo0 = lo0; h_lo1 = lo1;
}
static void unmap_all(void)
{
    for (u32 i = 0; i < 32; i++) tlbwi_(i, 0, 0x80000000u + (i << 13), 0, 0);
}
static void probe(u32 hi)
{
    MTC0(10, hi); TLBP();
    sink = MFC0(0);
}
static void dump_entry(u32 idx)
{
    MTC0(0, idx); TLBR();
    sink = MFC0(5); sink = MFC0(10); sink = MFC0(2); sink = MFC0(3);
}

static const u32 pmtab[7] = { 0x0, 0x6000, 0x1E000, 0x7E000, 0x1FE000, 0x7FE000, 0x1FFE000 };

/* the pages of every size: a refill (the handler writes the entry), then hits; loads and stores at the edges of both pages */
static void sizes(u32 asid, u32 global)
{
    for (u32 k = 0; k < 7; k++) {
        u32 span = 0x1000u << (2 * k);
        u32 va = 0x20000000u + (k << 26);
        u32 base = k == 6 ? 0x10000000u : k == 5 ? 0u : 0x00400000u;
        u32 even = base, odd = base + span;
        u32 hi = va | asid;
        u32 offs[4] = { 0, 4, span - 4, span / 2 };
        for (int j = 0; j < 4; j++) {
            if (k == 6) continue;
            u32 o = offs[j];
            /* (keep out of the program: the 4 MiB case's first page starts at 0; use the far end of each page) */
            if (k == 5 && j != 2) continue;
            *K1(even + o) = 0xE0000000u | (k << 24) | o;
            *K1(odd + o) = 0x0D000000u | (k << 24) | o;
        }
        /* a refill on a load of the even page, the entry written by the handler (tlbwr), then the rest hit */
        setup(1, 0, 0xFF, pmtab[k], hi, LO(even, CV, 1, 1, global), LO(odd, CV, 1, 1, global));
        MTC0(10, asid);
        for (int j = 0; j < 4; j++) {
            u32 o = offs[j];
            if (k == 6) { sink = *(volatile u32 *)(va + o); sink = *(volatile u32 *)(va + span + o); continue; }
            if (k == 5 && j != 2) continue;
            sink = *(volatile u32 *)(va + o);
            sink = *(volatile u32 *)(va + span + o);
            *(volatile u32 *)(va + o) = 0x5A5A0000u | (k << 8) | j;                /* a store through the even page ... */
            sink = *K1(even + o);                                                  /* ... seen through the physical */
            *(volatile u32 *)(va + span + o) = 0xA5A50000u | (k << 8) | j;
            sink = *K1(odd + o);
        }
        h_mode = 0;
        probe(hi);
        dump_entry(sink & 31);
        if (pmtab[k] == 0x1FFE000u) { MTC0(0, 5); TLBR(); sink = MFC0(5); }
    }
}

/* ---- a fetch through the TLB */
static void code_at(u32 pa, u32 value)
{
    *K1(pa) = 0x24020000u | (value & 0xFFFF);                  /* addiu v0, zero, value */
    *K1(pa + 4) = 0x03E00008u;                                 /* jr ra */
    *K1(pa + 8) = 0;
}
typedef u32 (*fn_t)(void);

int main(void)
{
    /* the vectors */
    for (u32 v = 0; v < 0x200; v += 0x180) {
        u32 a = (u32)(void *)h_entry;
        *K1(v) = 0x3C1A0000u | (a >> 16);                      /* lui k0, hi */
        *K1(v + 4) = 0x375A0000u | (a & 0xFFFF);               /* ori k0, k0, lo */
        *K1(v + 8) = 0x03400008u;                              /* jr k0 */
        *K1(v + 12) = 0;
    }
    unmap_all();
    MTC0(6, 0);
    MTC0(4, 0xFF800000u & 0x80800000u);                        /* PTEBase */

    /* ---- the sizes: global, and an ASID-tagged entry */
    sizes(0, 1);
    unmap_all();
    sizes(5, 0);
    unmap_all();

    /* ---- the ASID: an entry of ASID 5 maps the page for it alone; another ASID takes a refill */
    {
        u32 va = 0x30000000u;
        *K1(0x00500000) = 0xA5100000u; *K1(0x00501000) = 0xA5110000u;
        tlbwi_(4, 0, va | 5u, LO(0x00500000, CV, 1, 1, 0), LO(0x00501000, CV, 1, 1, 0));
        MTC0(10, 5);
        sink = *(volatile u32 *)va; sink = *(volatile u32 *)(va + 0x1000);
        probe(va | 5u); probe(va | 6u); probe(va | 0x1000u | 5u);
        MTC0(10, 6);
        { u32 r = 0x1234; setup(0, (u32)&&L_asid, 0, 0, 0, 0, 0); r = *(volatile u32 *)va; L_asid: sink = r; }
        { u32 r = 0x1234; setup(0, (u32)&&L_asid2, 0, 0, 0, 0, 0); *(volatile u32 *)(va + 0x1000) = r; L_asid2: sink = r; }
        /* tlbr loads EntryHi, ASID included: the page of ASID 5 no longer maps, though it was just translated */
        tlbwi_(10, 0, 0x30200000u | 7u, LO(0x00500000, CV, 1, 1, 0), LO(0x00501000, CV, 1, 1, 0));
        MTC0(10, 5);
        sink = *(volatile u32 *)va;
        MTC0(0, 10); TLBR();
        sink = MFC0(10);
        { u32 r = 0x1234; setup(0, (u32)&&L_asid3, 0, 0, 0, 0, 0); r = *(volatile u32 *)va; L_asid3: sink = r; }
        MTC0(10, 0);
        unmap_all();
    }

    /* ---- invalid, clean, and both */
    {
        u32 va = 0x30400000u;
        *K1(0x00500000) = 0xA5100000u; *K1(0x00501000) = 0xA5110000u;
        /* even: V clear; odd: valid, clean */
        tlbwi_(7, 0, va, LO(0x00500000, CV, 1, 0, 1), LO(0x00501000, CV, 0, 1, 1));
        { u32 r = 7; setup(0, (u32)&&L_i1, 0, 0, 0, 0, 0); r = *(volatile u32 *)va; L_i1: sink = r; }
        { setup(0, (u32)&&L_i2, 0, 0, 0, 0, 0); *(volatile u32 *)va = 5; L_i2: ; }
        sink = *(volatile u32 *)(va + 0x1000);                                  /* odd page: readable */
        { setup(0, (u32)&&L_i3, 0, 0, 0, 0, 0); *(volatile u32 *)(va + 0x1000) = 5; L_i3: ; }     /* clean: Mod */
        /* invalid and clean: TLBS (not Mod) for a store */
        tlbwi_(7, 0, va, LO(0x00500000, CV, 0, 0, 1), LO(0x00501000, CV, 0, 1, 1));
        { setup(0, (u32)&&L_i4, 0, 0, 0, 0, 0); *(volatile u32 *)va = 5; L_i4: ; }
        tlbwi_(7, 0, va, LO(0x00500000, CV, 1, 0, 1), LO(0x00501000, CV, 0, 1, 1));
        /* an invalid page repaired by the handler: the entry rewritten valid and dirty, the access retried */
        setup(2, 0, 0, 0, 0, LO(0x00500000, CV, 1, 1, 1), LO(0x00501000, CV, 1, 1, 1));
        sink = *(volatile u32 *)va;
        setup(2, 0, 0, 0, 0, LO(0x00500000, CV, 1, 1, 1), LO(0x00501000, CV, 1, 1, 1));
        tlbwi_(7, 0, va, LO(0x00500000, CV, 1, 1, 1), LO(0x00501000, CV, 0, 1, 1));
        *(volatile u32 *)(va + 0x1000) = 0x77u;                                 /* Mod, repaired, retried */
        sink = *K1(0x00501000);
        h_mode = 0;
        unmap_all();
    }

    /* ---- a refill, an invalid page and a clean one in a branch's delay slot (EPC the branch, BD) */
    {
        u32 va = 0x30800000u, r = 1;
        *K1(0x00500000) = 0xA5100000u;
        setup(1, 0, 0xFF, 0, va, LO(0x00500000, CV, 1, 1, 1), LO(0x00501000, CV, 1, 1, 1));
        __asm__ volatile(".set push\n.set noreorder\n"
                         "    b 1f\n"
                         "    lw %0, 0(%1)\n"
                         "    addiu %0, %0, 100\n"
                         "1:  addiu %0, %0, 1\n"
                         ".set pop" : "+r"(r) : "r"(va));
        sink = r;
        h_mode = 0;
        r = 2;
        probe(va);
        tlbwi_(8, 0, va, LO(0x00500000, CV, 1, 0, 1), LO(0x00501000, CV, 1, 1, 1));
        setup(0, (u32)&&L_d1, 0, 0, 0, 0, 0);
        __asm__ volatile(".set push\n.set noreorder\n"
                         "    b 2f\n"
                         "    lw %0, 0(%1)\n"
                         "2:  \n"
                         ".set pop" : "+r"(r) : "r"(va));
    L_d1:
        sink = r;
        unmap_all();
    }

    /* ---- a refill with EXL set: the general vector, EPC and BD left alone */
    {
        u32 va = 0x30C00000u, r = 3;
        *K1(0x00500000) = 0xA5100000u;
        setup(0, (u32)&&L_x1, 0, 0, 0, 0, 0);
        MTC0(14, 0x80001234u);
        MTC0(12, MFC0(12) | 2u);
        r = *(volatile u32 *)va;
    L_x1:
        sink = r; sink = MFC0(14); sink = MFC0(12);
        MTC0(12, MFC0(12) & ~2u);
        unmap_all();
    }

    /* ---- tlbr / tlbp / Index / Random / Wired */
    {
        tlbwi_(3, 0x6000, 0x34000000u | 9u, LO(0x00400000, CV, 1, 1, 0), LO(0x00404000, UC, 0, 1, 1));
        dump_entry(3);
        dump_entry(4);
        probe(0x34000000u | 9u); probe(0x34000000u | 10u); probe(0x34004000u | 9u); probe(0x34002000u | 9u);
        MTC0(6, 8); sink = MFC0(6);
        for (int i = 0; i < 6; i++) {
            MTC0(5, 0); MTC0(10, 0x38000000u + (i << 13)); MTC0(2, LO(0x00400000, CV, 1, 1, 1)); MTC0(3, LO(0x00402000, CV, 1, 1, 1));
            TLBWR();
            probe(0x38000000u + (i << 13));
            sink = MFC0(1);
        }
        MTC0(6, 0);
        tlbwi_(33, 0, 0x3C000000u, LO(0x00400000, CV, 1, 1, 1), LO(0x00402000, CV, 1, 1, 1));    /* (Index 33: entry 1) */
        probe(0x3C000000u);
        unmap_all();
    }

    /* ---- fetch */
    {
        u32 va = 0x40000000u;
        code_at(0x00600000, 0x6001); code_at(0x00601000, 0x6002);
        code_at(0x00602000, 0x6003);
        /* a refill of a fetch: the handler writes the pair; the code runs; the next fetch of the odd page hits */
        setup(1, 0, 0xFF, 0, va, LO(0x00600000, CV, 1, 1, 1), LO(0x00601000, CV, 1, 1, 1));
        sink = ((fn_t)va)();
        sink = ((fn_t)(va + 0x1000))();
        sink = ((fn_t)va)();
        h_mode = 0;
        /* an invalid page: TLBL on the fetch, BadVAddr the target, the handler continues past the call */
        tlbwi_(2, 0, va + 0x4000, LO(0x00600000, CV, 1, 0, 1), LO(0x00601000, CV, 1, 1, 1));
        {
            u32 r = 9;
            setup(0, (u32)&&L_f1, 0, 0, 0, 0, 0);
            __asm__ volatile(".set push\n.set noreorder\n"
                             "    jalr %1\n"
                             "    nop\n"
                             ".set pop" : "+r"(r) : "r"(va + 0x4000) : "ra", "v0", "memory");
        L_f1:
            sink = r;
        }
        /* straight-line code over a page boundary (the last word of one page, the first of the next, both frames) */
        *K1(0x00604000 - 8) = 0x24020000u | 0x1111;                             /* addiu v0, zero, 0x1111 */
        *K1(0x00604000 - 4) = 0x24420001u;                                      /* addiu v0, v0, 1 */
        *K1(0x00606000) = 0x24420002u;                                          /* addiu v0, v0, 2 (the odd page's frame) */
        *K1(0x00606004) = 0x03E00008u; *K1(0x00606008) = 0;
        tlbwi_(2, 0, va + 0x10000, LO(0x00604000 - 0x1000, CV, 1, 1, 1), LO(0x00606000, CV, 1, 1, 1));
        /* (the pair's even frame ends at 0x00604000-0x1000+0x1000 = 0x00604000; the code at its end falls into the odd page) */
        *K1(0x00604000 - 8) = 0x24020000u | 0x1111;
        sink = ((fn_t)(va + 0x10000 + 0x1000 - 8))();
        sink = ((fn_t)(va + 0x10000 + 0x1000 - 8))();
        unmap_all();
        /* a fetch from a big page, and a fetch after the entry was replaced */
        tlbwi_(5, 0x1E000, va + 0x100000, LO(0x00610000, CV, 1, 1, 1), LO(0x00620000, CV, 1, 1, 1));
        code_at(0x00615000, 0x7001);
        sink = ((fn_t)(va + 0x100000 + 0x5000))();
        sink = ((fn_t)(va + 0x100000 + 0x5000))();
        code_at(0x00625000, 0x7002);
        sink = ((fn_t)(va + 0x100000 + 0x10000 + 0x5000))();
        tlbwi_(5, 0x1E000, va + 0x100000, LO(0x00630000, CV, 1, 1, 1), LO(0x00620000, CV, 1, 1, 1));
        code_at(0x00635000, 0x7003);
        sink = ((fn_t)(va + 0x100000 + 0x5000))();
        /* a tlbwi at the end of a page that changes the next page's frame: the fetch behind it (a search under way for the new page
         * while the tlbwi executes) is of the new frame */
        {
            u32 pa = 0x00640000, pb1 = 0x00642000, pb2 = 0x00644000, vp = va + 0x200000;
            for (u32 i = 0; i < 0x1000; i += 4) *K1(pa + i) = 0;                        /* nops */
            *K1(pa + 0xFFC) = 0x42000002u;                                                /* tlbwi */
            *K1(pb1) = 0x24020111u; *K1(pb1 + 4) = 0x03E00008u; *K1(pb1 + 8) = 0;
            *K1(pb2) = 0x24020222u; *K1(pb2 + 4) = 0x03E00008u; *K1(pb2 + 8) = 0;
            for (u32 ix = 0; ix < 16; ix++)
                for (u32 r = 0; r < 8; r++) {
                    unmap_all();
                    tlbwi_(ix, 0, vp, LO(pa, CV, 1, 1, 1), LO(pb1, CV, 1, 1, 1));
                    MTC0(0, ix); MTC0(5, 0); MTC0(10, vp); MTC0(2, LO(pa, CV, 1, 1, 1)); MTC0(3, LO(r & 1 ? pb1 : pb2, CV, 1, 1, 1));
                    sink = ((fn_t)(vp + 0xFFC - 4 * r))();
                    sink = ((fn_t)(vp + 0x1000))();                                       /* (the micro-TLB must not hold the old frame) */
                }
            unmap_all();
        }
        /* an uncached page (C = 2) */
        tlbwi_(5, 0x1E000, va + 0x100000, LO(0x00630000, UC, 1, 1, 1), LO(0x00620000, UC, 1, 1, 1));
        sink = ((fn_t)(va + 0x100000 + 0x5000))();
        unmap_all();
    }

    /* ---- the top of KUSEG and KSEG2 (mapped by the model and the non-FLAT core alike) */
    {
        *K1(0x00700000) = 0xC0DE0001u; *K1(0x00701000) = 0xC0DE0002u;
        setup(1, 0, 0xFF, 0, 0x7FFFE000u, LO(0x00700000, CV, 1, 1, 1), LO(0x00701000, CV, 1, 1, 1));
        sink = *(volatile u32 *)0x7FFFE000u;
        sink = *(volatile u32 *)0x7FFFFFFCu;
        setup(1, 0, 0xFF, 0, 0xC0000000u, LO(0x00700000, CV, 1, 1, 1), LO(0x00701000, CV, 1, 1, 1));
        sink = *(volatile u32 *)0xC0000000u;
        setup(1, 0, 0xFF, 0, 0xFFFFE000u, LO(0x00700000, CV, 1, 1, 1), LO(0x00701000, CV, 1, 1, 1));
        sink = *(volatile u32 *)0xFFFFE000u;
        h_mode = 0;
        unmap_all();
    }

    /* ---- lwl / lwr / ldc1-less partial words through a page, a misaligned address (AdEL before the TLB) */
    {
        u32 va = 0x50000000u;
        *K1(0x00700000) = 0x01020304u; *K1(0x00700004) = 0x05060708u;
        tlbwi_(9, 0, va, LO(0x00700000, CV, 1, 1, 1), LO(0x00701000, CV, 1, 1, 1));
        u32 r = 0;
        __asm__ volatile("lwl %0, 1(%1)\n lwr %0, 4(%1)" : "+r"(r) : "r"(va));
        sink = r;
        __asm__ volatile("lbu %0, 3(%1)" : "=r"(r) : "r"(va));
        sink = r;
        __asm__ volatile("sb %0, 5(%1)" : : "r"(0x9Au), "r"(va));
        sink = *K1(0x00700004);
        { setup(0, (u32)&&L_m1, 0, 0, 0, 0, 0); __asm__ volatile("lw %0, 2(%1)" : "=r"(r) : "r"(va)); L_m1: sink = r; }
        unmap_all();
    }

    *K1(0x000F0F00) = 0x600DC0DEu;
    for (;;) ;
}
