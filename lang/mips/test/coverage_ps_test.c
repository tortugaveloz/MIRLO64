#include <stdint.h>
#include <generated/csr.h>
#include <layout.h>
static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
void trap_handler(void) { puts_("FAIL trap\n"); for (;;) {} }
static unsigned failures;
static void check(uint32_t got, uint32_t want) { if (got != want) failures++; }
int main(void) {
    static const uint32_t pos[] = { 0, 4, 0x7fffc, 0xffffc };
    volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)LAYOUT_HID;
    volatile uint32_t *rom = (volatile uint32_t *)(uintptr_t)0x30000000u;
    for (unsigned i=0;i<4;i++) h[pos[i]/4] = 0x4a512345u ^ pos[i];
    for (unsigned i=0;i<4;i++) check(h[pos[i]/4],0x4a512345u ^ pos[i]);
    check(rom[0],0x11223344u);
    check(rom[0x00dffffcu/4],0x81a2b3c4u);
    check(rom[0x00e00000u/4],0x92b3c4d5u);
    check(rom[(0x01e00000u-4)/4],0x55667788u);
    check(rom[0x01e00000u/4],0x99aabbccu);
    check(rom[(0x03000000u-4)/4],0x55667788u);
    check(rom[0x03000000u/4],0x99aabbccu);
    check(rom[0x02000000u/4],0x12345678u);
    check(rom[0x03fffffcu/4],0xdeadbeefu);
    check(rom[0x036ffffcu/4],0x01234567u);
    check(rom[0x03700000u/4],0x89abcdefu);
    for (unsigned i=0;i<4;i++) check(h[pos[i]/4],0x4a512345u ^ pos[i]);
    volatile uint8_t *save = (volatile uint8_t *)(uintptr_t)LAYOUT_SAVE;
    volatile uint32_t *paks=(volatile uint32_t *)(uintptr_t)LAYOUT_CPAK;
    for (unsigned i=0;i<4;i++) paks[(i*LAYOUT_PAK_BYTES)/4]=0x13570000u+i;
    for (unsigned i=0;i<128;i++) save[i]=(uint8_t)(i*31+7);
    for (unsigned i=0;i<4;i++) check(paks[(i*LAYOUT_PAK_BYTES)/4],0x13570000u+i);
    for (unsigned i=0;i<128;i++) check(save[i],(uint8_t)(i*31+7));
    check(rom[0],0x11223344u);
    puts_(failures ? "coverage_ps_test: FAIL\n" : "coverage_ps_test: PASS\n");
    putc_(4); for (;;) {}
}
