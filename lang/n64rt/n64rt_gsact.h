/* The codes the GameShark picker (n64rt_gspick.c) hands to the cheat engine (n64rt_cheat.c): the list it has just built from the player's choice, in SDRAM that nothing else uses
 * (layout.h, LAYOUT_GSP_ACTIVE), which n64rt_cheat_init() takes when the game's start sets the engine up. Nothing is read from the card for it: a game runs the codes of the picker
 * that was Started, and no code when there was no picker, or B was pressed, or no cheat was chosen. */
#ifndef N64RT_GSACT_H
#define N64RT_GSACT_H
#include <stdint.h>
#include <layout.h>
#include "n64rt_gs.h"

#define GSACT_MAGIC 0x47534143u          /* "GSAC" */
#define GSACT_MAX   2048u
typedef struct {
    uint32_t magic;                      /* GSACT_MAGIC when the list is there to be taken */
    uint32_t n;                          /* the codes */
    char     path[256];                  /* the game's .gs (the status file's name is made from it) */
    n64rt_gs_code_t code[GSACT_MAX];
} n64rt_gsact_t;
#define GSACT ((volatile n64rt_gsact_t *)(uintptr_t)LAYOUT_GSP_ACTIVE)
/* The game's .gs path to and from the list's `path` field. The field is in SDRAM that outlives the core's launches' leftovers and the games' (a Load Game does not clear it): a shorter path copied over a
 * longer one must not leave the longer one's tail after its end, so the field is cleared whole first (the status file of an earlier build was named after such a tail) and read with its last byte forced to 0. */
static inline void n64rt_gsact_set_path(volatile char *dst, const char *src)
{
    for (unsigned i = 0; i < 256u; i++) dst[i] = 0;
    for (unsigned i = 0; i < 255u && src[i]; i++) dst[i] = src[i];
}
static inline unsigned n64rt_gsact_get_path(char *dst, const volatile char *src)       /* `dst`: 256 bytes; the length */
{
    unsigned i = 0;
    for (; i < 255u && src[i]; i++) dst[i] = src[i];
    for (unsigned k = i; k < 256u; k++) dst[k] = 0;
    return i;
}
_Static_assert(LAYOUT_GSP_ACTIVE + sizeof(n64rt_gsact_t) <= LAYOUT_GSP_BLOB, "the active list fits below the database's block");
#endif
