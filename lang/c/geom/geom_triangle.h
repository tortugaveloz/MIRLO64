/* Screen-space triangle state shared by the geom pipeline and RDP's
 * triangle setup (geom_rdp.h): fixed-point vertices, the edge set of a
 * triangle, the scissor, and back-face culling. */
#ifndef GEOM_TRIANGLE_H
#define GEOM_TRIANGLE_H

#include <stdbool.h>
#include <stdint.h>

#define GEOM_TMU_COUNT 1

/* The scissor, in edge-function fixed point (pixels << 5). */
typedef struct {
    int32_t scissor_start_x_fx, scissor_start_y_fx;
    int32_t scissor_end_x_fx, scissor_end_y_fx;
    bool    scissor_enable;
    bool    tmu_enable[GEOM_TMU_COUNT];
} geom_raster_state_t;

void geom_raster_set_scissor(geom_raster_state_t *st, int32_t x, int32_t y,
                             uint32_t w, uint32_t h);

/* A screen-space vertex, fixed point: x, y in 1/32 pixel; z, w, colour and
 * texture coordinates S15.16. */
typedef struct {
    int32_t x, y, z, w;
    int32_t r, g, b, a;
    int32_t s, t, q;
} geom_vertex_fx_t;

/* A triangle's edges: its vertices in winding order, the winding's sign and
 * twice its area. */
typedef struct { int32_t v0x, v0y, v1x, v1y, v2x, v2y, sign, area; } geom_edges_t;

extern int g_geom_cull;   /* back-face culling on (GDL_GEOMODE) -- set it with: */
void geom_set_cull(int on);

#endif /* GEOM_TRIANGLE_H */
