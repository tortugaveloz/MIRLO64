/* Audio core address map (rtl/audio/AudioCore.v), as the audio core sees it;
 * the game CPU reaches it at 0x6000_0000 instead (game/audio_load.h). Shared by lang/c/audio and the game side
 * (lang/c/game/audio_load.c). */
#ifndef AUDIO_HW_H
#define AUDIO_HW_H

#include <stdint.h>

#define AUDIO_IMEM_BASE   0x80000000u   /* 8 KiB code: core fetch, SoC r/w   */
#define AUDIO_IMEM_SIZE   0x2000u
#define AUDIO_DMEM_BASE   0x80002000u   /* 8 KiB data/stack: core + SoC r/w  */
#define AUDIO_DMEM_SIZE   0x2000u
#define AUDIO_CTRL_BASE   0x80004000u
#define AUDIO_IMEM2_BASE  0x80006000u   /* 8 KiB more code: the second bank (.text2) */

#define AUDIO_RUN         (AUDIO_CTRL_BASE + 0x00u)  /* bit0: 1 = out of reset (SoC writes) */
#define AUDIO_CYCLES      (AUDIO_CTRL_BASE + 0x04u)  /* free-running sys-clock counter      */
#define AUDIO_MBOX(i)     (AUDIO_CTRL_BASE + 0x08u + 4u * (uint32_t)(i))   /* i = 0..7  */

/* MBOX0 is the core's state word; the trap handler (start.S) writes
 * AUDIO_ST_TRAP there and mcause/mepc/mtval to MBOX5..7. */
#define AUDIO_MBOX_STATE  0
#define AUDIO_ST_TRAP     0xA0D1DEADu

#endif
