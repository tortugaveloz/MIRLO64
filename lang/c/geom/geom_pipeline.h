/* Geometry-core vertex pipeline: walks a GDL display list, runs the matrix
 * stack / vertex transform / viewport map / lighting / fog (via the VPU),
 * clips, and streams N64 triangle commands to RDP. */
#ifndef GEOM_PIPELINE_H
#define GEOM_PIPELINE_H

#include <stdint.h>

/* Screen resolution the viewport/scissor default to (the render target). */
#ifndef GEOM_FB_HRES
#define GEOM_FB_HRES 268
#endif
#ifndef GEOM_FB_VRES
#define GEOM_FB_VRES 240
#endif

void geom_reset(void);

/* Walk the GDL at `dl` (word array in shared RAM). Emits triangles to RDP
 * as a side effect. */
void geom_run_display_list(const uint32_t *dl);

#endif /* GEOM_PIPELINE_H */
