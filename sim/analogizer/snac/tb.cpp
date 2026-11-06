// Bench of an_snac_pif (rtl/analogizer/an_snac_pif.sv, an_joybus_m.sv) on four wires with a behavioural Joybus device each
// (jb_dev.sv): the cells the master makes, the replies it takes, the four ports one after the other and the scan, the devices
// that are slow, late, short, talkative, noisy or hold the wire, and that no wire is ever driven high but the one case the
// shared bank allows (and never against a device). Exit status 0 if all pass.
#include "Vsnac_top.h"
#include <verilated.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>

static Vsnac_top *t;
static uint64_t clks = 0;
static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Pulse { int port; uint64_t start; uint64_t width; };            // the master's low pulses (clocks)
static std::vector<Pulse> pulses;
static int cur_start[4] = { -1, -1, -1, -1 };
static uint64_t pstart[4];
static uint64_t bus_busy_clks[4];

static void tick()
{
    t->clk = 0; t->eval();
    t->clk = 1; t->eval();
    clks++;
    for (int p = 0; p < 4; p++) {
        bool m = (t->mlow >> p) & 1;
        if (m && cur_start[p] < 0) { cur_start[p] = 1; pstart[p] = clks; }
        if (!m && cur_start[p] >= 0) { pulses.push_back({ p, pstart[p], clks - pstart[p] }); cur_start[p] = -1; }
    }
}
static void step(int n) { while (n--) tick(); }
static void reg_wr(int a, uint32_t v) { t->sel_wr = 1; t->a = a; t->wd = v; tick(); t->sel_wr = 0; step(3); }
static uint32_t reg_rd(int a, bool strobe = true)
{
    t->a = a; t->eval();
    uint32_t v = t->rd;
    if (strobe && a == 5) { t->sel_rd = 1; tick(); t->sel_rd = 0; } else tick();
    step(3);
    return v;
}
enum { R_ID = 0, R_CTRL = 1, R_STAT = 2, R_XFER = 3, R_PTR = 4, R_DATA = 5 };
struct Res { bool ok; bool timeout, shrt; unsigned cnt, us; std::vector<uint8_t> rx; uint64_t clocks; };
static Res xfer(int port, const std::vector<uint8_t> &tx, int rxlen, int max_clks = 400000)
{
    Res r{};
    reg_wr(R_PTR, 0);
    for (uint8_t b : tx) reg_wr(R_DATA, b);
    uint64_t c0 = clks;
    reg_wr(R_XFER, (uint32_t)port << 16 | (uint32_t)rxlen << 8 | (uint32_t)tx.size());
    uint32_t s;
    int n = 0;
    while (((s = reg_rd(R_XFER)) >> 31) && n < max_clks / 8) n++;
    r.ok = !(s >> 31);
    r.clocks = clks - c0;
    r.timeout = (s >> 30) & 1; r.shrt = (s >> 29) & 1; r.us = (s >> 16) & 0xFFF; r.cnt = s & 63;
    reg_wr(R_PTR, 0);
    for (unsigned i = 0; i < r.cnt; i++) r.rx.push_back((uint8_t)reg_rd(R_DATA));
    return r;
}
static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t c = 0;
    for (int i = 0; i <= n; i++)
        for (int k = 7; k >= 0; k--) {
            uint8_t x = (c & 0x80) ? 0x85 : 0;
            c = (uint8_t)(c << 1);
            if (i < n && ((d[i] >> k) & 1)) c |= 1;
            c ^= x;
        }
    return c;
}
static void set_dev(int p, int kind, int cell = 4000, int delay_us = 3, int btn = 0x1234, int stick = 0x7F80)
{
    t->k_kind = (t->k_kind & ~(0xFull << (4 * p))) | ((uint64_t)kind << (4 * p));
    auto setw = [&](uint64_t &w, int v) { w = (w & ~(0xFFFFull << (16 * p))) | ((uint64_t)(uint16_t)v << (16 * p)); };
    uint64_t c = t->k_cell, d = t->k_delay, b = t->k_btn, s = t->k_stick;
    setw(c, cell); setw(d, delay_us); setw(b, btn); setw(s, stick);
    t->k_cell = c; t->k_delay = d; t->k_btn = b; t->k_stick = s;
}
static void all_cells(int cell) { for (int p = 0; p < 4; p++) { uint64_t c = t->k_cell; c = (c & ~(0xFFFFull << (16 * p))) | ((uint64_t)cell << (16 * p)); t->k_cell = c; } }
static unsigned ncmds(int p) { return (t->d_ncmds >> (16 * p)) & 0xFFFF; }
static unsigned nwr(int p) { return (t->d_nwr >> (16 * p)) & 0xFFFF; }
static uint8_t pm(int addr) { return (uint8_t)((addr * 37 + 11) & 255); }
static void idle_us(int us) { step(us * 63); }

// the master's pulses of port p from index i on: widths in ns, and the gaps between their starts
static double ns(uint64_t c) { return c * 16.0; }
static void check_cells(int port, size_t from, const std::vector<uint8_t> &tx, const char *what)
{
    std::vector<Pulse> v;
    for (size_t i = from; i < pulses.size(); i++) if (pulses[i].port == port) v.push_back(pulses[i]);
    size_t want = tx.size() * 8 + 1;
    CHECK(v.size() == want, "%s port %d: %zu pulses, want %zu", what, port + 1, v.size(), want);
    if (v.size() != want) return;
    int bad_w = 0, bad_p = 0;
    for (size_t i = 0; i < tx.size() * 8; i++) {
        int bit = (tx[i / 8] >> (7 - i % 8)) & 1;
        double w = ns(v[i].width), want_w = bit ? 1000.0 : 3000.0;
        if (w < want_w - 60 || w > want_w + 60) bad_w++;
        double per = ns(v[i + 1].start - v[i].start);
        if (per < 3950 || per > 4050) bad_p++;
    }
    double sw = ns(v.back().width);
    CHECK(bad_w == 0, "%s port %d: %d cells with the low pulse off by more than 60 ns", what, port + 1, bad_w);
    CHECK(bad_p == 0, "%s port %d: %d cells off 4.00 us by more than 50 ns", what, port + 1, bad_p);
    CHECK(sw > 940 && sw < 1060, "%s port %d: stop bit %.0f ns", what, port + 1, sw);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    t = new Vsnac_top;
    t->rst = 1; t->sel_wr = t->sel_rd = 0; t->a = 0; t->wd = 0; t->own = 1;
    t->k_kind = 0; t->k_cell = 0; t->k_delay = 0; t->k_btn = 0; t->k_stick = 0;
    for (int p = 0; p < 4; p++) set_dev(p, 0);
    step(10); t->rst = 0; step(10);

    // ---- registers
    CHECK(reg_rd(R_ID) == 0x534E0001u, "ID");
    CHECK(((reg_rd(R_CTRL) >> 8) & 255) == 64, "TFIRST resets to 64");
    CHECK((reg_rd(R_STAT) & 255) == 0, "nothing present at reset");

    // ---- the four ports, one device each: a pad, a pad with a pak, a pad, a pad with a pak
    set_dev(0, 1, 4000, 3, 0xA5C3, 0x1020);
    set_dev(1, 2, 4000, 3, 0x0001, 0x7F7F);
    set_dev(2, 1, 4000, 3, 0x8000, 0xEF11);
    set_dev(3, 2, 4000, 3, 0x0F0F, 0x0102);
    reg_wr(R_CTRL, 1 | (64 << 8));
    step(200);
    unsigned c0[4]; for (int p = 0; p < 4; p++) c0[p] = ncmds(p);
    for (int p = 0; p < 4; p++) {
        size_t from = pulses.size();
        auto r = xfer(p, { 0x00 }, 3);
        CHECK(r.ok && r.cnt == 3 && !r.timeout && !r.shrt, "status port %d: ok %d cnt %u to %d sh %d", p + 1, r.ok, r.cnt, r.timeout, r.shrt);
        if (r.cnt == 3) CHECK(r.rx[0] == 0x05 && r.rx[1] == 0x00 && r.rx[2] == ((p == 1 || p == 3) ? 0x01 : 0x02), "status bytes port %d: %02x %02x %02x", p + 1, r.rx[0], r.rx[1], r.rx[2]);
        // 8 cells (32 us), the stop bit, the reply (3 bytes 96 us + its stop), the quiet wait: 130 to 190 us
        CHECK(r.us > 130 && r.us < 190, "status port %d: line time %u us", p + 1, r.us);
        check_cells(p, from, { 0x00 }, "status");
        from = pulses.size();
        r = xfer(p, { 0x01 }, 4);
        static const uint16_t btn[4] = { 0xA5C3, 0x0001, 0x8000, 0x0F0F }, stk[4] = { 0x1020, 0x7F7F, 0xEF11, 0x0102 };
        CHECK(r.ok && r.cnt == 4 && r.rx.size() == 4 && r.rx[0] == (btn[p] >> 8) && r.rx[1] == (btn[p] & 255) && r.rx[2] == (stk[p] >> 8) && r.rx[3] == (stk[p] & 255),
              "buttons port %d", p + 1);
        check_cells(p, from, { 0x01 }, "buttons");
        r = xfer(p, { 0xFF }, 3);
        CHECK(r.ok && r.cnt == 3 && r.rx[0] == 0x05, "reset/status 0xFF port %d", p + 1);
    }
    for (int p = 0; p < 4; p++) CHECK(ncmds(p) == c0[p] + 3, "port %d device saw %u commands (transactions on the other ports must not reach it)", p + 1, ncmds(p) - c0[p]);

    // ---- a pak: read a block, write one, read it back (ports 2 and 4: the bank-0 port too)
    for (int p : { 1, 3 }) {
        size_t from = pulses.size();
        auto r = xfer(p, { 0x02, 0x12, 0x40 }, 33);
        CHECK(r.ok && r.cnt == 33, "pak read port %d: cnt %u", p + 1, r.cnt);
        if (r.cnt == 33) {
            bool ok = true;
            for (int i = 0; i < 32; i++) ok &= r.rx[i] == pm(0x1240 + i);
            CHECK(ok, "pak read data port %d", p + 1);
            CHECK(r.rx[32] == crc8(r.rx.data(), 32), "pak read crc port %d: %02x want %02x", p + 1, r.rx[32], crc8(r.rx.data(), 32));
        }
        check_cells(p, from, { 0x02, 0x12, 0x40 }, "pak read");
        std::vector<uint8_t> w = { 0x03, 0x02, 0x80 };
        for (int i = 0; i < 32; i++) w.push_back((uint8_t)(0xC0 + i * 3 + p));
        from = pulses.size();
        unsigned n0 = nwr(p);
        r = xfer(p, w, 1);
        CHECK(r.ok && r.cnt == 1 && r.rx[0] == crc8(w.data() + 3, 32), "pak write crc port %d: %02x want %02x", p + 1, r.cnt ? r.rx[0] : 0, crc8(w.data() + 3, 32));
        CHECK(nwr(p) == n0 + 1, "pak write reached the device port %d", p + 1);
        // 35 bytes at 32 us, the stop, the reply: the line time is about 1,200 us
        CHECK(r.us > 1120 && r.us < 1300, "pak write port %d: line time %u us", p + 1, r.us);
        check_cells(p, from, w, "pak write");
        r = xfer(p, { 0x02, 0x02, 0x80 }, 33);
        bool ok = r.cnt == 33;
        for (int i = 0; ok && i < 32; i++) ok &= r.rx[i] == (uint8_t)(0xC0 + i * 3 + p);
        CHECK(ok, "pak read back port %d", p + 1);
    }
    // a pak command to a pad with no pak: no reply, a timeout
    {
        auto r = xfer(0, { 0x02, 0x00, 0x20 }, 33);
        CHECK(r.ok && r.timeout && r.cnt == 0, "pak read of a pad without pak: timeout %d cnt %u", r.timeout, r.cnt);
    }

    // ---- no device: a timeout after TFIRST
    set_dev(2, 0);
    {
        auto r = xfer(2, { 0x00 }, 3);
        CHECK(r.ok && r.timeout && r.cnt == 0 && !r.shrt, "absent: timeout %d cnt %u", r.timeout, r.cnt);
        // 32 us of command, the stop, 64 us of waiting
        CHECK(r.us > 90 && r.us < 110, "absent: line time %u us", r.us);
        reg_wr(R_CTRL, 1 | (20 << 8));
        r = xfer(2, { 0x00 }, 3);
        CHECK(r.timeout && r.us > 45 && r.us < 60, "absent with TFIRST 20: line time %u us", r.us);
        reg_wr(R_CTRL, 1 | (64 << 8));
    }
    set_dev(2, 1, 4000, 3, 0x8000, 0xEF11);

    // ---- bit cells off the nominal: -20 % .. +50 %
    for (int cell : { 3200, 3600, 4400, 5000, 6000 }) {
        all_cells(cell);
        bool ok = true;
        for (int p = 0; p < 4; p++) {
            auto r = xfer(p, { 0x01 }, 4);
            ok &= r.ok && r.cnt == 4 && r.rx[0] == (p == 0 ? 0xA5 : p == 1 ? 0x00 : p == 2 ? 0x80 : 0x0F);
            r = xfer(p, { 0x00 }, 3);
            ok &= r.ok && r.cnt == 3 && r.rx[0] == 5;
        }
        CHECK(ok, "devices with a %d ns cell", cell);
    }
    all_cells(4000);
    // a late device: 100 us is past TFIRST 64 (a timeout; the reply that comes is waited out), 100 us is fine with TFIRST 150
    set_dev(0, 1, 4000, 100, 0xA5C3, 0x1020);
    {
        auto r = xfer(0, { 0x00 }, 3);
        CHECK(r.timeout, "a reply after 100 us with TFIRST 64: timeout");
        idle_us(600);
        reg_wr(R_CTRL, 1 | (150 << 8));
        r = xfer(0, { 0x00 }, 3);
        CHECK(!r.timeout && r.cnt == 3 && r.rx[0] == 5, "a reply after 100 us with TFIRST 150: cnt %u", r.cnt);
        reg_wr(R_CTRL, 1 | (64 << 8));
    }
    set_dev(0, 1, 4000, 3, 0xA5C3, 0x1020);
    // a short reply, a device that talks on, a noisy line
    set_dev(0, 3);
    {
        auto r = xfer(0, { 0x00 }, 3);
        CHECK(r.ok && r.cnt == 2 && r.shrt && !r.timeout, "short reply: cnt %u short %d timeout %d", r.cnt, r.shrt, r.timeout);
        auto r2 = xfer(0, { 0x00 }, 3);
        CHECK(r2.cnt == 2 && r2.shrt, "short reply again");
    }
    set_dev(0, 5, 4000, 3, 0xA5C3, 0x1020);
    {
        auto r = xfer(0, { 0x00 }, 3);
        CHECK(r.ok && r.cnt == 3 && !r.shrt && !r.timeout && r.rx[0] == 5, "talkative: cnt %u short %d", r.cnt, r.shrt);
        r = xfer(0, { 0x01 }, 4);
        CHECK(r.ok && r.cnt == 4 && r.rx[0] == 0xA5, "and the next command after the talkative reply");
    }
    set_dev(0, 6, 4000, 3);
    {
        bool ok = true;
        for (int i = 0; i < 8; i++) {
            auto r = xfer(0, { 0x02, 0x00, (uint8_t)(0x20 * i) }, 33);
            ok &= r.ok && r.cnt == 33 && r.rx[0] == pm(0x20 * i) && r.rx[32] == crc8(r.rx.data(), 32);
        }
        CHECK(ok, "noisy line: eight pak reads");
    }
    // a wire held low for 400 us
    set_dev(0, 4);
    {
        auto r = xfer(0, { 0x00 }, 3, 2000000);
        CHECK(r.ok && r.cnt == 0 && (r.shrt || r.timeout), "wire held low: finished (cnt %u short %d timeout %d, %u us)", r.cnt, r.shrt, r.timeout, r.us);
        CHECK(r.us < 600, "wire held low: given up after %u us", r.us);
        idle_us(600);
        set_dev(0, 1, 4000, 3, 0xA5C3, 0x1020);
        idle_us(50);
        r = xfer(0, { 0x00 }, 3);
        CHECK(r.ok && r.cnt == 3, "after the wire is let go the port works again");
    }

    // ---- the scan
    for (int p = 0; p < 4; p++) set_dev(p, p == 2 ? 0 : (p & 1) ? 2 : 1);
    reg_wr(R_STAT, 0xF0);
    unsigned sc0[4]; for (int p = 0; p < 4; p++) sc0[p] = ncmds(p);
    reg_wr(R_CTRL, 3 | (64 << 8));
    idle_us(10000);
    {
        uint32_t s = reg_rd(R_STAT);
        CHECK((s & 15) == 0b1011, "scan: present %x", s & 15);
        CHECK(((s >> 4) & 15) == 0b1011, "scan: changed %x", (s >> 4) & 15);
        reg_wr(R_STAT, 0xF0);
        CHECK(((reg_rd(R_STAT) >> 4) & 15) == 0, "changed cleared by writing 1s");
        for (int p = 0; p < 4; p++) {
            reg_wr(R_PTR, 64 + 4 * p);
            uint8_t n = (uint8_t)reg_rd(R_DATA), b0 = (uint8_t)reg_rd(R_DATA), b1 = (uint8_t)reg_rd(R_DATA), b2 = (uint8_t)reg_rd(R_DATA);
            if (p == 2) CHECK(n == 0, "scan table port 3: %u", n);
            else CHECK(n == 3 && b0 == 5 && b1 == 0 && b2 == ((p & 1) ? 1 : 2), "scan table port %d: %u %02x %02x %02x", p + 1, n, b0, b1, b2);
        }
        for (int p = 0; p < 4; p++) {
            unsigned got = ncmds(p) - sc0[p];
            if (p == 2) continue;
            CHECK(got >= 1 && got <= 2, "scanned port %d %u times in 10 ms (one in 8 ms)", p + 1, got);
        }
    }
    // a transaction asked for at any moment of the scan
    {
        bool ok = true; unsigned worst = 0;
        for (int i = 0; i < 24; i++) {
            idle_us(137 + i * 211);
            uint64_t c = clks;
            auto r = xfer(i % 4 == 2 ? 3 : i % 4, { 0x01 }, 4);
            ok &= r.ok && r.cnt == 4;
            unsigned us = (unsigned)((clks - c) / 63);
            if (us > worst) worst = us;
        }
        CHECK(ok, "transactions among the scan");
        CHECK(worst < 700, "the worst transaction (with the scan's wait) took %u us", worst);
    }
    // a device goes, another comes
    set_dev(1, 0);
    set_dev(2, 1, 4000, 3, 0x1111, 0x2222);
    idle_us(10000);
    {
        uint32_t s = reg_rd(R_STAT);
        CHECK((s & 15) == 0b1101, "scan after the change: present %x", s & 15);
        CHECK(((s >> 4) & 15) == 0b0110, "scan after the change: changed %x", (s >> 4) & 15);
    }
    reg_wr(R_CTRL, 1);                              // the scan off: nothing more is polled
    idle_us(500);
    unsigned c1 = ncmds(0);
    idle_us(10000);
    CHECK(ncmds(0) == c1, "no scan, no commands");

    // ---- the pins stay off: ON clear, or the adapter not the owner
    {
        reg_wr(R_CTRL, 0);
        size_t n0 = pulses.size();
        auto r = xfer(0, { 0x00 }, 3);
        CHECK(r.ok && r.timeout && pulses.size() == n0 && !t->en, "ON clear: no pulse, a timeout");
        reg_wr(R_CTRL, 1);
        t->own = 0; step(5);
        n0 = pulses.size();
        r = xfer(0, { 0x00 }, 3);
        CHECK(r.timeout && pulses.size() == n0 && !t->en, "not the owner: no pulse, a timeout");
        t->own = 1; step(5);
        CHECK(t->en, "ON and the owner: the pins are the Joybus's");
    }

    // ---- the pins' rules
    CHECK(t->bad_oe == 0, "the driver was on with the translator not outward %u times", (unsigned)t->bad_oe);
    CHECK(t->conflict == 0, "bank 0 drove a 1 against a device %u times", (unsigned)t->conflict);
    {
        uint64_t n[4] = { 0, 0, 0, 0 };
        for (auto &p : pulses) n[p.port]++;
        CHECK(t->glitch30 == 2 * n[0], "pin 30: translator outward without the driver %u cycles for %llu pulses", (unsigned)t->glitch30, (unsigned long long)n[0]);
        CHECK(t->glitch31 == 2 * n[1], "pin 31: %u cycles for %llu pulses", (unsigned)t->glitch31, (unsigned long long)n[1]);
        CHECK(t->glitchb0 == 2 * (n[2] + n[3]), "bank 0: %u cycles for %llu pulses", (unsigned)t->glitchb0, (unsigned long long)(n[2] + n[3]));
        printf("pulses per port: %llu %llu %llu %llu; bank 0 drove a 1 in %u cycles (all with the other bit low and its device idle)\n",
               (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[3], (unsigned)t->drive_hi);
    }
    printf("%d checks, %d failed, %.1f ms simulated\n", checks, fails, clks * 16e-6);
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
    delete t;
    return fails ? 1 : 0;
}
