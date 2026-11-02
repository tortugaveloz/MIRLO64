/* The cartridge's byte and halfword reads (../n64rt_cart.h) against n64-systemtest's cart_memory expectations, on the host.
 *   gcc -O1 -Wall -I.. cart_host.c -o /tmp/cart_host && /tmp/cart_host */
#include <stdio.h>
#include <stdint.h>
typedef uint32_t u32;
#include "n64rt_cart.h"

/* the ROM as that test lays it out (words of the PI's bus) */
static const uint8_t rom[24] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x21, 0x43, 0x65, 0x87, 0x99, 0xBA, 0xDC, 0xFE,
                                 0xA9, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22 };
static const uint16_t want16[8] = { 0x0123, 0x89AB, 0x89AB, 0x2143, 0x2143, 0x99BA, 0x99BA, 0xA988 };
static const uint8_t want8[16] = { 0x01, 0x23, 0x89, 0xAB, 0x89, 0xAB, 0x21, 0x43, 0x21, 0x43, 0x99, 0xBA, 0x99, 0xBA, 0xA9, 0x88 };

int main(void)
{
    int fails = 0;
    for (u32 base = 0xB0000000u; base <= 0xB0123450u; base += 0x123450u)      /* (KSEG1 of the cartridge, two places) */
        for (u32 i = 0; i < 8; i++) {
            u32 a = n64rt_cart_sub_addr(base + 2 * i);
            unsigned got = rom[(a - base) & ~1u] << 8 | rom[((a - base) & ~1u) + 1];
            if (got != want16[i]) { printf("FAIL lh cart[%u] at %08x: %04x, want %04x\n", i, base, got, want16[i]); fails++; }
        }
    for (u32 i = 0; i < 16; i++) {
        u32 a = n64rt_cart_sub_addr(0xB0000000u + i);
        if (rom[a - 0xB0000000u] != want8[i]) { printf("FAIL lb cart[%u]: %02x, want %02x\n", i, rom[a - 0xB0000000u], want8[i]); fails++; }
    }
    /* the rest of the bus is as it is: RDRAM, the RCP, domain 2, the boot ROM's place, and the cartridge's end */
    static const u32 same[] = { 0x80000002u, 0xA0000006u, 0xA4040002u, 0xA8000002u, 0xBFC00002u, 0xA3F00002u };
    for (unsigned i = 0; i < sizeof same / sizeof same[0]; i++)
        if (n64rt_cart_sub_addr(same[i]) != same[i]) { printf("FAIL %08x moved\n", same[i]); fails++; }
    puts(fails ? "FAIL" : "PASS");
    return fails != 0;
}
