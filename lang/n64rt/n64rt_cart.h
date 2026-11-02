/* The cartridge's bus, for a game's own byte and halfword reads of it (n64rt_os.c emulate_hw_at).
 *
 * The PI gives a word at a time; a lb, lbu, lh or lhu of the cartridge is "broken" on the console: every other 16-bit word is out of
 * reach. A read at a halfword whose index is odd gets the next halfword's bytes (the address' bit 1 set: two more than asked), an even
 * one is as it should be -- for the halfwords 0 ... 7 of the ROM it gives 0, 2, 2, 4, 4, 6, 6, 8 (n64-systemtest's cart_memory). Words
 * are as they are. */
#ifndef N64RT_CART_H
#define N64RT_CART_H

/* the address a lb, lbu, lh or lhu of `addr` (a KSEG0 / KSEG1 address) reads */
static inline u32 n64rt_cart_sub_addr(u32 addr)
{
    u32 pa = addr & 0x1FFFFFFFu;
    return (pa >= 0x10000000u && pa < 0x1FC00000u && (addr & 2u)) ? addr + 2u : addr;
}

#endif
