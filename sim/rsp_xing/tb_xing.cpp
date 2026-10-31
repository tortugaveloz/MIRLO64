// tb_xing's driver: clk_sys at 62.5 MHz and the game CPU's clk_c at 3/2 of it, rising together every two cycles of clk_sys
// (units of 1/6 of a clk_sys cycle: clk_sys has 6 units a cycle, clk_c 4).
#include "Vtb_xing.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    long cycles = 200000; int resets = 0, seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "+cycles=", 8)) cycles = atol(argv[i] + 8);
        if (!strncmp(argv[i], "+reset=", 7)) resets = atoi(argv[i] + 7);
        if (!strncmp(argv[i], "+seed=", 6)) seed = atoi(argv[i] + 6);
    }
    srand(seed);
    Verilated::randReset(2); Verilated::randSeed(seed);
    Vtb_xing *t = new Vtb_xing;
    long ticks = cycles * 6;
    // a reset at the start (24 cycles), and `resets` more, 12 cycles each, at random places; the two sides' resets begin and end
    // a few ticks apart, as the two clocks' registers of one `reset` do
    long rs[64], skew[64]; if (resets > 60) resets = 60;
    for (int i = 0; i < resets; i++) { rs[i] = 3000 + (long)(i + 1) * (cycles - 6000) / (resets + 1) + rand() % 500; skew[i] = rand() % 4; }
    t->rst_in = 1; t->rstc_in = 1;
    for (long k = 0; k < ticks; k++) {
        t->clk = (k % 6) < 3; t->clk_c = (k % 4) < 2;
        long cyc = k / 6;
        if (cyc == 24) { t->rst_in = 0; t->rstc_in = 0; }
        for (int i = 0; i < resets; i++) {
            if (k == rs[i] * 6) t->rst_in = 1;
            if (k == rs[i] * 6 + skew[i]) t->rstc_in = 1;
            if (k == (rs[i] + 12) * 6) t->rst_in = 0;
            if (k == (rs[i] + 12) * 6 + 3 - skew[i]) t->rstc_in = 0;
        }
        t->eval();
    }
    printf("cycles %ld errors %u | dma writes %u reads %u host %u dpc %u | max request fifo %u, word fifo %u | rsp edges %u of %ld cpu cycles | host access %.1f clk_sys cycles\n",
           cycles, t->errors, t->st_dma_w, t->st_dma_r, t->st_host, t->st_dpc, t->st_maxq, t->st_maxb, t->st_edges, cycles * 3 / 2, t->st_host ? (double)t->st_lat / t->st_host : 0.0);
    int e = t->errors != 0;
    delete t;
    return e;
}
