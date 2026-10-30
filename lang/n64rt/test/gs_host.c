/* GameShark codes (../n64rt_gs.c) on the host: the parse's cases, each code type on a memory of bytes, and random bytes (an
 * address-sanitised run must find nothing).
 *   gcc -O1 -g -fsanitize=address,undefined -I.. gs_host.c ../n64rt_gs.c -o /tmp/gs_host && /tmp/gs_host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n64rt_gs.h"

static unsigned char ram[0x800000];
static int fails;
static unsigned rd8(unsigned a) { return ram[a]; }
static unsigned rd16(unsigned a) { return (unsigned)(ram[a] << 8 | ram[a + 1]); }
static int wr8(unsigned a, unsigned v) { int c = ram[a] != (unsigned char)v; ram[a] = (unsigned char)v; return c; }
static int wr16(unsigned a, unsigned v) { int c = rd16(a) != v; ram[a] = (unsigned char)(v >> 8); ram[a + 1] = (unsigned char)v; return c; }
static const n64rt_gs_mem_t M = { rd8, rd16, wr8, wr16 };
#define CHECK(c, what) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, what); fails++; } } while (0)

static unsigned run(const char *txt, n64rt_gs_code_t *out)
{
    unsigned n = n64rt_gs_parse(txt, (unsigned)strlen(txt), out, 64);
    n64rt_gs_apply(out, n, &M);
    return n;
}

int main(void)
{
    n64rt_gs_code_t c[64];
    unsigned n;
    memset(ram, 0, sizeof ram);
    n = run("80123456 0063\n81234568 1234  two bytes\n", c);
    CHECK(n == 2 && ram[0x123456] == 0x63 && ram[0x234568] == 0x12 && ram[0x234569] == 0x34, "80 and 81");
    n = run("# a comment\n\n  ; another\n// and one\nnot a code\n80000010 0001 x\n8000001 0002\n80000012 00\n", c);
    CHECK(n == 1 && ram[0x10] == 1, "comments, blanks and lines that are not codes");
    n = run("80000020 0005\r\n80000021 0006\r\n", c);
    CHECK(n == 2 && ram[0x20] == 5 && ram[0x21] == 6, "CRLF");
    n = run("80000030 00aB\n", c);
    CHECK(n == 1 && ram[0x30] == 0xAB, "any case");
    n = run("81000041 1111\n", c);
    CHECK(n == 1 && ram[0x41] == 0 && ram[0x42] == 0, "a 16-bit write at an odd address is not made");
    ram[0x50] = 7;
    run("D0000050 0007\n80000051 0001\nD0000050 0008\n80000052 0001\n", c);
    CHECK(ram[0x51] == 1 && ram[0x52] == 0, "D0: if equal");
    ram[0x60] = 7; ram[0x61] = ram[0x62] = 0;
    run("D2000060 0007\n80000061 0001\nD2000060 0008\n80000062 0001\n", c);
    CHECK(ram[0x61] == 0 && ram[0x62] == 1, "D2: if not equal");
    ram[0x70] = 0x12; ram[0x71] = 0x34; ram[0x72] = ram[0x73] = 0;
    run("D1000070 1234\n80000072 0001\nD3000070 1234\n80000073 0001\n", c);
    CHECK(ram[0x72] == 1 && ram[0x73] == 0, "D1 and D3");
    memset(&ram[0x100], 0, 16);
    run("50040002 0001\n80000100 0010\n", c);
    CHECK(ram[0x100] == 0x10 && ram[0x102] == 0x11 && ram[0x104] == 0x12 && ram[0x106] == 0x13 && ram[0x108] == 0, "50: the repeater");
    ram[0x80] = 1; ram[0x81] = 0; ram[0x82] = 0;
    run("D0000080 0002\n50020001 0000\n80000081 0009\n80000082 0009\n", c);
    CHECK(ram[0x81] == 0 && ram[0x82] == 9, "a false condition skips the repeater's two lines");
    ram[0x90] = 0;
    run("88000090 0001\nFF000090 0001\n", c);
    CHECK(ram[0x90] == 0, "88 and unknown types are skipped");
    ram[0xA0] = 0;
    n = run("A00000A0 0003\nF00000A1 0004\nF10000A2 0506\n", c);
    CHECK(n == 3 && ram[0xA0] == 3 && ram[0xA1] == 4 && ram[0xA2] == 5 && ram[0xA3] == 6, "A0, F0 and F1 are writes");
    n = run("80ffffff 0001\n", c);
    CHECK(ram[0x7FFFFF] == 1, "the last byte of the RDRAM");
    n = n64rt_gs_parse("80000000 0001\n80000001 0002\n80000002 0003\n", 42, c, 2);
    CHECK(n == 2, "no more than max");
    srand(64);
    for (int r = 0; r < 20000; r++) {
        char b[200];
        unsigned len = (unsigned)(rand() % 200);
        for (unsigned i = 0; i < len; i++) b[i] = (char)(rand() % 3 ? "0123456789abcdefABCDEF \n\r\t:#;/D58"[rand() % 34] : rand());
        unsigned k = n64rt_gs_parse(b, len, c, 64);
        n64rt_gs_apply(c, k, &M);
    }
    printf(fails ? "gs_host: FAILED\n" : "gs_host: PASS\n");
    return fails != 0;
}
