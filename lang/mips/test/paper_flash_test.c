/* Exercise Paper Mario's rotating FlashRAM records using the runtime's
 * FlashRAM interpreter and PI engine, with a real save preloaded as SLOT1. */
#include <stdint.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
typedef uint8_t u8;
typedef uint32_t u32;
static void d2_use(int kind);
static void nolog(const char *s, ...) { (void)s; }
#define N64RT_SAVE_LOG nolog
#include "../../n64rt/n64rt_save.h"
static void d2_use(int kind)
{
    if (s_d2 == D2_FLASH || (int)s_d2 == kind) return;
    s_d2 = (u32)kind;
    if (kind == D2_FLASH) { fl_status(0, 0); monitor_pi2_write(1); }
}
static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((u8)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putnum(unsigned n) { char b[12]; int k=0; do { b[k++]=(char)('0'+n%10); n/=10; } while(n); while(k) putc_(b[--k]); }
static int fails;
static void check(const char *s, int ok) { if (!ok) { puts_("FAIL "); puts_(s); puts_("\n"); fails++; } }
#define BUFFER_PA 0x00040000u
#define BUFFER_K1 ((volatile u32 *)(uintptr_t)0xa0040000u)
#define BLOCK_BYTES 0x1380u
static void pi_copy(u32 dst, u32 src, u32 bytes)
{
    u32 dp = dst >= 0x34000000u && dst < 0x34200000u;
    u32 sp = src >= 0x34000000u && src < 0x34200000u;
    monitor_pidst_write(((dst - (dp ? 0x34000000u : 0x40000000u)) >> 2) | (dp << 25));
    monitor_pisrc_write(((src - (sp ? 0x34000000u : 0x40000000u)) >> 2) | (sp << 25));
    monitor_pilen_write((bytes + 3u) >> 2);
    unsigned t;
    for (t=0; t<4000000u && (monitor_pilen_read() >> 31); t++) { }
    check("PI completion", !(monitor_pilen_read() >> 31));
}
static void read_sector(unsigned sector)
{
    flash_cmd(0xf0000000u);
    u32 src=d2_engine_addr(0, sector * 0x2000u, BLOCK_BYTES);
    check("flash read address", src != 0);
    pi_copy(0x43800000u + BUFFER_PA, src, BLOCK_BYTES);
}
static int valid_record(unsigned *slot, unsigned *generation)
{
    volatile u32 *p=BUFFER_K1; u32 sum=0;
    for (unsigned i=0; i<BLOCK_BYTES/4; i++) sum += p[i];
    *slot=p[0x38/4]; *generation=p[0x3c/4];
    return p[0] == 0x4d617269u && p[1] == 0x6f205374u && p[2] == 0x6f727920u &&
           p[3] == 0x30303600u && p[0x30/4] == ~p[0x34/4] && sum == p[0x30/4] && *slot < 4;
}
static int scan(int active[4], unsigned generation[4], unsigned physical_generation[6])
{
    for (int i=0;i<4;i++) { active[i]=-1; generation[i]=0; }
    for (int i=0;i<6;i++) {
        unsigned slot, gen;
        read_sector((unsigned)i);
        if (valid_record(&slot, &gen)) {
            physical_generation[i]=gen;
            if (active[slot] < 0 || gen > generation[slot]) { active[slot]=i; generation[slot]=gen; }
        } else physical_generation[i]=0;
    }
    for (int i=0;i<4;i++) if (active[i] < 0) return 0;
    return 1;
}
int main(void)
{
    int active[4]; unsigned generation[4], physical[6];
    puts_("paper_flash_test\n");
    monitor_mask_write(0); monitor_miclr_write(0x3e);
    flash_cmd(0xf0000000u);
#ifdef PAPER_VERIFY_ONLY
    check("four valid slots after reboot", scan(active, generation, physical));
#else
#ifdef PAPER_ONLY_FIRST
    const unsigned number_of_saves=1;
#else
    const unsigned number_of_saves=4;
#endif
    for (unsigned logical=0; logical<number_of_saves; logical++) {
        if (!scan(active, generation, physical)) { check("four valid slots before save", 0); break; }
        int victim=-1;
        for (int s=0;s<6;s++) {
            int used=0; for (int j=0;j<4;j++) if (active[j]==s) used=1;
            if (!used && (victim<0 || physical[s]<physical[victim])) victim=s;
        }
        check("available flash sector", victim>=0); if (victim<0) break;
        read_sector((unsigned)active[logical]);
        volatile u32 *p=BUFFER_K1;
        unsigned newest=0; for (int i=0;i<4;i++) if (generation[i]>newest) newest=generation[i];
        p[0x3c/4]=newest+1; p[0x30/4]=0; p[0x34/4]=0xffffffffu;
        u32 sum=0; for (unsigned i=0;i<BLOCK_BYTES/4;i++) sum+=p[i];
        p[0x30/4]=sum; p[0x34/4]=~sum;
        flash_cmd(0x4b000000u | ((unsigned)victim*128u)); flash_cmd(0x78000000u); flash_cmd(0xd2000000u);
        for (unsigned page=0;page<BLOCK_BYTES/128;page++) {
            flash_cmd(0xb4000000u);
            u32 dst=d2_engine_addr(1, 0, 128);
            pi_copy(dst, 0x43800000u + BUFFER_PA + page*128u, 128);
            flash_cmd(0xa5000000u | ((unsigned)victim*128u + page)); flash_cmd(0xd2000000u);
        }
        check("four valid slots after save", scan(active, generation, physical));
        check("new record selected", active[logical]==victim && generation[logical]==newest+1);
        puts_("slot "); putnum(logical+1); puts_(" saved to sector "); putnum((unsigned)victim); puts_("\n");
    }
#endif
    puts_(fails ? "paper_flash_test: FAIL\n" : "paper_flash_test: PASS\n");
    putc_(4); for (;;) { }
}
