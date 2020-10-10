#include "SDL_kmsdrmvideo.h"

#ifndef _SDL_kmsdrmmisc_h
#define _SDL_kmsdrmmisc_h

void dump_mode(drmModeModeInfo *mode);
int save_drm_pipe(_THIS, Uint32 plane, Uint32 crtc, Uint32 enc, drmModeConnector *conn);
Uint32 get_prop_id(_THIS, Uint32 obj_id, const char *prop_name);
int acquire_properties(_THIS, Uint32 id, Uint32 type);
int get_property(_THIS, uint32_t obj_id, const char *name, uint64_t *value);
int add_property(_THIS, drmModeAtomicReq *req, uint32_t obj_id, const char *name, int opt, uint64_t value);
int find_property(_THIS, uint32_t obj_id, const char *name);
int free_drm_prop_storage(_THIS);
int free_drm_pipe(_THIS);
drmModeModeInfo *find_pipe_closest_refresh(drm_pipe *pipe, float refresh);

#endif /* _SDL_kmsdrmmisc_h */
