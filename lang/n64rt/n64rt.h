/* n64rt: an N64 game's own code, run unmodified on Mirlo's game CPU.
 *
 * The game CPU is big-endian like the N64, and mips_core's N64 window maps
 * KSEG0/KSEG1 0x8000_0000 + 8 MiB onto SDRAM 0x4380_0000 (docs/mips.md,
 * "Running N64 code"): the game's instructions, data and pointers are the
 * N64's. What is not there is the N64's hardware. As in a static
 * recompilation (N64Recomp's runtime), the libultra functions that touch it
 * are replaced: at load, each one's first three instructions become a jump
 * to Mirlo's version here (n64rt_patch.S), which runs as ordinary native
 * code -- IDO's o32 calling convention is GCC's.
 *
 *   - threads, message queues, events, timers: a priority-preemptive
 *     scheduler of the game's threads (n64rt_os.c, n64rt_exc.S)
 *   - VI, PI (the ROM is in SDRAM), SI (the Pocket's controllers), AI and
 *     the RSP's tasks (n64rt_io.c)
 *
 * A game supplies an n64rt_game_t: where its ROM is, how it boots, and the
 * address of each libultra function to replace (from a symbol map). Any
 * access to N64 hardware the game still makes traps (an address error) and
 * is reported with its address. */
#ifndef N64RT_H
#define N64RT_H

#include <stdint.h>

typedef int8_t   s8;   typedef uint8_t  u8;
typedef int16_t  s16;  typedef uint16_t u16;
typedef int32_t  s32;  typedef uint32_t u32;
typedef int64_t  s64;  typedef uint64_t u64;

/* ---- libultra's types, as laid out in the game's memory --------------- */
typedef s32 OSPri;
typedef s32 OSId;
typedef void *OSMesg;
typedef u32 OSEvent;
typedef u64 OSTime;

typedef struct OSThread_s {
    struct OSThread_s  *next;       /* 0x00 run queue / wait queue link */
    OSPri               priority;   /* 0x04 */
    struct OSThread_s **queue;      /* 0x08 the queue it waits in */
    struct OSThread_s  *tlnext;     /* 0x0C */
    u16                 state;      /* 0x10 OS_STATE_* */
    u16                 flags;      /* 0x12 */
    OSId                id;         /* 0x14 */
    s32                 fp;         /* 0x18 */
    u32                 thprof;     /* 0x1C */
    u32                 context[100]; /* 0x20 libultra's __OSThreadContext:
                                     * n64rt keeps its own saved state here
                                     * (n64rt_ctx_t), 0x190 bytes */
} __attribute__((aligned(8))) OSThread;   /* (as libultra's, whose context holds 64-bit registers: they are
                                           * saved with sd, which takes an address error anywhere else) */
#define OS_STATE_STOPPED  1
#define OS_STATE_RUNNABLE 2
#define OS_STATE_RUNNING  4
#define OS_STATE_WAITING  8

typedef struct {
    OSThread *mtqueue;              /* threads waiting for a message */
    OSThread *fullqueue;            /* threads waiting for room */
    s32       validCount;
    s32       first;
    s32       msgCount;
    OSMesg   *msg;
} OSMesgQueue;
#define OS_MESG_NOBLOCK 0
#define OS_MESG_BLOCK   1

typedef struct OSTimer_s {
    struct OSTimer_s *next, *prev;
    OSTime            interval;
    OSTime            value;
    OSMesgQueue      *mq;
    OSMesg            msg;
} OSTimer;

#define OS_EVENT_SW1        0
#define OS_EVENT_SW2        1
#define OS_EVENT_CART       2
#define OS_EVENT_COUNTER    3
#define OS_EVENT_SP         4
#define OS_EVENT_SI         5
#define OS_EVENT_AI         6
#define OS_EVENT_VI         7
#define OS_EVENT_PI         8
#define OS_EVENT_DP         9
#define OS_EVENT_CPU_BREAK  10
#define OS_EVENT_SP_BREAK   11
#define OS_EVENT_FAULT      12
#define OS_EVENT_THREADSTATUS 13
#define OS_EVENT_PRENMI     14
#define OS_NUM_EVENTS       15

typedef struct OSPiHandle_s {
    struct OSPiHandle_s *next;
    u8  type, latency, pageSize, relDuration, pulse, domain;
    u32 baseAddress;
    u32 speed;
    u32 transferInfo[24];
} OSPiHandle;

typedef struct {
    u16          type;
    u8           pri;
    u8           status;
    OSMesgQueue *retQueue;
} OSIoMesgHdr;
typedef struct {
    OSIoMesgHdr  hdr;
    void        *dramAddr;
    u32          devAddr;
    u32          size;
    OSPiHandle  *piHandle;
} OSIoMesg;
#define OS_READ  0
#define OS_WRITE 1

typedef struct {
    u32  type, flags;
    u64 *ucode_boot;  u32 ucode_boot_size;
    u64 *ucode;       u32 ucode_size;
    u64 *ucode_data;  u32 ucode_data_size;
    u64 *dram_stack;  u32 dram_stack_size;
    u64 *output_buff; u64 *output_buff_size;
    u64 *data_ptr;    u32 data_size;
    u64 *yield_data_ptr; u32 yield_data_size;
} OSTask_t;
typedef union { OSTask_t t; u64 force_structure_alignment; } OSTask;
#define M_GFXTASK 1
#define M_AUDTASK 2

typedef struct { u16 type; u8 status; u8 errnum; } OSContStatus;
typedef struct { u16 button; s8 stick_x; s8 stick_y; u8 errnum; } OSContPad;

/* ---- the replaced functions: X(libultra name) ------------------------- */
#define N64RT_FUNCS(X) \
    X(osInitialize) X(osGetMemSize) X(osVirtualToPhysical) X(osMapTLB) X(osUnmapTLB) X(osUnmapTLBAll) \
    X(osCreateThread) X(osStartThread) X(osStopThread) X(osDestroyThread) \
    X(osSetThreadPri) X(osGetThreadPri) X(osYieldThread) \
    X(osCreateMesgQueue) X(osSendMesg) X(osJamMesg) X(osRecvMesg) \
    X(osSetEventMesg) X(osSetTimer) X(osGetTime) X(osGetCount) \
    X(osSetIntMask) X(__osDisableInt) X(__osRestoreInt) X(__osSetFpcCsr) \
    X(osInvalDCache) X(osInvalICache) X(osWritebackDCache) X(osWritebackDCacheAll) \
    X(osCreateViManager) X(osViSetMode) X(osViSetEvent) X(osViSwapBuffer) X(osViBlack) \
    X(osViSetSpecialFeatures) X(osViSetXScale) X(osViSetYScale) X(osViGetCurrentFramebuffer) \
    X(osViGetNextFramebuffer) \
    X(osCreatePiManager) X(osCartRomInit) X(osEPiStartDma) X(osEPiRawStartDma) \
    X(osEPiReadIo) X(osEPiWriteIo) X(osEPiRawReadIo) X(osEPiRawWriteIo) \
    X(osPiStartDma) X(osPiRawStartDma) X(osPiGetStatus) \
    X(osContInit) X(osContStartReadData) X(osContGetReadData) X(osContStartQuery) X(osContGetQuery) \
    X(__osContRamRead) X(__osContRamWrite) X(__osPfsGetStatus) X(__osSiRawStartDma) X(osPfsIsPlug) \
    X(osEepromProbe) X(osEepromRead) X(osEepromWrite) X(osEepromLongRead) X(osEepromLongWrite) \
    X(osAiSetFrequency) X(osAiSetNextBuffer) X(osAiGetLength) X(osAiGetStatus) \
    X(osSpTaskLoad) X(osSpTaskStartGo) X(osSpTaskYield) X(osSpTaskYielded) X(__osSpSetPc) \
    X(osDpSetStatus) X(osDpGetCounters) X(osAfterPreNMI) \
    X(LeoDriveExist) X(osMotorInit) X(osMotorStart) X(osMotorStop) X(osSetTime) X(osViRepeatLine) \
    X(osPiRawReadIo) \
    X(__ll_div) X(__ll_mul) X(__ull_div) X(__ull_rem) X(__ll_rem) X(__ll_mod) \
    X(__ll_lshift) X(__ll_rshift) X(__ull_rshift) X(__ull_divremi) \
    X(osStopTimer) X(osGetThreadId)

/* The SI's among them -- the controllers, their accessories, the cartridge's
 * EEPROM. A runtime built with N64RT_SI_GAME has none of these: the game's
 * own run, on the PIF as n64rt_io.c emulates it, and a game's list may still
 * name them (they are left alone). The one they all go through, the SI's
 * transfer of the PIF's block, is the game's too with N64RT_SI_GAME 1 -- each
 * of its register accesses a trap -- and with N64RT_SI_GAME 2 is replaced
 * where a game's list has it, which saves the traps and nothing else. */
#define N64RT_SI_FUNCS(X) \
    X(osContInit) X(osContStartReadData) X(osContGetReadData) X(osContStartQuery) X(osContGetQuery) \
    X(__osContRamRead) X(__osContRamWrite) X(__osPfsGetStatus) X(osPfsIsPlug) \
    X(osEepromProbe) X(osEepromRead) X(osEepromWrite) X(osEepromLongRead) X(osEepromLongWrite) \
    X(osMotorInit) X(osMotorStart) X(osMotorStop)
#define N64RT_SI_DMA(X) X(__osSiRawStartDma)

enum {
#define N64RT_ENUM(n) N64RT_##n,
    N64RT_FUNCS(N64RT_ENUM)
#undef N64RT_ENUM
    N64RT_NFUNCS
};

/* ---- a game ------------------------------------------------------------- */
typedef struct {
    u16 func;                       /* N64RT_* */
    u32 vram;                       /* the libultra function's address in the game */
} n64rt_patch_t;
/* A libultra function in code the game loads after its boot segment (a
 * port's list, ended by vram 0; none: no such list). It is patched when its
 * first two words are in memory -- looked for when the game has had a chance
 * to put them there: after a DMA from the cartridge, at osInvalICache (a game
 * that writes code must call it), and at every field. */
typedef struct {
    u16 func;
    u32 vram;
    u32 w[2];
} n64rt_late_t;
extern const n64rt_late_t n64rt_late[] __attribute__((weak));
extern const unsigned n64rt_nlate __attribute__((weak));
void n64rt_late_poll(void);
/* Where the boot segment goes into such code by itself -- a jump there once it
 * has unpacked it, with nothing of the runtime's called in between (a port's
 * list of those instructions, ended by 0). A break is put on each; the first
 * one met replaces the functions and puts the instructions back. */
extern const u32 n64rt_hooks[] __attribute__((weak));
extern const unsigned n64rt_nhooks __attribute__((weak));
int n64rt_hook_break(u32 epc);

typedef struct {
    const char  *name;
    u32          rom_sdram;         /* where the ROM sits in SDRAM (its words as the N64's) */
    u32          rom_size;
    u32          boot_rom, boot_vram, boot_len;   /* what IPL3 copies (ROM offset -> RDRAM) */
    u32          entry;             /* where IPL3 jumps */
    u32          mem_size;          /* RDRAM the game sees: 4 or 8 MiB */
    u32          cic;               /* 6101..6106: the CIC (osCicId, 0x80000310, is not this number: n64rt_boot.c) */
    const n64rt_patch_t *patches;
    u32          npatches;
    /* the game's own lists of functions in code loaded later and of the boot
     * segment's ways into it (as n64rt_late, n64rt_hooks above; null: those) */
    const n64rt_late_t *late;
    u32          nlate;
    const u32   *hooks;
    u32          nhooks;
} n64rt_game_t;

/* The word a CIC's IPL3 writes as osCicId (0x8000_0310): the 6103's 0x17D7, the 6105's 0x17D9, the 6106's 0x17D8; the
 * others write nothing there (0). The 6103's and the 6106's IPL3 also leave it in IMEM's first word (n64rt_rsp.c). */
static inline u32 n64rt_cic_id(u32 cic)
{
    return cic == 6103u ? 0x17D7u : cic == 6105u ? 0x17D9u : cic == 6106u ? 0x17D8u : 0u;
}

/* The ROM as its own file, unchanged -- a .z64 the Pocket loaded at 1 MiB
 * (N64RT_ROM_FILE_OFF), the program being another file at 0: rom_size is that
 * file's size (data slot 0's). Places it and runs the game; never returns. */
void n64rt_boot_rom(n64rt_game_t *g, u32 rom_size) __attribute__((noreturn));

/* Load the game (RDRAM, patches), start it; never returns. */
void n64rt_run(const n64rt_game_t *g) __attribute__((noreturn));
/* The usual ways in. The Pocket loaded the program at 0x4000_0000 and the
 * ROM at N64RT_ROM_FILE_OFF from there, as the block ROM0 of a game file
 * that holds both (n64rt_boot_game_file) or as a file of its own, a .z64 as
 * it is, of rom_size bytes (n64rt_boot_rom). Each sets g->rom_sdram and
 * rom_size, then n64rt_run(g); the port fills in the rest of g.
 * The ROM is read where the Pocket's load leaves it, in the SDRAM, and never
 * moved (lang/mips/include/layout.h): a ROM of up to 48 MiB.
 * n64rt_rom_word() reads a word of it. */
#define N64RT_ROM_FILE_OFF 0x00100000u
u32  n64rt_rom_word(u32 off);
void n64rt_boot_game_file(n64rt_game_t *g) __attribute__((noreturn));

/* the RDRAM window, as the runtime's native code reaches it */
#define N64RT_RDRAM_SDRAM 0x43800000u
#define N64RT_K0(pa) (0x80000000u | ((u32)(pa) & 0x007FFFFFu))
#define N64RT_PHYS(va) ((u32)(va) & 0x1FFFFFFFu)
#define N64RT_K1(a)  (0xA0000000u | ((u32)(a) & 0x007FFFFFu))   /* the same RDRAM, past the D-cache */

/* ---- inside the runtime ---------------------------------------------------- */
extern const n64rt_game_t *n64rt_game;
/* The console's TV system: libultra's osTvType (0 PAL, 1 NTSC, 2 MPAL), taken from the destination code of the ROM's header
 * (n64rt_boot.c) -- the only thing the runtime knows of a game's region. n64rt_vi_pal() is what the scan-out is using now: 50 Hz
 * (the game's PAL modes write V_SYNC 625; a PAL console's 60 Hz modes are NTSC's timing). */
extern u32 n64rt_tv;
u32 n64rt_vi_pal(void);
/* the VI's clock, libultra's osViClock for the TV system (the AI's DAC divides it: AI_DACRATE + 1 = osViClock / rate) */
static inline u32 n64rt_vi_clock(void) { return n64rt_tv == 0u ? 49656530u : n64rt_tv == 2u ? 48628316u : 48681812u; }
#define N64RT_FIELD_NTSC 1042710u              /* the scan-out's field, SoC cycles (rtl/soc/n64_video.sv): 59.940 Hz */
#define N64RT_FIELD_PAL  1250000u              /* 50.000 Hz */
extern OSThread *n64rt_cur;
void n64rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void n64rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
int  n64rt_post_event(int event);          /* from the runtime (interrupts masked); 0: the game set no queue for it */
void n64rt_send_isr(OSMesgQueue *mq, OSMesg msg);   /* likewise: never blocks, never switches */
void n64rt_resched(void);                  /* switch if a higher-priority thread is ready */
OSTime n64rt_time(void);                   /* the N64's Count rate, 46.875 MHz, 64-bit */
void n64rt_tick(void);                     /* the timer interrupt: VI fields, timers */
void n64rt_io_init(void);
extern u32 n64rt_cpu64, n64rt_cpu_alu, n64rt_cpu_sh, n64rt_cpu_md;   /* what the game CPU does of the VR4300's 64-bit integers (n64rt_os.c) */
extern u32 n64rt_yield_wait_ms;            /* a port's: a graphics task's yield held back this long (n64rt_rsp.c) */
/* controller 1's accessory (n64rt_pak.c): the game's own osPfs* and osGbpak*
 * run on these blocks */
enum { N64RT_PAK_NONE, N64RT_PAK_CONTROLLER, N64RT_PAK_TRANSFER };   /* (the "Controller 1 pak" setting's values: n64rt_pak.c) */
void n64rt_pak_init(void);
int  n64rt_pak_kind(int ch);
int  n64rt_pak_rw(int ch, u32 addr, u8 *block, int write);
u8   n64rt_pak_crc(const u8 *block);
void *n64rt_rdram_alloc(u32 size);         /* zeroed RDRAM the game can reach (above its mem_size) */
/* a task of another microcode (not graphics or audio): a game port may
 * provide it (weak; 1 = done) */
int n64rt_other_task(OSTask *t);
void n64rt_vi_field(void);                 /* a VI field elapsed (n64rt_tick) */
/* an RCP register the game reaches itself (the trap): 1 = done */
int n64rt_hw_read(u32 addr, u32 *v);
int n64rt_tlb_phys(u32 va, u32 *pa);       /* a page the game mapped (osMapTLB): 1, and its RDRAM address */
void n64rt_tlb_frames_loaded(u32 pa, u32 bytes);   /* the cartridge read into the RDRAM there: the TLB entries kept of those frames go (n64rt_os.c) */
int n64rt_hw_write(u32 addr, u32 v);

#endif
