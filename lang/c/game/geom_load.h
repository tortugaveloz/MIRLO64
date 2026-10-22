/* Game-CPU side of the geom core's firmware: each game brings its own build
 * of lang/c/geom (with its display-list translator) and loads it at boot into
 * the core's two ROMs. Nothing of it
 * is in the bitstream: the core stays in reset (MAILBOX.GEOM_RUN = 0) until
 * geom_start(). The images: lang/c/geom's $(B)/geom_fw.h. */
#ifndef GEOM_LOAD_H
#define GEOM_LOAD_H

#include <stdint.h>

/* the ROMs as the game CPU writes them (rtl/soc/mirlo_mips.sv: word writes
 * only, while the core is in reset; they do not read back) */
#define GEOM_ROM_A_BASE 0x20000000u        /* 16 KiB: code + read-only data + .data's image */
#define GEOM_ROM_B_BASE 0x20010000u        /* 64 KiB: code */
/* the triangles the last display list sent to RDP (lang/c/geom: the first
 * word of its RAM, geom_mips.ld .shared) */
#define GEOM_TRIS_EMITTED_ADDR 0x20008000u

/* Holds the core in reset and writes both images. */
void geom_load(const uint32_t *rom_a, uint32_t rom_a_words,
               const uint32_t *rom_b, uint32_t rom_b_words);
/* Releases the core and waits for its firmware's "waiting for a display list"
 * heartbeat (lang/c/geom/main.c). Publish anything the firmware reads at boot
 * (geom_cfg.h) first. Returns 0, or the last heartbeat if it never came. */
uint32_t geom_start(void);

#endif
