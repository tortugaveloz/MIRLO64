/* n64rt: the cartridge's domain 2 -- the save's memory, an SRAM or a FlashRAM -- as the runtime keeps it in the Pocket's
 * save slot (n64rt_io.c has the rest: the slot's size, the traps, the DMAs). Shared with lang/mips/test/pi2_test.c, which
 * runs this FlashRAM on the PI's engine in hardware (MIRLO_PI2). The includer has u8 / u32, <string.h>, <layout.h>,
 * <system.h> and defines d2_use(kind) and N64RT_SAVE_LOG (printf-like).
 *
 * The FlashRAM (128 KiB, libultra's osFlash*) speaks its command protocol through the command register (domain 2 + 0x1_0000):
 * read-array, status and ID modes, sector and chip erase, page program from a 128-byte buffer the game DMAs in -- as
 * mupen64plus has it: 0xD2 executes the erase or the program the commands before it chose (libultra's osFlashClearStatus).
 * Array reads address 16-bit units: the ID is an old MX chip's (0x00C2001E), for which libultra's devAddr is page * 64. */
#ifndef N64RT_SAVE_H
#define N64RT_SAVE_H
#include "n64rt_persist.h"
#define SAVE_MEM    ((u8 *)(uintptr_t)LAYOUT_SAVE)
#define FL_PAGE     ((u8 *)(uintptr_t)LAYOUT_SAVE_AUX)                       /* the page buffer */
#define FL_STATW    ((volatile u32 *)(uintptr_t)(LAYOUT_SAVE_AUX + 0x100u))  /* the status words (the hardware reads [0]) */
#define FLASH_BYTES 0x20000u
#define SRAM_BYTES  0x8000u
#define D2_WINDOW   0x20000u            /* (the hardware's: domain 2 repeats every 128 KiB) */
enum { FL_READ, FL_STATUS, FL_ERASE, FL_WRITE };
enum { D2_NONE, D2_SRAM, D2_FLASH };
static u32 s_fl_mode = FL_READ;
static u32 s_fl_off;                    /* the selected sector/page, in bytes; ~0: the whole chip */
static u32 s_d2 = D2_NONE;              /* what domain 2 is: an SRAM until the game writes the command register */
static inline void fl_status(u32 a, u32 b) { FL_STATW[0] = a; FL_STATW[1] = b; n64rt_persist((const void *)FL_STATW, 8); }
static void flash_cmd(u32 c)
{
    d2_use(D2_FLASH);
    switch (c >> 24) {
    case 0xF0: s_fl_mode = FL_READ;   fl_status(0x11118004u, 0xF0000000u); break;
    case 0xE1: s_fl_mode = FL_STATUS; fl_status(0x11118001u, 0x00C2001Eu); break;  /* (the MX ID) */
    case 0x4B: s_fl_mode = FL_ERASE;  s_fl_off = (c & 0xFFFFu) * 128u; break;
    case 0x3C: s_fl_mode = FL_ERASE;  s_fl_off = ~0u; break;
    case 0x78: s_fl_mode = FL_ERASE;  fl_status(0x11118008u, 0x00C2001Eu); break;
    case 0xB4: s_fl_mode = FL_WRITE;  break;
    case 0xA5: s_fl_off = (c & 0xFFFFu) * 128u; fl_status(0x11118004u, 0x00C2001Eu); break;
    case 0xD2:                                      /* execute */
        if (s_fl_mode == FL_ERASE) {
            if (s_fl_off == ~0u) { memset(SAVE_MEM, 0xFF, FLASH_BYTES); n64rt_persist(SAVE_MEM, FLASH_BYTES); }
            else if (s_fl_off < FLASH_BYTES) { memset(SAVE_MEM + (s_fl_off & ~0x3FFFu), 0xFF, 0x4000u); n64rt_persist(SAVE_MEM + (s_fl_off & ~0x3FFFu), 0x4000u); }
        } else if (s_fl_mode == FL_WRITE && s_fl_off + 128u <= FLASH_BYTES) {
            flush_cpu_dcache_range(FL_PAGE, 128u);  /* (the PI's engine may have written it) */
            memcpy(SAVE_MEM + s_fl_off, FL_PAGE, 128u);
            n64rt_persist(SAVE_MEM + s_fl_off, 128u);
        }
        break;
    default: N64RT_SAVE_LOG("n64rt: flash command %08lx?\n", (unsigned long)c); break;
    }
}
static inline int is_flash(u32 dev) { u32 pa = dev & 0x1FFFFFFFu; return pa >= 0x08000000u && pa < 0x10000000u; }
/* A DMA between the RDRAM and domain 2 at `off` (its 17 bits), `size` bytes (rd: the RDRAM into domain 2, PI_RD_LEN):
 * the SDRAM's address of domain 2's side, as the PI's engine takes it -- or 0, the CPU's copy's (n64rt_io.c's d2_copy:
 * what is out of the memory, or a DMA a FlashRAM in that mode ignores). A FlashRAM's page buffer is written from its start
 * whatever the offset, as its status words are read; its array is at off * 2. */
static __attribute__((unused)) u32 d2_engine_addr(int rd, u32 off, u32 size)
{
    if (s_d2 == D2_FLASH) {
        if (rd) return s_fl_mode == FL_WRITE && size <= 128u ? (u32)LAYOUT_SAVE_AUX : 0u;
        if (s_fl_mode == FL_STATUS || s_fl_mode == FL_ERASE) return off == 0u && size <= 8u ? (u32)LAYOUT_SAVE_AUX + 0x100u : 0u;
        return off * 2u + size <= FLASH_BYTES ? (u32)LAYOUT_SAVE + off * 2u : 0u;
    }
    if (off + size > D2_WINDOW || (rd && (off & 3u))) return 0u;
    return (u32)LAYOUT_SAVE + off;
}
#endif
