#include "SDL_config.h"
#include "SDL_stdinc.h"

#include <libdrm/drm_fourcc.h>

#define KMSDRM_DEFAULT_COLOR_DEPTH 16
typedef struct drm_color_def {
    Uint32 four_cc;
    Uint32 bpp;
    Uint32 r_mask,  g_mask,  b_mask,  a_mask;
    Uint32 r_bits,  g_bits,  b_bits,  a_bits;
    Uint32 r_shift, g_shift, b_shift, a_shift;
} drm_color_def;

#define MAKE_RGBA(type, bpp, rbits, gbits, bbits, abits, rsh, gsh, bsh, ash) \
    drm_color_def KMSDRM_COLOR_##type = { \
        DRM_FORMAT_##type, bpp, \
        (0xFF >> (8-rbits)) << rsh, (0xFF >> (8-gbits)) << gsh, \
        (0xFF >> (8-bbits)) << bsh, (0xFF >> (8-abits)) << ash, \
        rbits, gbits, bbits, abits, rsh, gsh, bsh, ash, \
    }

/*       |   CODE |BPP| R| G| B| A| RS| GS| BS| AS| */
MAKE_RGBA(XRGB8888, 32, 8, 8, 8, 0, 16,  8,  0,  0);
MAKE_RGBA(RGB565,   16, 5, 6, 5, 0, 11,  5,  0,  0);
MAKE_RGBA(XRGB1555, 16, 5, 5, 5, 0, 10,  5,  0,  0);
MAKE_RGBA(XBGR8888, 32, 8, 8, 8, 0,  0,  8, 16,  0);
MAKE_RGBA(BGR565,   16, 5, 6, 5, 0,  0,  5, 11,  0);
MAKE_RGBA(XBGR1555, 16, 5, 5, 5, 0,  0,  5, 10,  0);

/* Provides information on how to configure color format. */
const drm_color_def *get_drm_color_def(int *depth, int isyuv, Uint32 flags)
{
    /** 
     * TODO:: Implement SDL_BGRSWIZZLE, implement YUV. Until then, isyuv is left
     * unused.
     **/
    if (flags & SDL_SWIZZLEBGR) {
        switch(*depth) {
        /**
         * Currently unimplemented - set bpp to 16, fallthrough and let the
         * shadow surface deal with it
         **/
        case 8: *depth = 16;
        case 16: return &KMSDRM_COLOR_BGR565;
        case 15: return &KMSDRM_COLOR_XBGR1555;
        case 24:
        case 32: return &KMSDRM_COLOR_XBGR8888;
        default: return NULL;
        }
    } else {
        switch(*depth) {
        /**
         * Currently unimplemented - set bpp to 16, fallthrough and let the
         * shadow surface deal with it
         **/
        case 8: *depth = 16;
        case 16: return &KMSDRM_COLOR_RGB565;
        case 15: return &KMSDRM_COLOR_XRGB1555;
        case 24:
        case 32: return &KMSDRM_COLOR_XRGB8888;
        default: return NULL;
        }
    }
}

void get_framebuffer_args(const drm_color_def *def, unsigned int handle, unsigned int pitch,
    Uint32 *handles, Uint32 *pitches, Uint32 *offsets)
{
    switch (def->four_cc)
    {
        case DRM_FORMAT_RGB565:
        case DRM_FORMAT_XRGB1555:
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_BGR565:
        case DRM_FORMAT_XBGR1555:
        case DRM_FORMAT_XBGR8888:
            handles[0] = handle;
            pitches[0] = pitch;
            break;
    }
}

/* Pick a bpp value that is appropriate for drm_mode_create_dumb. */
int get_rounded_bpp(int depth, int def)
{
    switch(depth) {
    case  8: return def; /** TODO:: Palletized color depth support **/ 
    case 15:
    case 16: return 16;
    case 24:
    case 32: return 32;
    default: return 0;
    }
}