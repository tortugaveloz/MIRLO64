/* Runs RSP tasks dumped from the sim (sim/mips_soc RSPDUMP: the RDRAM and the
 * OSTask's address, as osSpTaskStartGo() got it) on the reference model, the
 * way the N64 starts one -- the task's header to DMEM 0xFC0 with physical
 * addresses, its boot microcode to IMEM, the PC at its start -- and reports
 * what the RSP did: instructions per kind, DMA traffic, RDP commands.
 *
 *   rsp_task [-o out_prefix] [-q] task_*.bin ...
 *     -o: for each task (n = its place in the list) <prefix>_n.rdp (the RDP's
 *         command words, big-endian), <prefix>_n.ram (the RDRAM after the
 *         task) and <prefix>_n.dmem (the DMEM after it)
 *     -q: only the totals over all the tasks given
 *     -t: also the cycles the task takes on the console (rsp_time.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsp.h"
#include "rsp_time.h"

#define RDRAM_SIZE (8u << 20)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static FILE *rdp_out;
static uint64_t rdp_ops[64];
static void on_rdp(void *user, const uint32_t *w, int n)
{
    (void)user;
    rdp_ops[(w[0] >> 24) & 63u]++;
    if (rdp_out)
        for (int i = 0; i < 2 * n; i++) { uint8_t b[4]; wbe32(b, w[i]); fwrite(b, 1, 4, rdp_out); }
}

/* (-t) where the console's cycles go, by kind -- as tb_rsp's KINDS counts the RTL's: a slot's cycles to its first instruction */
enum { K_SCALAR, K_LOAD, K_BRANCH, K_VMUL, K_VOTHER, K_VRCP, K_VLOAD, K_VSTORE, K_C2MOVE, K_COP0, K_DMA, K_N };
static const char *kname[K_N] = { "scalar", "scalar load", "branch/jump", "vector multiply", "vector other", "vector reciprocal",
                                  "vector load", "vector store", "MFC2/MTC2/CFC2/CTC2", "MFC0/MTC0", "DMA (MTC0)" };
static double kcyc[K_N], kins[K_N];
static int kind_of(uint32_t in)
{
    unsigned op = in >> 26, fn = in & 63u;
    if (op == 0x32) return K_VLOAD;
    if (op == 0x3A) return K_VSTORE;
    if (op == 0x12) {
        if (!(in & (1u << 25))) return K_C2MOVE;
        if (fn < 0x10) return K_VMUL;
        if (fn >= 0x30 && fn <= 0x36 && fn != 0x33) return K_VRCP;
        return K_VOTHER;
    }
    if (op == 0x10) return ((in >> 21) & 31u) == 4 && (((in >> 11) & 15u) == 2 || ((in >> 11) & 15u) == 3) ? K_DMA : K_COP0;
    if (op >= 0x20 && op <= 0x27) return K_LOAD;
    if ((op >= 1 && op <= 7) || (op == 0 && (fn == 8 || fn == 9))) return K_BRANCH;
    return K_SCALAR;
}

static uint64_t tot_prof[RSP_P_N], tot_insts, tot_dma_rd, tot_dma_wr, tot_cmds, tot_unimpl;
static uint64_t tot_plain[2][16], tot_e[16], tot_vv;
static int tot_tasks;

static void report(const char *title, const uint64_t *prof, uint64_t insts, uint64_t dma_rd, uint64_t dma_wr, uint64_t cmds, uint64_t unimpl)
{
    uint64_t vec = 0, ld = 0, st = 0;
    for (int i = 0; i < 64; i++) vec += prof[i];
    for (int i = 64; i < 80; i++) ld += prof[i];
    for (int i = 80; i < 96; i++) st += prof[i];
    printf("%s: %llu instructions; vector ops %.1f %%, vector loads %.1f %%, vector stores %.1f %%; DMA %llu B in, %llu B out; %llu RDP commands%s\n",
           title, (unsigned long long)insts, 100.0 * vec / insts, 100.0 * ld / insts, 100.0 * st / insts,
           (unsigned long long)dma_rd, (unsigned long long)dma_wr, (unsigned long long)cmds, unimpl ? "; UNIMPLEMENTED ops ran" : "");
    /* sorted by count */
    int idx[RSP_P_N];
    for (int i = 0; i < RSP_P_N; i++) idx[i] = i;
    for (int i = 0; i < RSP_P_N; i++)
        for (int j = i + 1; j < RSP_P_N; j++)
            if (prof[idx[j]] > prof[idx[i]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (int i = 0; i < RSP_P_N && prof[idx[i]]; i++)
        printf("  %-7s %10llu  %5.2f %%\n", rsp_prof_name(idx[i]), (unsigned long long)prof[idx[i]], 100.0 * prof[idx[i]] / insts);
}

int main(int argc, char **argv)
{
    const char *prefix = 0;
    int quiet = 0, timed = 0, ai = 1;
    uint64_t tot_cyc = 0;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        if (!strcmp(argv[ai], "-o") && ai + 1 < argc) prefix = argv[++ai];
        else if (!strcmp(argv[ai], "-q")) quiet = 1;
        else if (!strcmp(argv[ai], "-t")) timed = 1;
        else { fprintf(stderr, "usage: rsp_task [-o out_prefix] [-q] [-t] task.bin ...\n"); return 2; }
    }
    static uint8_t ram[RDRAM_SIZE];
    static rsp_t s;
    for (; ai < argc; ai++) {
        FILE *f = fopen(argv[ai], "rb");
        uint8_t hdr[16];
        if (!f || fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "RSPT", 4) || fread(ram, 1, RDRAM_SIZE, f) != RDRAM_SIZE) {
            fprintf(stderr, "%s: not a task dump\n", argv[ai]); return 1;
        }
        fclose(f);
        uint32_t ta = be32(hdr + 4) & (RDRAM_SIZE - 1u);
        rsp_reset(&s, ram, RDRAM_SIZE);
        s.rdp_cmd = on_rdp;
        /* osSpTaskLoad: the header with its pointers physical ... */
        uint8_t task[64];
        memcpy(task, ram + ta, 64);
        static const int ptrs[] = { 2, 4, 6, 8, 10, 11, 12, 14 };   /* ucode_boot, ucode, ucode_data, dram_stack, output_buff(+_size), data_ptr, yield_data_ptr */
        for (unsigned i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++) wbe32(task + 4 * ptrs[i], be32(task + 4 * ptrs[i]) & 0x1FFFFFFFu);
        memcpy(s.dmem + 0xFC0, task, 64);
        /* ... and the boot microcode at IMEM's start, where the PC begins */
        uint32_t boot = be32(task + 8) & (RDRAM_SIZE - 1u), boot_size = be32(task + 12);
        if (boot_size > 4096) boot_size = 4096;
        memcpy(s.imem, ram + boot, boot_size);
        s.pc = 0;
        char path[1024];
        if (prefix) { snprintf(path, sizeof path, "%s_%d.rdp", prefix, tot_tasks); rdp_out = fopen(path, "wb"); }
        int r;
        rsp_time_t tm;
        if (timed) {
            rsp_time_reset(&tm);
            s.status &= ~1u;
            r = -1;
            while (s.insts < 200000000ull) {
                uint64_t c0 = tm.now, i0 = s.insts;
                uint32_t pc0 = s.pc & 0xFFCu;
                int end = rsp_time_slot(&tm, &s);
                for (uint64_t k = 0; k < s.insts - i0; k++) {
                    uint32_t a = (pc0 + 4u * (uint32_t)k) & 0xFFCu;
                    uint32_t in = (uint32_t)s.imem[a] << 24 | s.imem[a + 1] << 16 | s.imem[a + 2] << 8 | s.imem[a + 3];
                    kins[kind_of(in)]++;
                    if (!k) kcyc[kind_of(in)] += (double)(tm.now - c0);
                }
                if (end) { r = 0; break; }
            }
            printf("%s: console %llu cycles (%.3f ms at 62.5 MHz), %llu instructions, %llu pairs, %llu stall cycles, %llu bubbles, %llu cycles with a DMA running\n",
                   argv[ai], (unsigned long long)tm.now, tm.now / 62500.0, (unsigned long long)s.insts, (unsigned long long)tm.pairs,
                   (unsigned long long)tm.stalls, (unsigned long long)tm.bubbles, (unsigned long long)tm.dma_wait);
            tot_cyc += tm.now;
        } else r = rsp_run(&s, 200000000ull);
        if (rdp_out) { fclose(rdp_out); rdp_out = 0; }
        if (prefix) {
            snprintf(path, sizeof path, "%s_%d.ram", prefix, tot_tasks);
            if ((f = fopen(path, "wb"))) { fwrite(ram, 1, RDRAM_SIZE, f); fclose(f); }
            snprintf(path, sizeof path, "%s_%d.dmem", prefix, tot_tasks);
            if ((f = fopen(path, "wb"))) { fwrite(s.dmem, 1, 4096, f); fclose(f); }
        }
        if (!quiet || r) {
            char title[1200];
            snprintf(title, sizeof title, "%s (type %u, ucode %06x, %s, SP_STATUS %04x)", argv[ai], be32(task), be32(task + 16),
                     r ? "DID NOT END" : "ended", s.status);
            report(title, s.prof, s.insts, s.dma_rd_bytes, s.dma_wr_bytes, s.rdp_cmds, s.unimpl);
        }
        for (int i = 0; i < RSP_P_N; i++) tot_prof[i] += s.prof[i];
        for (int i = 0; i < 16; i++) { tot_plain[0][i] += s.ls_plain[0][i]; tot_plain[1][i] += s.ls_plain[1][i]; tot_e[i] += s.vec_e[i]; }
        tot_vv += s.vec_after_vec;
        tot_insts += s.insts; tot_dma_rd += s.dma_rd_bytes; tot_dma_wr += s.dma_wr_bytes; tot_cmds += s.rdp_cmds; tot_unimpl += s.unimpl;
        tot_tasks++;
    }
    if (tot_tasks > 1 || quiet) {
        char title[64];
        snprintf(title, sizeof title, "TOTAL of %d tasks", tot_tasks);
        report(title, tot_prof, tot_insts, tot_dma_rd, tot_dma_wr, tot_cmds, tot_unimpl);
        if (timed) {
            printf("  console: %llu cycles\n", (unsigned long long)tot_cyc);
            for (int k = 0; k < K_N; k++)
                if (kins[k]) printf("  %-22s %9.0f instructions, %5.2f cycles each, %4.1f %% of the cycles\n", kname[k], kins[k],
                                    kcyc[k] / kins[k], 100.0 * kcyc[k] / (double)tot_cyc);
        }
        /* what a lite RSP may do in several cycles: the loads/stores that are not "plain" */
        printf("  vector loads/stores, plain (element 0, aligned) / all:");
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < 12; i++)
                if (tot_prof[(k ? RSP_P_SWC2 : RSP_P_LWC2) + i])
                    printf(" %s %llu/%llu", rsp_prof_name((k ? RSP_P_SWC2 : RSP_P_LWC2) + i), (unsigned long long)tot_plain[k][i],
                           (unsigned long long)tot_prof[(k ? RSP_P_SWC2 : RSP_P_LWC2) + i]);
        uint64_t en = tot_e[0] + tot_e[1], eq = tot_e[2] + tot_e[3], eh = tot_e[4] + tot_e[5] + tot_e[6] + tot_e[7], e1 = 0, ev;
        for (int i = 8; i < 16; i++) e1 += tot_e[i];
        ev = en + eq + eh + e1;
        printf("\n  vector ops by vt element: whole %.1f %%, quarters %.1f %%, halves %.1f %%, one lane %.1f %%; vector instructions right after another: %.1f %% of all instructions\n",
               ev ? 100.0 * en / ev : 0, ev ? 100.0 * eq / ev : 0, ev ? 100.0 * eh / ev : 0, ev ? 100.0 * e1 / ev : 0, 100.0 * tot_vv / tot_insts);
        printf("  RDP commands by opcode:");
        for (int i = 0; i < 64; i++) if (rdp_ops[i]) printf(" %02x:%llu", i, (unsigned long long)rdp_ops[i]);
        printf("\n");
    }
    return 0;
}
