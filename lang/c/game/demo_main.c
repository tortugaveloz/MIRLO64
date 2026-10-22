/* A spinning, Gouraud-shaded triangle: the smallest program that draws with
 * the geometry core and RDP.
 *
 * Every frame the game CPU writes a GDL (lang/c/geom/geom_gdl.h, built with
 * gdl_build.h): the projection and modelview matrices, the viewport, three
 * vertices and one triangle. frame_submit() hands it to the geom core, which
 * transforms, lights, clips and sets the triangle up for RDP; frames flip on
 * vblank. No vertex maths runs on this core.
 */
#include <stdint.h>
#include <stdio.h>
#include <generated/csr.h>

#include "frame.h"
#include "gdl_build.h"

/* 256-entry s8 sine, so this program needs no libm */
static const int8_t sintab[256] = {
    0,3,6,9,12,15,18,21,24,27,30,33,36,39,42,45,48,51,54,57,59,62,65,67,70,73,75,78,80,82,85,87,89,91,
    94,96,98,100,102,103,105,107,108,110,112,113,114,116,117,118,119,120,121,122,123,123,124,125,125,126,126,126,126,126,
    127,126,126,126,126,126,125,125,124,123,123,122,121,120,119,118,117,116,114,113,112,110,108,107,105,103,102,100,98,96,
    94,91,89,87,85,82,80,78,75,73,70,67,65,62,59,57,54,51,48,45,42,39,36,33,30,27,24,21,18,15,12,9,6,3,
    0,-3,-6,-9,-12,-15,-18,-21,-24,-27,-30,-33,-36,-39,-42,-45,-48,-51,-54,-57,-59,-62,-65,-67,-70,-73,-75,-78,-80,-82,-85,-87,-89,-91,
    -94,-96,-98,-100,-102,-103,-105,-107,-108,-110,-112,-113,-114,-116,-117,-118,-119,-120,-121,-122,-123,-123,-124,-125,-125,-126,-126,-126,-126,-126,
    -127,-126,-126,-126,-126,-126,-125,-125,-124,-123,-123,-122,-121,-120,-119,-118,-117,-116,-114,-113,-112,-110,-108,-107,-105,-103,-102,-100,-98,-96,
    -94,-91,-89,-87,-85,-82,-80,-78,-75,-73,-70,-67,-65,-62,-59,-57,-54,-51,-48,-45,-42,-39,-36,-33,-30,-27,-24,-21,-18,-15,-12,-9,
};
static float fsin(int a) { return (float)sintab[a & 0xFF] / 127.0f; }
static float fcos(int a) { return (float)sintab[(a + 64) & 0xFF] / 127.0f; }

int main(void)
{
    printf("Mirlo: spinning triangle\n");
    frame_init();

    /* Row-vector matrices (a vertex is transformed as v * M). Orthographic
     * projection: 80 units across the height of the screen. */
    static const float proj[16] = { 1.6f/80.0f,0,0,0,  0,2.0f/80.0f,0,0,  0,0,1.0f,0,  0,0,0,1 };
    const float vp[4] = { VIDEO_FRAMEBUFFER_HRES * 0.5f, VIDEO_FRAMEBUFFER_VRES * 0.5f, 0, 0 };

    /* object-space position, texture coordinates, RGBA */
    geom_vtx_t verts[3] = {
        {{   0,  22, 0}, 0, {0,0}, {255, 40,  40,  255}},
        {{ -22, -16, 0}, 0, {0,0}, {40,  255, 40,  255}},
        {{  22, -16, 0}, 0, {0,0}, {40,  40,  255, 255}},
    };

    int ang = 0;
    for (;;) {
        gdl_cur_t c;
        frame_begin(&c);

        float s = fsin(ang), co = fcos(ang);
        float mv[16] = { co, s, 0, 0,   -s, co, 0, 0,   0, 0, 1, 0,   0, 0, -80, 1 };

        gdl_mtx_load(&c, GDL_MTX_TARGET_PROJECTION, proj);
        gdl_mtx_load(&c, GDL_MTX_TARGET_MODELVIEW, mv);
        gdl_viewport(&c, vp, vp);
        gdl_geomode(&c, GDL_GEOMODE_NOCULL);   /* both faces: it spins */
        gdl_texnorm(&c, 0.0f);                 /* no texture */
        gdl_vtx(&c, 0, verts, 3);
        gdl_tri1(&c, 0, 1, 2);

        frame_submit(&c);
        ang += 2;
    }
    return 0;
}
