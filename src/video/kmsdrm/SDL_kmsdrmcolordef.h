#include "SDL_config.h"
#include "SDL_stdinc.h"

#include <libdrm/drm_fourcc.h>

typedef struct drm_color_def {
    Uint32 four_cc;
    Uint32 handles[4];
    Uint32 pitches[4];
    Uint32 offsets[4];
    Uint32 r_mask,  g_mask,  b_mask,  a_mask;
    Uint32 r_bits,  g_bits,  b_bits,  a_bits;
    Uint32 r_shift, g_shift, b_shift, a_shift;
} drm_color_def;

#define MAKE_RGBA(type, rbits, gbits, bbits, abits, rsh, gsh, bsh, ash) \
    drm_color_def KMSDRM_COLOR_##type = { \
        DRM_FORMAT_##type, {}, {}, {}, \
        (0xFF >> (8-rbits)) << rsh, (0xFF >> (8-gbits)) << gsh, \
        (0xFF >> (8-bbits)) << bsh, (0xFF >> (8-abits)) << ash, \
        rbits, gbits, bbits, abits, rsh, gsh, bsh, ash, \
    }

/*       |   CODE | R| G| B| A| RS| GS| BS| AS| */
MAKE_RGBA(XRGB8888, 8, 8, 8, 0, 16,  8,  0,  0);
MAKE_RGBA(RGB565,   5, 6, 5, 0, 11,  5,  0,  0);
MAKE_RGBA(XRGB1555, 5, 5, 5, 0, 11,  5,  0,  0);

/* Provides information on how to configure color format. */
int get_drm_color_def(drm_color_def *dst, int depth, int isyuv, Uint32 handle, Uint32 pitch, Uint32 flags)
{
    /** 
     * TODO:: Implement SDL_BGRSWIZZLE, implement YUV. Until then, isyuv is left
     * unused.
     **/
    const drm_color_def *src = NULL;
    switch(depth) {
    case 0:
    case 16: src = &KMSDRM_COLOR_RGB565; break;
    case 15: src = &KMSDRM_COLOR_XRGB1555; break;
    case 24:
    case 32: src = &KMSDRM_COLOR_XRGB8888; break;
    default: return 0;
    }

    memcpy(dst, src, sizeof(*dst));
    /**
     * Note: This is hardcoded right now for RGB/XRGB formats, and will need to
     * be computed in case of YUV surfaces.
     **/
    dst->handles[0] = handle;
    dst->pitches[0] = pitch;
    return 1;
}

/* Pick a bpp value that is appropriate for drm_mode_create_dumb. */
int get_rounded_bpp(int depth)
{
    switch(depth) {
    //case  8: return 8;
    case  0:
    case 15:
    case 16: return 16;
    case 24:
    case 32: return 32;
    default: return 0;
    }
}